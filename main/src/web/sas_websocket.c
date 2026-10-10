/*!
    \file    sas_websocket.c
    \brief   Implementation of the WebSocket module.

    \description
    See sas_websocket.h for the public contract. Subscribes to
    SAS_EVENT_WIFI_STATE_CHANGED, SAS_EVENT_BLE_DEVICE_DISCOVERED, and
    SAS_EVENT_LOG_ENTRY (the events the current dashboard feature needs);
    SAS_EVENT_OTA_PROGRESS is not subscribed since OTA is out of scope for
    this change.

    \note    Event Bus callbacks run on whichever task published the event
    (Wi-Fi's event loop task, the NimBLE host task, or a caller's task via
    Logger) - never the httpd task. Frames are therefore never sent
    directly from sas_ws_broadcast()/sas_ws_send_to_client(): each enqueues
    into a fixed-size per-client ring (copying the serialized message, so
    no dynamic allocation), then schedules a drain via httpd_queue_work()
    so the actual httpd_ws_send_frame_async() call runs on the httpd task,
    per ESP-IDF's documented async-send pattern.
*/

/* 1. Includes */
#include "sas_websocket.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sas_http_server.h"

#include "../core/sas_event_bus.h"
#include "../core/sas_logger.h"
#include "../network/sas_ble_scanner.h"
#include "../network/sas_wifi_manager.h"

static const char *TAG = "sas_ws";

/* 2. Private macros */
#define SAS_WS_LOCK_WAIT_TICKS                                                                    \
    pdMS_TO_TICKS(50) ///< Bounded wait for s_table_lock; critical sections are short array scans/
                      ///< ring pushes.
#define SAS_WS_MESSAGE_MAX_LEN                                                                     \
    (320U) ///< Max serialized envelope length kept per queued message; every envelope this
          ///< module builds (wifi_state/ble_device/log_entry) fits comfortably under this.
#define SAS_WS_URI "/ws" ///< Sole WebSocket endpoint registered with the HTTP Server.

/* 3. Private typedefs */

/*! One queued outbound message, copied in full (no dynamic allocation). */
typedef struct {
    char text[SAS_WS_MESSAGE_MAX_LEN]; ///< Serialized "{ type, data }" envelope.
    size_t len;                        ///< strlen(text).
} sas_ws_queued_msg_t;

/*! One tracked client connection and its outbound ring. */
typedef struct {
    bool in_use;    ///< false if this slot is free.
    int fd;         ///< Underlying socket descriptor.
    sas_ws_queued_msg_t ring[SAS_WS_CLIENT_SEND_QUEUE_LEN]; ///< Circular outbound queue.
    uint8_t ring_head;  ///< Index of the oldest queued message.
    uint8_t ring_count; ///< Number of valid entries in ring.
} sas_ws_slot_t;

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;

static sas_ws_slot_t s_clients[SAS_WS_MAX_CLIENTS];

static SemaphoreHandle_t s_table_lock = NULL;
static StaticSemaphore_t s_table_lock_storage;

static uint32_t s_wifi_state_sub = 0U;
static uint32_t s_ble_device_sub = 0U;
static uint32_t s_log_entry_sub = 0U;

/* 5. Forward declarations */
/* Stays esp_err_t: same ESP-IDF-mandated exception as sas_http_handler_fn, see
   sas_http_server.h. */
static esp_err_t ws_handler(httpd_req_t *req);
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

/* 6. Public functions */

