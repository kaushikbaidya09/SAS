/*!
    \file    sas_event_bus.h
    \brief   Public interface for the Event Bus module.

    \description
    Synchronous, in-process publish/subscribe event dispatcher. This module
    owns the single project-wide registry of well-known SAS_EVENT_* IDs and
    the subscribe/unsubscribe/publish API used by every other module to
    decouple event producers from event consumers.

    \dependencies
    (none: Event Bus is a Layer-1 module with no compile-time header deps)
*/

#pragma once

/* 1. Standard includes */
#include <stddef.h>
#include <stdint.h>

/* 2. ESP-IDF includes */
/* (none) */

/* 3. Project includes */
#include "sas_status.h"

/* 4. Public types */

/*! A single published event: an ID plus an optional, caller-owned payload. */
typedef struct {
    uint32_t    event_id;    ///< One of the SAS_EVENT_* IDs below.
    const void *payload;     ///< Optional payload; may be NULL.
    size_t      payload_len; ///< Payload length in bytes; 0 if payload is NULL.
} sas_event_t;

/*!
    \brief Subscriber callback invoked synchronously from sas_event_bus_publish().
    \param event    Non-NULL event being dispatched; valid only for the
                     duration of the call.
    \param user_ctx Opaque context supplied at subscribe time.
*/
typedef void (*sas_event_callback_fn)(const sas_event_t *event, void *user_ctx);

/*! One entry in the internal subscription table. */
typedef struct {
    uint32_t              event_id; ///< Event ID this subscription is for.
    sas_event_callback_fn callback; ///< Non-NULL callback to invoke.
    void                 *user_ctx; ///< Opaque context passed back to callback.
} sas_event_subscription_t;

/* 5. Public constants */

#define SAS_EVT_ERR_BASE (0x8400U) ///< Custom sas_status_t base for this module.
#define SAS_EVT_ERR_TABLE_FULL (SAS_EVT_ERR_BASE + 1U) ///< subscribe() failed: SAS_EVT_MAX_SUBSCRIPTIONS entries already in use.
#define SAS_EVT_ERR_PAYLOAD_TOO_LARGE (SAS_EVT_ERR_BASE + 2U) ///< publish() failed: payload_len exceeds SAS_EVT_MAX_PAYLOAD_LEN; event was not dispatched.

/*! Maximum concurrent subscriptions across all event IDs.
    Placeholder sizing honoring the ">= 32" floor; to be tuned during
    implementation against actual subscriber counts. */
#define SAS_EVT_MAX_SUBSCRIPTIONS (32U)

/*! Maximum payload size, in bytes, accepted by sas_event_bus_publish().
    Placeholder sizing; to be tuned during implementation against actual
    event payload shapes. */
#define SAS_EVT_MAX_PAYLOAD_LEN (256U)

/*!
    Well-known event IDs. This is the single project-wide registry; each
    owning module appends its own SAS_EVENT_* constants here as it is
    implemented.
*/
typedef enum {
    /* Configuration Manager */
    SAS_EVENT_CONFIG_CHANGED = 1,

    /* Wi-Fi Manager */
    SAS_EVENT_WIFI_STATE_CHANGED,

    /* OTA Manager */
    SAS_EVENT_OTA_PROGRESS,

    /* BLE Manager */
    SAS_EVENT_BLE_DEVICE_DISCOVERED,

    /* Additional SAS_EVENT_* constants are appended here by each owning
     * module as it is implemented. */
} sas_event_id_t;

/* 6. Public API declarations */

/*!
    \brief   Initialize the Event Bus module.
    \return  SAS_STATUS_OK on success.
*/
sas_status_t sas_event_bus_init(void);

/*!
    \brief   Start active event dispatch operation.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_event_bus_start(void);

/*!
    \brief   Stop active event dispatch operation without releasing resources.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_event_bus_stop(void);

/*!
    \brief   Release all resources acquired by sas_event_bus_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_event_bus_deinit(void);

/*!
    \brief   Subscribe a callback to an event ID.
    \param   event_id                 Event ID to subscribe to.
    \param   cb                       Non-NULL callback to invoke on publish.
    \param   user_ctx                 Opaque context passed back to cb; may be NULL.
    \param   out_subscription_handle  Non-NULL; receives a handle usable with
                                       sas_event_bus_unsubscribe().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if cb or
             out_subscription_handle is NULL; SAS_EVT_ERR_TABLE_FULL if
             SAS_EVT_MAX_SUBSCRIPTIONS is already in use.
*/
sas_status_t sas_event_bus_subscribe(uint32_t event_id,
                                      sas_event_callback_fn cb,
                                      void *user_ctx,
                                      uint32_t *out_subscription_handle);

/*!
    \brief   Remove a previously registered subscription.
    \param   subscription_handle Handle returned by sas_event_bus_subscribe().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if the handle is unknown.
*/
sas_status_t sas_event_bus_unsubscribe(uint32_t subscription_handle);

/*!
    \brief   Publish an event synchronously to all current subscribers of
             event_id.
    \param   event_id    Event ID to publish.
    \param   payload     Optional payload to copy to subscribers; may be NULL.
    \param   payload_len Payload length in bytes; must be 0 if payload is NULL.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if payload is NULL
             and payload_len is nonzero; SAS_EVT_ERR_PAYLOAD_TOO_LARGE if
             payload_len exceeds SAS_EVT_MAX_PAYLOAD_LEN (event not dispatched).
*/
sas_status_t sas_event_bus_publish(uint32_t event_id, const void *payload,
                                    size_t payload_len);
