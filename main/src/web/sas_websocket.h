//=============================================================================
/*!
    \file    sas_websocket.h
    \brief   Public interface for the WebSocket module.
*/
//=============================================================================

// Sends live updates (system statistics, Wi-Fi state, OTA progress, BLE
// discoveries, terminal output, log entries) to connected browsers over one
// WebSocket endpoint. Modules can also send or broadcast messages directly and
// can register a handler for a message type sent by the client.
//
// A client may pass its session token as ?token=<token> when connecting. With
// authentication enabled, a client without a valid token only receives the
// "sysinfo" and "wifi_state" messages.

#ifndef SAS_WEBSOCKET_H
#define SAS_WEBSOCKET_H

#include <stddef.h>

#include "../core/sas_status.h"
#include "cJSON.h"

/*! \brief Broadcastable message categories carried in the JSON envelope's "type" field. */
typedef enum
{
    SAS_WS_MSG_SYSINFO = 0,
    SAS_WS_MSG_WIFI_STATE,
    SAS_WS_MSG_OTA_PROGRESS,
    SAS_WS_MSG_BLE_DEVICE,
    SAS_WS_MSG_TERMINAL_OUTPUT,
    SAS_WS_MSG_LOG_ENTRY,
} sas_ws_msg_type_t;

/*! \brief Handle identifying a single connected WebSocket client. */
typedef struct
{
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

#define SAS_WS_ERR_BASE (0x8900U) ///< Custom status base for this module.
/*! \brief Connect attempt rejected: SAS_WS_MAX_CLIENTS connections already tracked. */
#define SAS_WS_ERR_CLIENT_TABLE_FULL (SAS_WS_ERR_BASE + 1U)

/*!
    \brief   Maximum number of WebSocket clients served at once.
*/
#define SAS_WS_MAX_CLIENTS (4U)

/*!
    \brief Per-client outbound message ring buffer depth. Sized so one Terminal
    command's full output (SAS_TERM_MAX_OUTPUT_LEN, sent as several
    "terminal_output" messages that cannot drain until the command handler
    returns) fits without evicting its own earlier chunks.
*/
#define SAS_WS_CLIENT_SEND_QUEUE_LEN (16U)

/*!
    \brief Largest serialized { "type", "data" } envelope, in bytes including the
    NUL, that sas_ws_broadcast()/sas_ws_send_to_client() can queue.
*/
#define SAS_WS_MESSAGE_MAX_LEN (320U)

#define SAS_WS_MAX_MESSAGE_HANDLERS (4U) ///< Capacity of the client-to-device handler table.

sas_status_t sas_ws_init(void);

sas_status_t sas_ws_start(void);

sas_status_t sas_ws_stop(void);

sas_status_t sas_ws_deinit(void);

sas_status_t sas_ws_broadcast(sas_ws_msg_type_t type, cJSON *payload);

sas_status_t sas_ws_send_to_client(const sas_ws_client_t *client, sas_ws_msg_type_t type,
                                   cJSON *payload);

sas_status_t sas_ws_register_message_handler(const char *type, sas_ws_message_fn handler);

sas_status_t sas_ws_get_client_token(const sas_ws_client_t *client, char *out, size_t out_len);

#endif /* SAS_WEBSOCKET_H */
