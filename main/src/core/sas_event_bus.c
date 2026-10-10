/*!
    \file    sas_event_bus.c
    \brief   Implementation of the Event Bus module.
*/

/* 1. Includes */
#include "sas_event_bus.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* 2. Private macros */
#define SAS_EVT_LOCK_WAIT_TICKS                                                                    \
    pdMS_TO_TICKS(50) ///< Bounded wait for s_table_lock; critical sections are microsecond-scale.
#define SAS_EVT_DISPATCH_WAIT_TICKS                                                                \
    pdMS_TO_TICKS(1000) ///< Bounded wait for s_dispatch_lock; covers a concurrent publish() on
                        ///< another task or a chain of nested same-task publish() calls.

#define SAS_EVT_HANDLE_INDEX_MASK                                                                  \
    (0xFFFFU) ///< Low 16 bits of a handle: 1-based slot index (0 is invalid).

/* 3. Private typedefs */

/*! One subscription-table slot: bookkeeping plus the public-facing subscription fields. */
typedef struct {
    bool in_use;         ///< false if this slot is free.
    uint16_t generation; ///< Bumped on unsubscribe(); invalidates stale handles into a reused slot.
    sas_event_subscription_t sub; ///< event_id / callback / user_ctx.
} sas_event_slot_t;

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;

static sas_event_slot_t s_table[SAS_EVT_MAX_SUBSCRIPTIONS];

static SemaphoreHandle_t s_table_lock = NULL; ///< Guards the subscription table.
static StaticSemaphore_t s_table_lock_storage;

/* Recursive, not a plain mutex, so a callback can call publish() again
 * from the same task (e.g. to chain a second event) without deadlocking.
 * Held for one publish() call end to end; publish() snapshots
 * subscribers and releases s_table_lock before dispatching, so a
 * callback can also safely (un)subscribe. */
static SemaphoreHandle_t s_dispatch_lock = NULL;
static StaticSemaphore_t s_dispatch_lock_storage;

/* 5. Forward declarations */
static uint32_t make_handle(size_t index, uint16_t generation);
static bool decode_handle(uint32_t handle, size_t *out_index, uint16_t *out_generation);

/* 6. Public functions */

