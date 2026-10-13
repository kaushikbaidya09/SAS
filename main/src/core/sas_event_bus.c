//=============================================================================
/*!
    \file    sas_event_bus.c
    \brief   Implementation of the Event Bus module.
*/
//=============================================================================

#include "sas_event_bus.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*! \brief Bounded wait for s_table_lock; critical sections are microsecond-scale. */
#define SAS_EVT_LOCK_WAIT_TICKS pdMS_TO_TICKS(50)
/*!
    \brief   Bounded wait for s_dispatch_lock; covers a concurrent publish() on
             another task or a chain of nested same-task publish() calls.
*/
#define SAS_EVT_DISPATCH_WAIT_TICKS pdMS_TO_TICKS(1000)

/*! \brief Low 16 bits of a handle: 1-based slot index (0 is invalid). */
#define SAS_EVT_HANDLE_INDEX_MASK (0xFFFFU)

/*! \brief One subscription-table slot: bookkeeping plus the public-facing subscription fields. */
typedef struct
{
    bool in_use;         ///< false if this slot is free.
    uint16_t generation; ///< Bumped on unsubscribe(); invalidates stale handles into a reused slot.
    sas_event_subscription_t sub; ///< event_id / callback / user_ctx.
} sas_event_slot_t;

static bool s_initialized = false;
static bool s_started     = false;

static sas_event_slot_t s_table[SAS_EVT_MAX_SUBSCRIPTIONS];

static SemaphoreHandle_t s_table_lock = NULL; ///< Guards the subscription table.
static StaticSemaphore_t s_table_lock_storage;

// Recursive, so that a callback can publish another event from the same task.
// It is held for a whole publish() call; the table lock is released before the
// callbacks run, so a callback can also subscribe or unsubscribe.
static SemaphoreHandle_t s_dispatch_lock = NULL;
static StaticSemaphore_t s_dispatch_lock_storage;

static uint32_t make_handle(size_t index, uint16_t generation);
static bool decode_handle(uint32_t handle, size_t *out_index, uint16_t *out_generation);

