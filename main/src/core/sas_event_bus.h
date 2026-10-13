//=============================================================================
/*!
    \file    sas_event_bus.h
    \brief   Public interface for the Event Bus module.
*/
//=============================================================================

// Synchronous publish/subscribe dispatcher that lets modules exchange events
// without depending on each other. Also defines the list of event IDs.

#ifndef SAS_EVENT_BUS_H
#define SAS_EVENT_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sas_status.h"

/*! \brief A single published event: an ID plus an optional, caller-owned payload. */
typedef struct
{
    uint32_t event_id;   ///< One of the SAS_EVENT_* IDs below.
    const void *payload; ///< Optional payload; may be NULL.
    size_t payload_len;  ///< Payload length in bytes; 0 if payload is NULL.
} sas_event_t;

/*!
    \brief Subscriber callback invoked synchronously from sas_event_bus_publish().
    \param event    Non-NULL event being dispatched; valid only for the
                     duration of the call.
    \param user_ctx Opaque context supplied at subscribe time.
*/
typedef void (*sas_event_callback_fn)(const sas_event_t *event, void *user_ctx);

/*! \brief One entry in the internal subscription table. */
typedef struct
{
    uint32_t event_id;              ///< Event ID this subscription is for.
    sas_event_callback_fn callback; ///< Non-NULL callback to invoke.
    void *user_ctx;                 ///< Opaque context passed back to callback.
} sas_event_subscription_t;

#define SAS_EVT_ERR_BASE (0x8400U) ///< Custom sas_status_t base for this module.
/*! \brief subscribe() failed: SAS_EVT_MAX_SUBSCRIPTIONS entries already in use. */
#define SAS_EVT_ERR_TABLE_FULL (SAS_EVT_ERR_BASE + 1U)
/*!
    \brief   publish() failed: payload_len exceeds SAS_EVT_MAX_PAYLOAD_LEN;
             event was not dispatched.
*/
#define SAS_EVT_ERR_PAYLOAD_TOO_LARGE (SAS_EVT_ERR_BASE + 2U)

/*! \brief Maximum number of subscriptions across all event IDs. */
#define SAS_EVT_MAX_SUBSCRIPTIONS (32U)

/*! \brief Maximum payload size in bytes accepted by sas_event_bus_publish(). */
#define SAS_EVT_MAX_PAYLOAD_LEN (256U)

/*! \brief Event IDs used across the project. */
typedef enum
{
    // Configuration Manager
    SAS_EVENT_CONFIG_CHANGED = 1,

    // Wi-Fi Manager
    SAS_EVENT_WIFI_STATE_CHANGED,

    // OTA Manager
    SAS_EVENT_OTA_PROGRESS, ///< Payload is a sas_ota_progress_t (sas_ota_manager.h).

    // BLE Scanner
    SAS_EVENT_BLE_DEVICE_DISCOVERED,

    // Logger
    SAS_EVENT_LOG_ENTRY, ///< Payload is a sas_log_entry_t (sas_logger.h).

    // Authentication. These carry no payload, so no token or credential is
    // exposed.
    SAS_EVENT_AUTH_LOGIN,   ///< A login succeeded and a session token was issued.
    SAS_EVENT_AUTH_LOGOUT,  ///< A session was ended by sas_auth_logout().
    SAS_EVENT_AUTH_LOCKOUT, ///< A login attempt was rejected by failed-login throttling.
} sas_event_id_t;

sas_status_t sas_event_bus_init(void);

sas_status_t sas_event_bus_start(void);

sas_status_t sas_event_bus_stop(void);

sas_status_t sas_event_bus_deinit(void);

sas_status_t sas_event_bus_subscribe(uint32_t event_id, sas_event_callback_fn cb, void *user_ctx,
                                     uint32_t *out_subscription_handle);

sas_status_t sas_event_bus_unsubscribe(uint32_t subscription_handle);

sas_status_t sas_event_bus_publish(uint32_t event_id, const void *payload, size_t payload_len);

bool sas_event_bus_is_started(void);

#endif /* SAS_EVENT_BUS_H */
