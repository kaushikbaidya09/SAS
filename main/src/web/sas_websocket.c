//=============================================================================
/*!
    \file    sas_websocket.c
    \brief   Implementation of the WebSocket module.
*/
//=============================================================================

// ESP-IDF's httpd answers the WebSocket upgrade itself without calling the URI
// handler, so clients are registered from the post-handshake callback
// (CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT).
//
// Events are published from other tasks, never the httpd task, so frames are
// not sent directly. Each message is copied into a fixed-size queue per client
// and sent later from the httpd task through httpd_queue_work().

#include "sas_websocket.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sas_http_server.h"

#include "../auth/sas_auth.h"
#include "../core/sas_event_bus.h"
#include "../core/sas_logger.h"
#include "../core/sas_sysinfo.h"
#include "../network/sas_ble_scanner.h"
#include "../network/sas_ota_manager.h"
#include "../network/sas_wifi_manager.h"

static const char *TAG = "sas_ws";

/*! \brief Bounded wait for s_table_lock; critical sections are short array scans/ ring pushes. */
#define SAS_WS_LOCK_WAIT_TICKS   pdMS_TO_TICKS(50)
#define SAS_WS_URI               "/ws" ///< Sole WebSocket endpoint registered with the HTTP Server.
#define SAS_WS_RX_MAX_LEN        (512U) ///< Largest client-to-device frame accepted, including the NUL.
#define SAS_WS_TOKEN_LEN         (33U)        ///< Session token buffer: 32 hex digits plus NUL.
#define SAS_WS_SYSINFO_PERIOD_US (2000000ULL) ///< Interval of the periodic "sysinfo" broadcast.

/*! \brief One queued outbound message, copied in full (no dynamic allocation). */
typedef struct
{
    char text[SAS_WS_MESSAGE_MAX_LEN]; ///< Serialized "{ type, data }" envelope.
    size_t len;                        ///< strlen(text).
} sas_ws_queued_msg_t;

/*! \brief One tracked client connection and its outbound ring. */
typedef struct
{
    bool in_use;                  ///< false if this slot is free.
    int fd;                       ///< Underlying socket descriptor.
    bool authenticated;           ///< Presented a valid token on the upgrade request.
    char token[SAS_WS_TOKEN_LEN]; ///< That token; empty if none was presented.
    sas_ws_queued_msg_t ring[SAS_WS_CLIENT_SEND_QUEUE_LEN]; ///< Circular outbound queue.
    uint8_t ring_head;                                      ///< Index of the oldest queued message.
    uint8_t ring_count;                                     ///< Number of valid entries in ring.
} sas_ws_slot_t;

static bool s_initialized = false;
static bool s_started     = false;

static sas_ws_slot_t s_clients[SAS_WS_MAX_CLIENTS];

static SemaphoreHandle_t s_table_lock = NULL;
static StaticSemaphore_t s_table_lock_storage;

static uint32_t s_wifi_state_sub   = 0U;
static uint32_t s_ota_progress_sub = 0U;
static uint32_t s_ble_device_sub   = 0U;
static uint32_t s_log_entry_sub    = 0U;

static esp_timer_handle_t s_sysinfo_timer = NULL;

/*! \brief One claimed client-to-device message type. */
static struct
{
    const char *type;          ///< Envelope "type" value; NULL if this row is free.
    sas_ws_message_fn handler; ///< Handler invoked for it.
} s_message_handlers[SAS_WS_MAX_MESSAGE_HANDLERS];

static char s_rx_buf[SAS_WS_RX_MAX_LEN]; ///< Inbound frame buffer; httpd task only.