/*!
    \brief   Initialize the Event Bus module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already
             initialized.
*/
sas_status_t sas_event_bus_init(void)
{
    if (s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    (void)memset(s_table, 0, sizeof(s_table));

    s_table_lock = xSemaphoreCreateMutexStatic(&s_table_lock_storage);
    configASSERT(s_table_lock != NULL);

    s_dispatch_lock = xSemaphoreCreateRecursiveMutexStatic(&s_dispatch_lock_storage);
    configASSERT(s_dispatch_lock != NULL);

    s_started     = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Start active event dispatch operation.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized
             or already started.
*/
sas_status_t sas_event_bus_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    s_started = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Stop active event dispatch operation without releasing resources.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_event_bus_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    s_started = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Release all resources acquired by sas_event_bus_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized
             or still started (call sas_event_bus_stop() first).
*/
sas_status_t sas_event_bus_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_dispatch_lock != NULL)
    {
        vSemaphoreDelete(s_dispatch_lock);
        s_dispatch_lock = NULL;
    }
    if (s_table_lock != NULL)
    {
        vSemaphoreDelete(s_table_lock);
        s_table_lock = NULL;
    }
    (void)memset(s_table, 0, sizeof(s_table));

    s_initialized = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Subscribe a callback to an event ID.
    \param[in]  event_id                 Event ID to subscribe to.
    \param[in]  cb                       Non-NULL callback to invoke on
                                         publish.
    \param[in]  user_ctx                 Opaque context passed back to cb; may
                                         be NULL.
    \param[out] out_subscription_handle  Non-NULL; receives a handle usable
                                         with sas_event_bus_unsubscribe().
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_ARG (cb or handle is NULL);
             SAS_EVT_ERR_TABLE_FULL (table full); SAS_ERR_TIMEOUT (lock busy).
*/
sas_status_t sas_event_bus_subscribe(uint32_t event_id, sas_event_callback_fn cb, void *user_ctx,
                                     uint32_t *out_subscription_handle)
{
    if ((cb == NULL) || (out_subscription_handle == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }
    configASSERT(s_initialized);

    if (xSemaphoreTake(s_table_lock, SAS_EVT_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_EVT_ERR_TABLE_FULL;
    for (size_t i = 0U; i < SAS_EVT_MAX_SUBSCRIPTIONS; i++)
    {
        if (!s_table[i].in_use)
        {
            s_table[i].in_use        = true;
            s_table[i].sub.event_id  = event_id;
            s_table[i].sub.callback  = cb;
            s_table[i].sub.user_ctx  = user_ctx;
            *out_subscription_handle = make_handle(i, s_table[i].generation);
            status                   = SAS_STATUS_OK;
            break;
        }
    }

    xSemaphoreGive(s_table_lock);
    return status;
}

/*!
    \brief   Remove a previously registered subscription. Safe to call from
             within that subscription's own callback during dispatch.
    \param[in] subscription_handle  Handle returned by
                                    sas_event_bus_subscribe().
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_ARG (unknown or already-removed
             handle); SAS_ERR_TIMEOUT (lock busy).
*/
sas_status_t sas_event_bus_unsubscribe(uint32_t subscription_handle)
{
    size_t index        = 0U;
    uint16_t generation = 0U;
    if (!decode_handle(subscription_handle, &index, &generation))
    {
        return SAS_ERR_INVALID_ARG;
    }
    configASSERT(s_initialized);

    if (xSemaphoreTake(s_table_lock, SAS_EVT_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_ERR_INVALID_ARG;
    if (s_table[index].in_use && (s_table[index].generation == generation))
    {
        s_table[index].in_use       = false;
        s_table[index].sub.callback = NULL;
        s_table[index].sub.user_ctx = NULL;
        s_table[index].generation++;
        status = SAS_STATUS_OK;
    }

    xSemaphoreGive(s_table_lock);
    return status;
}

/*!
    \brief   Publish an event synchronously to every callback currently
             subscribed to event_id.
    \param[in] event_id     Event ID to publish.
    \param[in] payload      Optional payload to copy to subscribers; may be
                            NULL.
    \param[in] payload_len  Payload length in bytes; must be 0 if payload is
                            NULL.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_ARG (payload NULL, len nonzero);
             SAS_EVT_ERR_PAYLOAD_TOO_LARGE (exceeds SAS_EVT_MAX_PAYLOAD_LEN,
             not dispatched); SAS_ERR_TIMEOUT (dispatch lock busy, not
             dispatched).
*/
sas_status_t sas_event_bus_publish(uint32_t event_id, const void *payload, size_t payload_len)
{
    // A callback may subscribe, unsubscribe or publish. A change to the
    // subscriptions only affects later publish() calls.

    if ((payload == NULL) && (payload_len != 0U))
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (payload_len > SAS_EVT_MAX_PAYLOAD_LEN)
    {
        return SAS_EVT_ERR_PAYLOAD_TOO_LARGE;
    }
    configASSERT(s_initialized);

    if (xSemaphoreTakeRecursive(s_dispatch_lock, SAS_EVT_DISPATCH_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    // On the stack, not static: a callback may publish again, and a shared
    // buffer would let that nested call overwrite this one's payload.
    uint8_t payload_copy[SAS_EVT_MAX_PAYLOAD_LEN];
    if (payload_len > 0U)
    {
        (void)memcpy(payload_copy, payload, payload_len);
    }

    sas_event_subscription_t snapshot[SAS_EVT_MAX_SUBSCRIPTIONS];
    size_t match_count = 0U;

    if (xSemaphoreTake(s_table_lock, SAS_EVT_LOCK_WAIT_TICKS) == pdTRUE)
    {
        for (size_t i = 0U; i < SAS_EVT_MAX_SUBSCRIPTIONS; i++)
        {
            if (s_table[i].in_use && (s_table[i].sub.event_id == event_id))
            {
                snapshot[match_count] = s_table[i].sub;
                match_count++;
            }
        }
        xSemaphoreGive(s_table_lock);
    }

    const sas_event_t event = {
        .event_id    = event_id,
        .payload     = (payload_len > 0U) ? payload_copy : NULL,
        .payload_len = payload_len,
    };

    for (size_t i = 0U; i < match_count; i++)
    {
        snapshot[i].callback(&event, snapshot[i].user_ctx);
    }

    xSemaphoreGiveRecursive(s_dispatch_lock);
    return SAS_STATUS_OK;
}

/*!
    \brief   Query whether the Event Bus is currently started and safe to
             publish() to.
    \return  true if sas_event_bus_start() has been called and
             sas_event_bus_stop()/sas_event_bus_deinit() has not since; false
             otherwise.
*/
bool sas_event_bus_is_started(void)
{
    // Lets modules that boot before the Event Bus, such as the Logger, check
    // whether publishing is possible yet.

    return s_started;
}

/*!
    \brief   Pack a 0-based slot index and its current generation into an
             opaque, externally-visible handle.
*/
static uint32_t make_handle(size_t index, uint16_t generation)
{
    return ((uint32_t)generation << 16) | (uint32_t)(index + 1U);
}

/*!
    \brief   Unpack a handle, validating that its index is in range.
*/
static bool decode_handle(uint32_t handle, size_t *out_index, uint16_t *out_generation)
{
    // Returns true when the index is in range. The caller still has to check
    // that the slot is in use and that the generation matches.

    uint32_t index_plus_one = handle & SAS_EVT_HANDLE_INDEX_MASK;
    if ((index_plus_one == 0U) || (index_plus_one > SAS_EVT_MAX_SUBSCRIPTIONS))
    {
        return false;
    }

    *out_index      = (size_t)(index_plus_one - 1U);
    *out_generation = (uint16_t)(handle >> 16);
    return true;
}