sas_status_t sas_event_bus_init(void)
{
    if (s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    (void)memset(s_table, 0, sizeof(s_table));

    s_table_lock = xSemaphoreCreateMutexStatic(&s_table_lock_storage);
    configASSERT(s_table_lock != NULL);

    s_dispatch_lock = xSemaphoreCreateRecursiveMutexStatic(&s_dispatch_lock_storage);
    configASSERT(s_dispatch_lock != NULL);

    s_started = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_event_bus_start(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    s_started = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_event_bus_stop(void)
{
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    s_started = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_event_bus_deinit(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_dispatch_lock != NULL) {
        vSemaphoreDelete(s_dispatch_lock);
        s_dispatch_lock = NULL;
    }
    if (s_table_lock != NULL) {
        vSemaphoreDelete(s_table_lock);
        s_table_lock = NULL;
    }
    (void)memset(s_table, 0, sizeof(s_table));

    s_initialized = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_event_bus_subscribe(uint32_t event_id, sas_event_callback_fn cb, void *user_ctx,
                                     uint32_t *out_subscription_handle)
{
    if ((cb == NULL) || (out_subscription_handle == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }
    configASSERT(s_initialized);

    if (xSemaphoreTake(s_table_lock, SAS_EVT_LOCK_WAIT_TICKS) != pdTRUE) {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_EVT_ERR_TABLE_FULL;
    for (size_t i = 0U; i < SAS_EVT_MAX_SUBSCRIPTIONS; i++) {
        if (!s_table[i].in_use) {
            s_table[i].in_use = true;
            s_table[i].sub.event_id = event_id;
            s_table[i].sub.callback = cb;
            s_table[i].sub.user_ctx = user_ctx;
            *out_subscription_handle = make_handle(i, s_table[i].generation);
            status = SAS_STATUS_OK;
            break;
        }
    }

    xSemaphoreGive(s_table_lock);
    return status;
}

sas_status_t sas_event_bus_unsubscribe(uint32_t subscription_handle)
{
    size_t index = 0U;
    uint16_t generation = 0U;
    if (!decode_handle(subscription_handle, &index, &generation)) {
        return SAS_ERR_INVALID_ARG;
    }
    configASSERT(s_initialized);

    if (xSemaphoreTake(s_table_lock, SAS_EVT_LOCK_WAIT_TICKS) != pdTRUE) {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_ERR_INVALID_ARG;
    if (s_table[index].in_use && (s_table[index].generation == generation)) {
        s_table[index].in_use = false;
        s_table[index].sub.callback = NULL;
        s_table[index].sub.user_ctx = NULL;
        s_table[index].generation++;
        status = SAS_STATUS_OK;
    }

    xSemaphoreGive(s_table_lock);
    return status;
}

sas_status_t sas_event_bus_publish(uint32_t event_id, const void *payload, size_t payload_len)
{
    if ((payload == NULL) && (payload_len != 0U)) {
        return SAS_ERR_INVALID_ARG;
    }
    if (payload_len > SAS_EVT_MAX_PAYLOAD_LEN) {
        return SAS_EVT_ERR_PAYLOAD_TOO_LARGE;
    }
    configASSERT(s_initialized);

    if (xSemaphoreTakeRecursive(s_dispatch_lock, SAS_EVT_DISPATCH_WAIT_TICKS) != pdTRUE) {
        return SAS_ERR_TIMEOUT;
    }

    /* Stack-local, not static/shared: a subscriber callback invoked below
     * may itself call sas_event_bus_publish() (the recursive dispatch lock
     * allows it); a shared scratch buffer would let that nested call
     * corrupt this call's in-flight snapshot/payload. */
    uint8_t payload_copy[SAS_EVT_MAX_PAYLOAD_LEN];
    if (payload_len > 0U) {
        (void)memcpy(payload_copy, payload, payload_len);
    }

    sas_event_subscription_t snapshot[SAS_EVT_MAX_SUBSCRIPTIONS];
    size_t match_count = 0U;

    if (xSemaphoreTake(s_table_lock, SAS_EVT_LOCK_WAIT_TICKS) == pdTRUE) {
        for (size_t i = 0U; i < SAS_EVT_MAX_SUBSCRIPTIONS; i++) {
            if (s_table[i].in_use && (s_table[i].sub.event_id == event_id)) {
                snapshot[match_count] = s_table[i].sub;
                match_count++;
            }
        }
        xSemaphoreGive(s_table_lock);
    }

    const sas_event_t event = {
        .event_id = event_id,
        .payload = (payload_len > 0U) ? payload_copy : NULL,
        .payload_len = payload_len,
    };

    for (size_t i = 0U; i < match_count; i++) {
        snapshot[i].callback(&event, snapshot[i].user_ctx);
    }

    xSemaphoreGiveRecursive(s_dispatch_lock);
    return SAS_STATUS_OK;
}

bool sas_event_bus_is_started(void)
{
    return s_started;
}

/* 7. Private functions */

/*! \brief Pack a 0-based slot index and its current generation into an opaque, externally-visible
 * handle. */
static uint32_t make_handle(size_t index, uint16_t generation)
{
    return ((uint32_t)generation << 16) | (uint32_t)(index + 1U);
}

/*!
    \brief   Unpack a handle, validating that its index is in range.
    \return  true if handle decodes to a valid slot index (the caller must
             still check in_use/generation against the live table).
*/
static bool decode_handle(uint32_t handle, size_t *out_index, uint16_t *out_generation)
{
    uint32_t index_plus_one = handle & SAS_EVT_HANDLE_INDEX_MASK;
    if ((index_plus_one == 0U) || (index_plus_one > SAS_EVT_MAX_SUBSCRIPTIONS)) {
        return false;
    }

    *out_index = (size_t)(index_plus_one - 1U);
    *out_generation = (uint16_t)(handle >> 16);
    return true;
}