// Returns esp_err_t because ESP-IDF requires it of HTTP handlers.
static esp_err_t ws_handler(httpd_req_t *req);
static esp_err_t ws_post_handshake(httpd_req_t *req);
static void register_client(int fd, const char *token);
static void remove_client(int fd);
static void dispatch_client_message(int fd, const char *text);
static bool type_is_public(sas_ws_msg_type_t type);
static const char *msg_type_name(sas_ws_msg_type_t type);
static const char *ota_state_name(sas_ota_state_t state);
static void sysinfo_timer_callback(void *arg);
static void on_ota_progress(const sas_event_t *event, void *user_ctx);
static void async_drain_work(void *arg);
static void enqueue_for_client(size_t slot_index, const char *text, size_t len);
static sas_status_t build_and_enqueue(int target_fd_or_broadcast, sas_ws_msg_type_t type,
                                      cJSON *payload);
static const char *wifi_state_name(sas_wifi_state_t state);
static const char *log_level_name(sas_log_level_t level);
static void format_mac(const uint8_t mac[6], char *out, size_t out_len);
static void on_wifi_state_changed(const sas_event_t *event, void *user_ctx);
static void on_ble_device_discovered(const sas_event_t *event, void *user_ctx);
static void on_log_entry(const sas_event_t *event, void *user_ctx);

/*!
    \brief   Initialize the WebSocket module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already
             initialized.
*/
sas_status_t sas_ws_init(void)
{
    if (s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    (void)memset(s_clients, 0, sizeof(s_clients));
    (void)memset(s_message_handlers, 0, sizeof(s_message_handlers));

    s_table_lock = xSemaphoreCreateMutexStatic(&s_table_lock_storage);
    configASSERT(s_table_lock != NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = sysinfo_timer_callback,
        .name     = "sas_ws_sysinfo",
    };
    if (esp_timer_create(&timer_args, &s_sysinfo_timer) != ESP_OK)
    {
        s_sysinfo_timer = NULL;
        sas_log_warn("sysinfo broadcast timer unavailable");
    }

    s_started     = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Start active WebSocket operation: register the URI handler with
             the HTTP Server and subscribe to broadcastable Event Bus events.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_ws_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    httpd_uri_t ws_uri = {
        .uri                  = SAS_WS_URI,
        .method               = HTTP_GET,
        .handler              = ws_handler,
        .user_ctx             = NULL,
        .is_websocket         = true,
        .ws_post_handshake_cb = ws_post_handshake,
    };
    sas_status_t status = sas_http_register_ws_handler(SAS_WS_URI, &ws_uri);
    if ((status != SAS_STATUS_OK) && (status != SAS_HTTP_ERR_ROUTE_CONFLICT))
    {
        return status;
    }

    (void)sas_event_bus_subscribe(SAS_EVENT_WIFI_STATE_CHANGED, on_wifi_state_changed, NULL,
                                  &s_wifi_state_sub);
    (void)sas_event_bus_subscribe(SAS_EVENT_OTA_PROGRESS, on_ota_progress, NULL,
                                  &s_ota_progress_sub);
    (void)sas_event_bus_subscribe(SAS_EVENT_BLE_DEVICE_DISCOVERED, on_ble_device_discovered, NULL,
                                  &s_ble_device_sub);
    (void)sas_event_bus_subscribe(SAS_EVENT_LOG_ENTRY, on_log_entry, NULL, &s_log_entry_sub);

    if (s_sysinfo_timer != NULL)
    {
        (void)esp_timer_start_periodic(s_sysinfo_timer, SAS_WS_SYSINFO_PERIOD_US);
    }

    s_started = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Stop active WebSocket operation without releasing resources.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_ws_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_sysinfo_timer != NULL)
    {
        (void)esp_timer_stop(s_sysinfo_timer);
    }

    (void)sas_event_bus_unsubscribe(s_wifi_state_sub);
    (void)sas_event_bus_unsubscribe(s_ota_progress_sub);
    (void)sas_event_bus_unsubscribe(s_ble_device_sub);
    (void)sas_event_bus_unsubscribe(s_log_entry_sub);

    s_started = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Release all resources acquired by sas_ws_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_ws_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_sysinfo_timer != NULL)
    {
        (void)esp_timer_delete(s_sysinfo_timer);
        s_sysinfo_timer = NULL;
    }
    if (s_table_lock != NULL)
    {
        vSemaphoreDelete(s_table_lock);
        s_table_lock = NULL;
    }
    (void)memset(s_clients, 0, sizeof(s_clients));
    (void)memset(s_message_handlers, 0, sizeof(s_message_handlers));

    s_initialized = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Send a JSON envelope { "type": ..., "data": payload } to every
             currently connected client.
    \param[in] type     Message category carried in the envelope's "type"
                        field.
    \param[in] payload  Non-NULL cJSON payload; ownership remains with the
                        caller, this call does not delete it.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if payload is NULL;
             SAS_ERR_INVALID_STATE if not started; SAS_ERR_NO_MEM if the
             serialized envelope exceeds SAS_WS_MESSAGE_MAX_LEN.
