/*!
    \file    sas_websocket.h
    \brief   Public interface for the WebSocket module.

    \description
    Broadcasts real-time server-originated events (runtime statistics,
    Wi-Fi state, OTA progress, BLE discoveries, terminal output, streamed
    log entries) to connected browser clients over a shared WebSocket
    endpoint registered with the HTTP Server. Also exposes a direct
    send/broadcast API for modules (e.g. Terminal) that need to push data
    outside the Event Bus, and lets a module claim a client-to-device
    message type (e.g. Terminal's "terminal_cmd") without this module
    depending on it.

    A client may present its session token as ?token=<token> on the upgrade
    request. Where Authentication is enabled, a client without a valid token
    is still accepted but only receives the message types the Home view
    shows without a login ("sysinfo", "wifi_state").

    \dependencies
    - HTTP Server (main/src/web/sas_http_server.h): owns the underlying
      HTTPS listener; this module registers its WebSocket URI handler with
      sas_http_register_ws_handler() and never opens its own socket.
    - Event Bus (main/src/core/sas_event_bus.h): source of the
      "broadcastable" events this module forwards to connected clients.
    - System Information (main/src/core/sas_sysinfo.h): source of the
      periodic "sysinfo" broadcast.
    - Authentication (main/src/auth/sas_auth.h): validates the token a
      client presents on the upgrade request.
*/

#pragma once

/* 1. Standard includes */
#include <stddef.h>

/* 2. ESP-IDF includes */
/* (none) */

/* 3. Project includes */
#include "../core/sas_status.h"
#include "cJSON.h"

/* 4. Public types */

/*! Broadcastable message categories carried in the JSON envelope's "type" field. */
typedef enum {
    SAS_WS_MSG_SYSINFO = 0,
    SAS_WS_MSG_WIFI_STATE,
    SAS_WS_MSG_OTA_PROGRESS,
    SAS_WS_MSG_BLE_DEVICE,
    SAS_WS_MSG_TERMINAL_OUTPUT,
    SAS_WS_MSG_LOG_ENTRY,
} sas_ws_msg_type_t;

/*! Handle identifying a single connected WebSocket client. */
typedef struct {
    int fd; ///< Underlying socket descriptor, as reported by the HTTP Server.
} sas_ws_client_t;

/*!
    \brief   Handler for one client-to-device message type, registered with
             sas_ws_register_message_handler(). Runs on the HTTP Server's
             task.
    \param   client Non-NULL client the message came from; valid only for
                    the duration of the call.
    \param   data   The envelope's "data" member; NULL if the message had
                    none. Valid only for the duration of the call.
*/
typedef void (*sas_ws_message_fn)(const sas_ws_client_t *client, const cJSON *data);

/* 5. Public constants */

#define SAS_WS_ERR_BASE (0x8900U) ///< Custom status base for this module.
#define SAS_WS_ERR_CLIENT_TABLE_FULL                                                               \
    (SAS_WS_ERR_BASE +                                                                             \
     1U) ///< Connect attempt rejected: SAS_WS_MAX_CLIENTS connections already tracked.

/*!
    Maximum concurrent WebSocket clients tracked by this module.
    Placeholder sizing honoring the ">= 4" floor; to be tuned during
    implementation against actual concurrent-client requirements.
*/
#define SAS_WS_MAX_CLIENTS (4U)

/*!
    Per-client outbound message ring buffer depth. Sized so one Terminal
    command's full output (SAS_TERM_MAX_OUTPUT_LEN, sent as several
    "terminal_output" messages that cannot drain until the command handler
    returns) fits without evicting its own earlier chunks.
*/
#define SAS_WS_CLIENT_SEND_QUEUE_LEN (16U)

/*!
    Largest serialized { "type", "data" } envelope, in bytes including the
    NUL, that sas_ws_broadcast()/sas_ws_send_to_client() can queue.
*/
#define SAS_WS_MESSAGE_MAX_LEN (320U)

#define SAS_WS_MAX_MESSAGE_HANDLERS (4U) ///< Capacity of the client-to-device handler table.

/* 6. Public API declarations */

/*!
    \brief   Initialize the WebSocket module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already initialized.
*/
sas_status_t sas_ws_init(void);

/*!
    \brief   Start active WebSocket operation: register the URI handler with
             the HTTP Server and subscribe to broadcastable Event Bus events.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_ws_start(void);

/*!
    \brief   Stop active WebSocket operation without releasing resources.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_ws_stop(void);

/*!
    \brief   Release all resources acquired by sas_ws_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_ws_deinit(void);

/*!
    \brief   Send a JSON envelope { "type": ..., "data": payload } to every
             currently connected client.
    \param   type    Message category carried in the envelope's "type" field.
    \param   payload Non-NULL cJSON payload; ownership remains with the
                      caller, this call does not delete it.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if payload is NULL;
             SAS_ERR_INVALID_STATE if not started; SAS_ERR_NO_MEM if the
             serialized envelope exceeds SAS_WS_MESSAGE_MAX_LEN.
*/
sas_status_t sas_ws_broadcast(sas_ws_msg_type_t type, cJSON *payload);

/*!
    \brief   Send a JSON envelope { "type": ..., "data": payload } to a
             single connected client, e.g. for per-session Terminal output.
    \param   client  Non-NULL descriptor of a currently connected client.
    \param   type    Message category carried in the envelope's "type" field.
    \param   payload Non-NULL cJSON payload; ownership remains with the
                      caller, this call does not delete it.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if client or
             payload is NULL; SAS_ERR_INVALID_STATE if not started;
             SAS_ERR_NO_MEM if the serialized envelope exceeds
             SAS_WS_MESSAGE_MAX_LEN.
*/
sas_status_t sas_ws_send_to_client(const sas_ws_client_t *client, sas_ws_msg_type_t type,
                                   cJSON *payload);

/*!
    \brief   Claim a client-to-device message type: every received
             { "type": type, "data": ... } envelope is passed to handler.
    \param   type    Non-NULL message type with static storage duration
                     (stored by pointer), e.g. "terminal_cmd".
    \param   handler Non-NULL handler.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if type or handler
             is NULL; SAS_ERR_NO_MEM if SAS_WS_MAX_MESSAGE_HANDLERS types are
             already claimed.
*/
sas_status_t sas_ws_register_message_handler(const char *type, sas_ws_message_fn handler);

/*!
    \brief   Copy out the session token a client presented on its upgrade
             request.
    \param   client  Non-NULL descriptor of a connected client.
    \param   out     Non-NULL destination; receives an empty string if the
                     client presented no token.
    \param   out_len Capacity of out in bytes (33 holds any token).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if client or out
             is NULL or out_len is 0; SAS_ERR_INVALID_STATE if client is not
             connected.
*/
sas_status_t sas_ws_get_client_token(const sas_ws_client_t *client, char *out, size_t out_len);
