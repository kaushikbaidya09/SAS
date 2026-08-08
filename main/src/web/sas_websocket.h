/*!
    \file    sas_websocket.h
    \brief   Public interface for the WebSocket module.

    \description
    Broadcasts real-time server-originated events (Wi-Fi state, OTA
    progress, BLE discoveries, terminal output, streamed log entries) to
    connected browser clients over a shared WebSocket endpoint registered
    with the HTTP Server. Also exposes a direct send/broadcast API for
    modules (e.g. Terminal) that need to push data outside the Event Bus.

    \dependencies
    - HTTP Server (main/src/web/sas_http_server.h): owns the underlying
      HTTPS listener; this module registers its WebSocket URI handler with
      sas_http_register_ws_handler() and never opens its own socket.
    - Event Bus (main/src/core/sas_event_bus.h): source of the
      "broadcastable" events this module forwards to connected clients.
*/

#pragma once

/* 1. Standard includes */
/* (none) */

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

/* 5. Public constants */

#define SAS_WS_ERR_BASE (0x8900U) ///< Custom status base for this module.
#define SAS_WS_ERR_CLIENT_TABLE_FULL (SAS_WS_ERR_BASE + 1U) ///< Connect attempt rejected: SAS_WS_MAX_CLIENTS connections already tracked.

/*!
    Maximum concurrent WebSocket clients tracked by this module.
    Placeholder sizing honoring the ">= 4" floor; to be tuned during
    implementation against actual concurrent-client requirements.
*/
#define SAS_WS_MAX_CLIENTS (4U)

/*!
    Per-client outbound message ring buffer depth.
    Placeholder sizing honoring the ">= 4" floor; to be tuned during
    implementation against actual broadcast burst rates.
*/
#define SAS_WS_CLIENT_SEND_QUEUE_LEN (8U)

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
             SAS_ERR_INVALID_STATE if not started.
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
             payload is NULL; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_ws_send_to_client(const sas_ws_client_t *client,
                                   sas_ws_msg_type_t type,
                                   cJSON *payload);