sas_status_t sas_ws_init(void)
{
    if (s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    (void)memset(s_clients, 0, sizeof(s_clients));

    s_table_lock = xSemaphoreCreateMutexStatic(&s_table_lock_storage);
    configASSERT(s_table_lock != NULL);

    s_started = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_ws_start(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    httpd_uri_t ws_uri = {
        .uri = SAS_WS_URI,
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
    };
    sas_status_t status = sas_http_register_ws_handler(SAS_WS_URI, &ws_uri);
    if ((status != SAS_STATUS_OK) && (status != SAS_HTTP_ERR_ROUTE_CONFLICT)) {
        return status;
    }

    (void)sas_event_bus_subscribe(SAS_EVENT_WIFI_STATE_CHANGED, on_wifi_state_changed, NULL,
                                  &s_wifi_state_sub);
    (void)sas_event_bus_subscribe(SAS_EVENT_BLE_DEVICE_DISCOVERED, on_ble_device_discovered, NULL,
                                  &s_ble_device_sub);
    (void)sas_event_bus_subscribe(SAS_EVENT_LOG_ENTRY, on_log_entry, NULL, &s_log_entry_sub);

    s_started = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_ws_stop(void)
{
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    (void)sas_event_bus_unsubscribe(s_wifi_state_sub);
    (void)sas_event_bus_unsubscribe(s_ble_device_sub);
    (void)sas_event_bus_unsubscribe(s_log_entry_sub);

    s_started = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_ws_deinit(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_table_lock != NULL) {
        vSemaphoreDelete(s_table_lock);
        s_table_lock = NULL;
    }
    (void)memset(s_clients, 0, sizeof(s_clients));

    s_initialized = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_ws_broadcast(sas_ws_msg_type_t type, cJSON *payload)
{
    if (payload == NULL) {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    return build_and_enqueue(-1, type, payload);
}

sas_status_t sas_ws_send_to_client(const sas_ws_client_t *client, sas_ws_msg_type_t type,
                                   cJSON *payload)
{
    if ((client == NULL) || (payload == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    return build_and_enqueue(client->fd, type, payload);
}

/* 7. Private functions */

/*!
    \brief   Build the { "type", "data" } envelope once and enqueue it for
             one client (target_fd >= 0) or every connected client
             (target_fd < 0), scheduling an async drain for each.
*/
static sas_status_t build_and_enqueue(int target_fd_or_broadcast, sas_ws_msg_type_t type,
                                      cJSON *payload)
{
    cJSON *envelope = cJSON_CreateObject();
    if (envelope == NULL) {
        return SAS_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(envelope, "type", (double)type);
    cJSON_AddItemReferenceToObject(envelope, "data", payload);

    char *text = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    if (text == NULL) {
        return SAS_ERR_NO_MEM;
    }
    size_t len = strlen(text);

    if (xSemaphoreTake(s_table_lock, SAS_WS_LOCK_WAIT_TICKS) == pdTRUE) {
        for (size_t i = 0U; i < SAS_WS_MAX_CLIENTS; i++) {
            if (!s_clients[i].in_use) {
                continue;
            }
            if ((target_fd_or_broadcast >= 0) && (s_clients[i].fd != target_fd_or_broadcast)) {
                continue;
            }
            enqueue_for_client(i, text, len);
        }
        xSemaphoreGive(s_table_lock);
    }

    cJSON_free(text);
    return SAS_STATUS_OK;
}

/*! \brief Push text onto s_clients[slot_index]'s ring (dropping the oldest entry if full), then
 * schedule its async drain. Caller holds s_table_lock. */
static void enqueue_for_client(size_t slot_index, const char *text, size_t len)
{
    sas_ws_slot_t *slot = &s_clients[slot_index];
    size_t copy_len = (len < (SAS_WS_MESSAGE_MAX_LEN - 1U)) ? len : (SAS_WS_MESSAGE_MAX_LEN - 1U);

    uint8_t write_index;
    if (slot->ring_count < SAS_WS_CLIENT_SEND_QUEUE_LEN) {
        write_index = (uint8_t)((slot->ring_head + slot->ring_count) % SAS_WS_CLIENT_SEND_QUEUE_LEN);
        slot->ring_count++;
    } else {
        write_index = slot->ring_head; /* full: overwrite the oldest, per this module's contract */
        slot->ring_head = (uint8_t)((slot->ring_head + 1U) % SAS_WS_CLIENT_SEND_QUEUE_LEN);
    }
    (void)memcpy(slot->ring[write_index].text, text, copy_len);
    slot->ring[write_index].text[copy_len] = '\0';
    slot->ring[write_index].len = copy_len;

    httpd_handle_t server = sas_http_get_server_handle();
    if (server != NULL) {
        (void)httpd_queue_work(server, async_drain_work, (void *)(intptr_t)slot->fd);
    }
}

/*!
    \brief   httpd-task-context work item: drain every currently queued
             message for the client identified by fd (passed by value via
             arg) and send each as a WebSocket text frame. Runs on the
             httpd task, so httpd_ws_send_frame_async() is safe here
             regardless of which task originally enqueued the message.
    \note    Removes the client from the table on a send failure (treated
             as "this client disconnected"), matching this module's
             contract to skip a disconnected client without aborting
             delivery to the others.
*/
static void async_drain_work(void *arg)
{
    int fd = (int)(intptr_t)arg;

    for (;;) {
        sas_ws_queued_msg_t msg;
        bool have_msg = false;
        size_t slot_index = 0U;
        bool client_still_present = false;

        if (xSemaphoreTake(s_table_lock, SAS_WS_LOCK_WAIT_TICKS) == pdTRUE) {
            for (size_t i = 0U; i < SAS_WS_MAX_CLIENTS; i++) {
                if (s_clients[i].in_use && (s_clients[i].fd == fd)) {
                    slot_index = i;
                    client_still_present = true;
                    if (s_clients[i].ring_count > 0U) {
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

        if (!client_still_present || !have_msg) {
            break;
        }

        httpd_ws_frame_t frame;
        (void)memset(&frame, 0, sizeof(frame));
        frame.type = HTTPD_WS_TYPE_TEXT;
        frame.payload = (uint8_t *)msg.text;
        frame.len = msg.len;

        httpd_handle_t server = sas_http_get_server_handle();
        if ((server == NULL) || (httpd_ws_send_frame_async(server, fd, &frame) != ESP_OK)) {
            if (xSemaphoreTake(s_table_lock, SAS_WS_LOCK_WAIT_TICKS) == pdTRUE) {
                s_clients[slot_index].in_use = false;
                xSemaphoreGive(s_table_lock);
            }
            break;
        }
    }
}

/*!
    \brief   Registered WebSocket handler for SAS_WS_URI. The initial
             (handshake) call arrives with req->method == HTTP_GET; every
             call after the upgrade is a data/control frame.
*/
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        if (xSemaphoreTake(s_table_lock, SAS_WS_LOCK_WAIT_TICKS) == pdTRUE) {
            for (size_t i = 0U; i < SAS_WS_MAX_CLIENTS; i++) {
                if (!s_clients[i].in_use) {
                    (void)memset(&s_clients[i], 0, sizeof(s_clients[i]));
                    s_clients[i].in_use = true;
                    s_clients[i].fd = fd;
                    xSemaphoreGive(s_table_lock);
                    return ESP_OK;
                }
            }
            xSemaphoreGive(s_table_lock);
        }
        /* SAS_WS_ERR_CLIENT_TABLE_FULL is a sas_status_t, not a valid
         * esp_err_t; log it and return a real esp_err_t to httpd. */
        sas_log_warn("SAS_WS_MAX_CLIENTS reached (0x%x), rejecting fd %d",
                    (unsigned int)SAS_WS_ERR_CLIENT_TABLE_FULL, fd);
        return ESP_FAIL;
    }

    httpd_ws_frame_t frame;
    (void)memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        if (xSemaphoreTake(s_table_lock, SAS_WS_LOCK_WAIT_TICKS) == pdTRUE) {
            for (size_t i = 0U; i < SAS_WS_MAX_CLIENTS; i++) {
                if (s_clients[i].in_use && (s_clients[i].fd == fd)) {
                    s_clients[i].in_use = false;
                    break;
                }
            }
            xSemaphoreGive(s_table_lock);
        }
        return ESP_OK;
    }

    /* This dashboard feature (Wi-Fi/BLE/log display) has no client-to-
     * device WebSocket messages; Terminal's terminal_cmd is out of scope.
     * Drain and discard any payload so the connection stays healthy. */
    if (frame.len > 0U) {
        uint8_t discard[128];
        frame.payload = discard;
        size_t to_read = (frame.len < sizeof(discard)) ? frame.len : sizeof(discard);
        (void)httpd_ws_recv_frame(req, &frame, to_read);
    }
    return ESP_OK;
}

/*! \brief Map sas_wifi_state_t to its JSON field string. */
static const char *wifi_state_name(sas_wifi_state_t state)
{
    switch (state) {
    case SAS_WIFI_STATE_IDLE:
        return "IDLE";
    case SAS_WIFI_STATE_CONNECTING:
        return "CONNECTING";
    case SAS_WIFI_STATE_CONNECTED:
        return "CONNECTED";
    case SAS_WIFI_STATE_RECONNECTING:
        return "RECONNECTING";
    case SAS_WIFI_STATE_AP_FALLBACK:
        return "AP_FALLBACK";
    default:
        return "UNKNOWN";
    }
}

/*! \brief Map sas_log_level_t to its JSON field string; mirrors sas_rest_api.c's log_level_name(),
 * kept private to each translation unit rather than shared, per this project's one-.c/.h-per-
 * module rule. */
static const char *log_level_name(sas_log_level_t level)
{
    switch (level) {
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

/*! \brief Format a 6-byte MAC as "AA:BB:CC:DD:EE:FF" into out (out_len >= 18). */
static void format_mac(const uint8_t mac[6], char *out, size_t out_len)
{
    (void)snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                   mac[4], mac[5]);
}

/*! \brief SAS_EVENT_WIFI_STATE_CHANGED subscriber: forward as SAS_WS_MSG_WIFI_STATE. */
static void on_wifi_state_changed(const sas_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if ((event == NULL) || (event->payload == NULL) ||
        (event->payload_len != sizeof(sas_wifi_state_t))) {
        return;
    }
    sas_wifi_state_t state;
    (void)memcpy(&state, event->payload, sizeof(state));

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return;
    }
    cJSON_AddStringToObject(data, "state", wifi_state_name(state));
    (void)sas_ws_broadcast(SAS_WS_MSG_WIFI_STATE, data);
    cJSON_Delete(data);
}

/*! \brief SAS_EVENT_BLE_DEVICE_DISCOVERED subscriber: forward as SAS_WS_MSG_BLE_DEVICE. */
static void on_ble_device_discovered(const sas_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if ((event == NULL) || (event->payload == NULL) ||
        (event->payload_len != sizeof(sas_ble_device_t))) {
        return;
    }
    sas_ble_device_t device;
    (void)memcpy(&device, event->payload, sizeof(device));

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
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

/*! \brief SAS_EVENT_LOG_ENTRY subscriber: forward as SAS_WS_MSG_LOG_ENTRY. */
static void on_log_entry(const sas_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if ((event == NULL) || (event->payload == NULL) ||
        (event->payload_len != sizeof(sas_log_entry_t))) {
        return;
    }
    sas_log_entry_t entry;
    (void)memcpy(&entry, event->payload, sizeof(entry));

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return;
    }
    cJSON_AddNumberToObject(data, "timestamp_us", (double)entry.timestamp_us);
    cJSON_AddStringToObject(data, "level", log_level_name(entry.level));
    cJSON_AddStringToObject(data, "tag", entry.tag);
    cJSON_AddStringToObject(data, "message", entry.message);
    (void)sas_ws_broadcast(SAS_WS_MSG_LOG_ENTRY, data);
    cJSON_Delete(data);
}
