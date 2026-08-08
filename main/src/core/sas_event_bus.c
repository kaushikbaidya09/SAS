/*!
    \file    sas_event_bus.c
    \brief   Implementation skeleton for the Event Bus module.

    \description
    See sas_event_bus.h for the public contract. This file currently contains
    only the lifecycle and dispatch API stubs; behaviour is filled in per
    module during implementation, in Development Order.

    \dependencies
    (none)
*/

/* 1. Includes */
#include "sas_event_bus.h"

#include <stdbool.h>
#include <stddef.h>

/* 2. Private macros */
/* (none yet) */

/* 3. Private typedefs */
/* (none yet: internal subscription-table/mutex/dispatch-lock state to be
 * added during implementation) */

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started     = false;

/* 5. Forward declarations */
/* (none yet) */

/* 6. Public functions */

sas_status_t sas_event_bus_init(void)
{
    /* TODO:
     * 1. Allocate the static array of SAS_EVT_MAX_SUBSCRIPTIONS
     *    sas_event_subscription_t entries (static allocation only).
     * 2. Create the subscription-table mutex.
     * 3. Create the separate dispatch lock. */
    s_initialized = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_event_bus_start(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Mark the bus ready to accept publish() calls.
     * 2. No dedicated task is created since dispatch is synchronous on the
     *    caller's task. */
    s_started = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_event_bus_stop(void)
{
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Stop accepting publish() calls.
     * 2. Leave the subscription table, mutex, and dispatch lock acquired
     *    in sas_event_bus_init() untouched (stop() must not release
     *    resources). */
    s_started = false;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_event_bus_deinit(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Release the subscription-table mutex and dispatch lock.
     * 2. Release any other resources acquired in sas_event_bus_init().
     * 3. Reset module state to uninitialized. */
    s_initialized = false;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_event_bus_subscribe(uint32_t event_id,
                                      sas_event_callback_fn cb,
                                      void *user_ctx,
                                      uint32_t *out_subscription_handle)
{
    if ((cb == NULL) || (out_subscription_handle == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Under the subscription-table mutex, find a free slot in the
     *    static SAS_EVT_MAX_SUBSCRIPTIONS array.
     * 2. Store event_id/cb/user_ctx in that slot.
     * 3. Hand back a stable handle via out_subscription_handle.
     * 4. Return SAS_EVT_ERR_TABLE_FULL if no free slot exists. */
    (void)event_id;
    (void)user_ctx;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_event_bus_unsubscribe(uint32_t subscription_handle)
{
    /* TODO:
     * 1. Under the subscription-table mutex (distinct from the dispatch
     *    lock held during publish()), clear the slot identified by
     *    subscription_handle.
     * 2. Ensure unsubscribing from within a subscriber's own callback
     *    neither corrupts the table nor skips other subscribers still
     *    pending dispatch.
     * 3. Return SAS_ERR_INVALID_ARG if the handle does not identify an
     *    active subscription. */
    (void)subscription_handle;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_event_bus_publish(uint32_t event_id, const void *payload,
                                    size_t payload_len)
{
    if ((payload == NULL) && (payload_len != 0U)) {
        return SAS_ERR_INVALID_ARG;
    }
    if (payload_len > SAS_EVT_MAX_PAYLOAD_LEN) {
        return SAS_EVT_ERR_PAYLOAD_TOO_LARGE;
    }

    /* TODO:
     * 1. Copy payload_len bytes into a per-publish scratch buffer bounded
     *    by SAS_EVT_MAX_PAYLOAD_LEN.
     * 2. Under the dispatch lock (held separately from the
     *    subscription-table mutex so a callback that subscribes/
     *    unsubscribes does not deadlock), walk the subscription table.
     * 3. Invoke every currently subscribed callback for event_id exactly
     *    once, synchronously on the caller's task.
     * 4. Reclaim the scratch buffer on return; publish() is synchronous
     *    and bounded by subscriber execution time. */
    (void)event_id;
    return SAS_ERR_NOT_SUPPORTED;
}

/* 7. Private functions */
/* (none yet) */