*/
sas_status_t sas_ws_broadcast(sas_ws_msg_type_t type, cJSON *payload)
{
    if (payload == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    return build_and_enqueue(-1, type, payload);
}

/*!
    \brief   Send a JSON envelope { "type": ..., "data": payload } to a single
             connected client, e.g. for per-session Terminal output.
    \param[in] client   Non-NULL descriptor of a currently connected client.
    \param[in] type     Message category carried in the envelope's "type"
                        field.
    \param[in] payload  Non-NULL cJSON payload; ownership remains with the
                        caller, this call does not delete it.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if client or payload
             is NULL; SAS_ERR_INVALID_STATE if not started; SAS_ERR_NO_MEM if
             the serialized envelope exceeds SAS_WS_MESSAGE_MAX_LEN.
*/
sas_status_t sas_ws_send_to_client(const sas_ws_client_t *client, sas_ws_msg_type_t type,
                                   cJSON *payload)
{
    if ((client == NULL) || (payload == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    return build_and_enqueue(client->fd, type, payload);
}

/*!
    \brief   Claim a client-to-device message type: every received { "type":
             type, "data": ... } envelope is passed to handler.
    \param[in] type     Non-NULL message type with static storage duration
                        (stored by pointer), e.g. "terminal_cmd".
    \param[in] handler  Non-NULL handler.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if type or handler
             is NULL; SAS_ERR_NO_MEM if SAS_WS_MAX_MESSAGE_HANDLERS types are
             already claimed.
*/
sas_status_t sas_ws_register_message_handler(const char *type, sas_ws_message_fn handler)
{
    if ((type == NULL) || (handler == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    for (size_t i = 0U; i < SAS_WS_MAX_MESSAGE_HANDLERS; i++)
    {
        if ((s_message_handlers[i].type == NULL) || (strcmp(s_message_handlers[i].type, type) == 0))
        {
            s_message_handlers[i].handler = handler;
            s_message_handlers[i].type    = type;
            return SAS_STATUS_OK;
        }
    }
    return SAS_ERR_NO_MEM;
}

/*!
    \brief   Copy out the session token a client presented on its upgrade
             request.
    \param[in]  client   Non-NULL descriptor of a connected client.
    \param[out] out      Non-NULL destination; receives an empty string if the
                         client presented no token.
    \param[in]  out_len  Capacity of out in bytes (33 holds any token).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if client or out is
             NULL or out_len is 0; SAS_ERR_INVALID_STATE if client is not
             connected.
*/
sas_status_t sas_ws_get_client_token(const sas_ws_client_t *client, char *out, size_t out_len)
{
    if ((client == NULL) || (out == NULL) || (out_len == 0U))
    {
        return SAS_ERR_INVALID_ARG;
    }

    sas_status_t status = SAS_ERR_INVALID_STATE;
    if (s_initialized && (xSemaphoreTake(s_table_lock, SAS_WS_LOCK_WAIT_TICKS) == pdTRUE))
    {
        for (size_t i = 0U; i < SAS_WS_MAX_CLIENTS; i++)
        {
            if (s_clients[i].in_use && (s_clients[i].fd == client->fd))
            {
                (void)strncpy(out, s_clients[i].token, out_len - 1U);
                out[out_len - 1U] = '\0';
                status            = SAS_STATUS_OK;
                break;
            }
        }
        xSemaphoreGive(s_table_lock);
    }
    return status;
}

/*!
    \brief   Build the { "type", "data" } envelope once and enqueue it for one
             client (target_fd >= 0) or every connected client (target_fd < 0),
             scheduling an async drain for each.
*/
static sas_status_t build_and_enqueue(int target_fd_or_broadcast, sas_ws_msg_type_t type,
                                      cJSON *payload)
{
    cJSON *envelope = cJSON_CreateObject();
    if (envelope == NULL)
    {
        return SAS_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(envelope, "type", msg_type_name(type));
    cJSON_AddItemReferenceToObject(envelope, "data", payload);

    char *text = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    if (text == NULL)
    {
        return SAS_ERR_NO_MEM;
    }
    size_t len = strlen(text);
    if (len >= SAS_WS_MESSAGE_MAX_LEN)
    {
        // Truncating would hand the client malformed JSON; drop the message instead.
        cJSON_free(text);
        return SAS_ERR_NO_MEM;
    }

    // With authentication enabled, a broadcast of a non-public message type
    // only reaches clients that presented a valid token. A direct send is not
    // filtered.
    bool restrict_to_authenticated =
        (target_fd_or_broadcast < 0) && !type_is_public(type) && sas_auth_is_enabled();

    if (xSemaphoreTake(s_table_lock, SAS_WS_LOCK_WAIT_TICKS) == pdTRUE)
    {
        for (size_t i = 0U; i < SAS_WS_MAX_CLIENTS; i++)
        {
            if (!s_clients[i].in_use)
            {
                continue;
            }
            if ((target_fd_or_broadcast >= 0) && (s_clients[i].fd != target_fd_or_broadcast))
            {
                continue;
            }
            if (restrict_to_authenticated && !s_clients[i].authenticated)
            {
                continue;
            }
            enqueue_for_client(i, text, len);
        }
        xSemaphoreGive(s_table_lock);
    }

    cJSON_free(text);
    return SAS_STATUS_OK;
}

/*!
    \brief   Push text onto s_clients[slot_index]'s ring (dropping the oldest
             entry if full), then schedule its async drain. Caller holds
             s_table_lock.
*/
static void enqueue_for_client(size_t slot_index, const char *text, size_t len)
{
    sas_ws_slot_t *slot = &s_clients[slot_index];
    size_t copy_len = (len < (SAS_WS_MESSAGE_MAX_LEN - 1U)) ? len : (SAS_WS_MESSAGE_MAX_LEN - 1U);

    uint8_t write_index;
    if (slot->ring_count < SAS_WS_CLIENT_SEND_QUEUE_LEN)
    {
        write_index =
            (uint8_t)((slot->ring_head + slot->ring_count) % SAS_WS_CLIENT_SEND_QUEUE_LEN);
        slot->ring_count++;
    }
    else
    {
        write_index     = slot->ring_head; // full: overwrite the oldest message
        slot->ring_head = (uint8_t)((slot->ring_head + 1U) % SAS_WS_CLIENT_SEND_QUEUE_LEN);
    }
    (void)memcpy(slot->ring[write_index].text, text, copy_len);
    slot->ring[write_index].text[copy_len] = '\0';
    slot->ring[write_index].len            = copy_len;

    httpd_handle_t server = sas_http_get_server_handle();
    if (server != NULL)
    {
        (void)httpd_queue_work(server, async_drain_work, (void *)(intptr_t)slot->fd);
    }
}

/*!
    \brief   httpd-task-context work item: drain every currently queued message
             for the client identified by fd (passed by value via arg) and send
             each as a WebSocket text frame. Runs on the httpd task, so
             httpd_ws_send_frame_async() is safe here regardless of which task
             originally enqueued the message.
*/
static void async_drain_work(void *arg)
{
    // A client whose send fails is treated as disconnected and removed;
    // delivery to the other clients continues.

    int fd = (int)(intptr_t)arg;

    for (;;)
    {
        sas_ws_queued_msg_t msg;
        bool have_msg             = false;
        size_t slot_index         = 0U;
        bool client_still_present = false;

        if (xSemaphoreTake(s_table_lock, SAS_WS_LOCK_WAIT_TICKS) == pdTRUE)
        {
            for (size_t i = 0U; i < SAS_WS_MAX_CLIENTS; i++)
            {
                if (s_clients[i].in_use && (s_clients[i].fd == fd))
                {
                    slot_index           = i;
                    client_still_present = true;
                    if (s_clients[i].ring_count > 0U)
                    {
                        msg = s_clients[i].ring[s_clients[i].ring_head];
                        s_clients[i].ring_head =
                            (uint8_t)((s_clients[i].ring_head + 1U) % SAS_WS_CLIENT_SEND_QUEUE_LEN);
                        s_clients[i].ring_count--;
                        have_msg = true;
                    }
                    break;
                }
            }
            xSemaphoreGive(s_table_lock);
        }

        if (!client_still_present || !have_msg)
        {
            break;
        }

        httpd_ws_frame_t frame;
        (void)memset(&frame, 0, sizeof(frame));
        frame.type    = HTTPD_WS_TYPE_TEXT;
        frame.payload = (uint8_t *)msg.text;
        frame.len     = msg.len;

        // The fd-type check keeps a stale slot from writing a WebSocket frame into a socket the
        // server has since reused for a plain HTTP connection.
        httpd_handle_t server = sas_http_get_server_handle();
        if ((server == NULL) || (httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) ||
            (httpd_ws_send_frame_async(server, fd, &frame) != ESP_OK))
        {
            if (xSemaphoreTake(s_table_lock, SAS_WS_LOCK_WAIT_TICKS) == pdTRUE)
            {
                s_clients[slot_index].in_use = false;
                xSemaphoreGive(s_table_lock);
            }
            break;
        }
    }
}

/*!
    \brief   ws_post_handshake_cb for SAS_WS_URI: runs once per client, right
             after httpd has completed the upgrade. Tracks the client and
             records the session token from its ?token= query parameter.
*/
static esp_err_t ws_post_handshake(httpd_req_t *req)
{
    char query[96];
    char token[SAS_WS_TOKEN_LEN] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK)
        {
            token[0] = '\0';
        }
    }

    register_client(httpd_req_to_sockfd(req), token);
    return ESP_OK;
}

/*!
    \brief   Track fd in the client table, replacing any stale slot that still
             names it.
*/
static void register_client(int fd, const char *token)
{
    bool authenticated = (token[0] != '\0') && (sas_auth_validate_token(token) == SAS_STATUS_OK);
    bool tracked       = false;

    if (xSemaphoreTake(s_table_lock, SAS_WS_LOCK_WAIT_TICKS) == pdTRUE)
    {
        size_t slot_index = SAS_WS_MAX_CLIENTS;
        for (size_t i = 0U; i < SAS_WS_MAX_CLIENTS; i++)
        {
            if (s_clients[i].in_use && (s_clients[i].fd == fd))
            {
                slot_index = i; // fd reused by a new connection
                break;
            }
            if (!s_clients[i].in_use && (slot_index == SAS_WS_MAX_CLIENTS))
            {
                slot_index = i;
            }
        }
        if (slot_index < SAS_WS_MAX_CLIENTS)
        {
            (void)memset(&s_clients[slot_index], 0, sizeof(s_clients[slot_index]));
            s_clients[slot_index].in_use        = true;
            s_clients[slot_index].fd            = fd;
            s_clients[slot_index].authenticated = authenticated;
            if (authenticated)
            {
                (void)strncpy(s_clients[slot_index].token, token, SAS_WS_TOKEN_LEN - 1U);
            }
            tracked = true;
        }
        xSemaphoreGive(s_table_lock);
    }

    if (!tracked)
    {
        // The connection stays open but receives nothing; it can still send commands.
        sas_log_warn("SAS_WS_MAX_CLIENTS reached (0x%x), fd %d not tracked",
                     (unsigned int)SAS_WS_ERR_CLIENT_TABLE_FULL, fd);
    }
}

/*!
    \brief   Release fd's slot and its queued messages, if tracked.
*/
static void remove_client(int fd)
{
    if (xSemaphoreTake(s_table_lock, SAS_WS_LOCK_WAIT_TICKS) == pdTRUE)
    {
        for (size_t i = 0U; i < SAS_WS_MAX_CLIENTS; i++)
        {
            if (s_clients[i].in_use && (s_clients[i].fd == fd))
            {
                (void)memset(&s_clients[i], 0, sizeof(s_clients[i]));
                break;
            }
        }
        xSemaphoreGive(s_table_lock);
    }
}

/*!
    \brief   Registered WebSocket handler for SAS_WS_URI: called for every
             frame a client sends after the upgrade.
*/
static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    httpd_ws_frame_t frame;
    (void)memset(&frame, 0, sizeof(frame));
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0); // length/type only
    if (ret != ESP_OK)
    {
        remove_client(fd);
        return ret;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE)
    {
        remove_client(fd);
        return ESP_OK;
    }

    if (frame.len >= sizeof(s_rx_buf))
    {
        // Too large to buffer, and leaving it unread would desynchronize framing: close.
        sas_log_warn("oversized frame (%u bytes) from fd %d, closing", (unsigned int)frame.len, fd);
        remove_client(fd);
        return ESP_FAIL;
    }

    if (frame.len > 0U)
    {
        frame.payload = (uint8_t *)s_rx_buf;
        ret           = httpd_ws_recv_frame(req, &frame, sizeof(s_rx_buf) - 1U);
        if (ret != ESP_OK)
        {
            remove_client(fd);
            return ret;
        }
    }
    s_rx_buf[frame.len] = '\0';

    if (frame.type == HTTPD_WS_TYPE_TEXT)
    {
        dispatch_client_message(fd, s_rx_buf);
    }
    return ESP_OK;
}

/*!
    \brief   Parse a client-to-device { "type", "data" } envelope and hand it
             to the module that claimed its type; unknown types and malformed
             JSON are ignored.
*/
static void dispatch_client_message(int fd, const char *text)
{
    cJSON *envelope = cJSON_Parse(text);
    if (envelope == NULL)
    {
        return;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(envelope, "type");
    if (cJSON_IsString(type) && (type->valuestring != NULL))
    {
        for (size_t i = 0U; i < SAS_WS_MAX_MESSAGE_HANDLERS; i++)
        {
            if ((s_message_handlers[i].type != NULL) &&
                (strcmp(s_message_handlers[i].type, type->valuestring) == 0))
            {
                const sas_ws_client_t client = {.fd = fd};
                s_message_handlers[i].handler(&client,
                                              cJSON_GetObjectItemCaseSensitive(envelope, "data"));
                break;
            }
        }
    }
    cJSON_Delete(envelope);
}

/*!
    \brief   True for the message types an unauthenticated client may receive:
             the ones the Home view shows without a login.
*/
static bool type_is_public(sas_ws_msg_type_t type)
{
    return (type == SAS_WS_MSG_SYSINFO) || (type == SAS_WS_MSG_WIFI_STATE);
}

/*!
    \brief   Map sas_ws_msg_type_t to the envelope's "type" string.
*/
static const char *msg_type_name(sas_ws_msg_type_t type)
{
    switch (type)
    {
        case SAS_WS_MSG_SYSINFO:
            return "sysinfo";
        case SAS_WS_MSG_WIFI_STATE:
            return "wifi_state";
        case SAS_WS_MSG_OTA_PROGRESS:
            return "ota_progress";
        case SAS_WS_MSG_BLE_DEVICE:
            return "ble_device";
        case SAS_WS_MSG_TERMINAL_OUTPUT:
            return "terminal_output";
        case SAS_WS_MSG_LOG_ENTRY:
            return "log_entry";
        default:
            return "unknown";
    }
}

/*!
    \brief   Map sas_ota_state_t to its JSON field string.
*/
static const char *ota_state_name(sas_ota_state_t state)
{
    switch (state)
    {
        case SAS_OTA_STATE_IDLE:
            return "idle";
        case SAS_OTA_STATE_RECEIVING:
            return "receiving";
        case SAS_OTA_STATE_VERIFYING:
            return "verifying";
        case SAS_OTA_STATE_READY_TO_REBOOT:
            return "ready_to_reboot";
        case SAS_OTA_STATE_FAILED:
        default:
            return "failed";
    }
}

/*!
    \brief   Periodic timer: broadcast System Information's runtime statistics
             as "sysinfo".
*/
static void sysinfo_timer_callback(void *arg)
{
    (void)arg;

    sas_runtime_info_t info;
    if (!s_started || (sas_sysinfo_get_runtime_info(&info) != SAS_STATUS_OK))
    {
        return;
    }

    cJSON *data = cJSON_CreateObject();
    if (data == NULL)
    {
        return;
    }
    cJSON_AddNumberToObject(data, "free_heap_bytes", (double)info.free_heap_bytes);
    cJSON_AddNumberToObject(data, "min_free_heap_bytes", (double)info.min_free_heap_bytes);
    cJSON_AddNumberToObject(data, "uptime_seconds", (double)info.uptime_seconds);
    (void)sas_ws_broadcast(SAS_WS_MSG_SYSINFO, data);
    cJSON_Delete(data);
}

/*!
    \brief   Map sas_wifi_state_t to its JSON field string.
*/
static const char *wifi_state_name(sas_wifi_state_t state)
{
    switch (state)
    {
        case SAS_WIFI_STATE_IDLE:
            return "idle";
        case SAS_WIFI_STATE_CONNECTING:
            return "connecting";
        case SAS_WIFI_STATE_CONNECTED:
            return "connected";
        case SAS_WIFI_STATE_RECONNECTING:
            return "reconnecting";
        case SAS_WIFI_STATE_AP_FALLBACK:
            return "ap_fallback";
        default:
            return "unknown";
    }
}

/*!
    \brief   Map sas_log_level_t to its JSON field string; mirrors
             sas_rest_api.c's log_level_name(), kept private to each
             translation unit rather than shared, per this project's
             one-.c/.h-per- module rule.
*/
static const char *log_level_name(sas_log_level_t level)
{
    switch (level)
    {
        case SAS_LOG_LEVEL_ERROR:
            return "ERROR";
        case SAS_LOG_LEVEL_WARN:
            return "WARN";
        case SAS_LOG_LEVEL_INFO:
            return "INFO";
        case SAS_LOG_LEVEL_DEBUG:
            return "DEBUG";
        case SAS_LOG_LEVEL_VERBOSE:
        default:
            return "VERBOSE";
    }
}

/*!
    \brief   Format a 6-byte MAC as "AA:BB:CC:DD:EE:FF" into out (out_len >=
             18).
*/
static void format_mac(const uint8_t mac[6], char *out, size_t out_len)
{
    (void)snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                   mac[4], mac[5]);
}

/*!
    \brief   SAS_EVENT_WIFI_STATE_CHANGED subscriber: forward as
             SAS_WS_MSG_WIFI_STATE.
*/
static void on_wifi_state_changed(const sas_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if ((event == NULL) || (event->payload == NULL) ||
        (event->payload_len != sizeof(sas_wifi_state_t)))
    {
        return;
    }
    sas_wifi_state_t state;
    (void)memcpy(&state, event->payload, sizeof(state));

    cJSON *data = cJSON_CreateObject();
    if (data == NULL)
    {
        return;
    }
    // The event carries the new state; the SSID/RSSI it applies to come from Wi-Fi Manager.
    sas_wifi_status_t wifi_status;
    (void)memset(&wifi_status, 0, sizeof(wifi_status));
    (void)sas_wifi_get_status(&wifi_status);

    cJSON_AddStringToObject(data, "state", wifi_state_name(state));
    cJSON_AddStringToObject(data, "ssid", wifi_status.ssid);
    cJSON_AddNumberToObject(data, "rssi", wifi_status.rssi);
    cJSON_AddStringToObject(data, "ip", wifi_status.ip);
    cJSON_AddStringToObject(data, "hostname", wifi_status.hostname);
    cJSON_AddStringToObject(data, "ap_ssid", wifi_status.ap_ssid);
    cJSON_AddBoolToObject(data, "ap_active", wifi_status.ap_active);
    cJSON_AddBoolToObject(data, "ap_always_on", wifi_status.ap_always_on);
    cJSON_AddBoolToObject(data, "ap_secured", wifi_status.ap_secured);
    (void)sas_ws_broadcast(SAS_WS_MSG_WIFI_STATE, data);
    cJSON_Delete(data);
}

/*!
    \brief   SAS_EVENT_OTA_PROGRESS subscriber: forward as
             SAS_WS_MSG_OTA_PROGRESS.
*/
static void on_ota_progress(const sas_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if ((event == NULL) || (event->payload == NULL) ||
        (event->payload_len != sizeof(sas_ota_progress_t)))
    {
        return;
    }
    sas_ota_progress_t progress;
    (void)memcpy(&progress, event->payload, sizeof(progress));

    cJSON *data = cJSON_CreateObject();
    if (data == NULL)
    {
        return;
    }
    cJSON_AddStringToObject(data, "state", ota_state_name(progress.state));
    cJSON_AddNumberToObject(data, "percent_complete", progress.percent_complete);
    (void)sas_ws_broadcast(SAS_WS_MSG_OTA_PROGRESS, data);
    cJSON_Delete(data);
}

/*!
    \brief   SAS_EVENT_BLE_DEVICE_DISCOVERED subscriber: forward as
             SAS_WS_MSG_BLE_DEVICE.
*/
static void on_ble_device_discovered(const sas_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if ((event == NULL) || (event->payload == NULL) ||
        (event->payload_len != sizeof(sas_ble_device_t)))
    {
        return;
    }
    sas_ble_device_t device;
    (void)memcpy(&device, event->payload, sizeof(device));

    cJSON *data = cJSON_CreateObject();
    if (data == NULL)
    {
        return;
    }
    char mac_str[18];
    format_mac(device.mac, mac_str, sizeof(mac_str));
    cJSON_AddStringToObject(data, "mac", mac_str);
    cJSON_AddStringToObject(data, "name", device.name);
    cJSON_AddNumberToObject(data, "rssi", device.rssi);
    cJSON_AddNumberToObject(data, "last_seen_us", (double)device.last_seen_us);
    (void)sas_ws_broadcast(SAS_WS_MSG_BLE_DEVICE, data);
    cJSON_Delete(data);
}

/*!
    \brief   SAS_EVENT_LOG_ENTRY subscriber: forward as SAS_WS_MSG_LOG_ENTRY.
*/
static void on_log_entry(const sas_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if ((event == NULL) || (event->payload == NULL) ||
        (event->payload_len != sizeof(sas_log_entry_t)))
    {
        return;
    }
    sas_log_entry_t entry;
    (void)memcpy(&entry, event->payload, sizeof(entry));

    cJSON *data = cJSON_CreateObject();
    if (data == NULL)
    {
        return;
    }
    cJSON_AddNumberToObject(data, "timestamp_us", (double)entry.timestamp_us);
    cJSON_AddStringToObject(data, "level", log_level_name(entry.level));
    cJSON_AddStringToObject(data, "tag", entry.tag);
    cJSON_AddStringToObject(data, "message", entry.message);
    (void)sas_ws_broadcast(SAS_WS_MSG_LOG_ENTRY, data);
    cJSON_Delete(data);
}
