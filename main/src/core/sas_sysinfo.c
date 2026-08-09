/*!
    \file    sas_sysinfo.c
    \brief   Implementation skeleton for the System Information module.

    \description
    See sas_sysinfo.h for the public contract. This file currently contains
    only the lifecycle and getter API stubs; behaviour is filled in per
    module during implementation, in Development Order.

    \dependencies
    (none: System Information has no dependencies on other SAS modules).
*/

/* 1. Includes */
#include "sas_sysinfo.h"

#include <stdbool.h>
#include <stddef.h>

/* 2. Private macros */
/* (none yet: getters call ESP-IDF APIs directly, no module-specific sizing) */

/* 3. Private typedefs */
/* (none yet: no internal state beyond the boot timestamp below) */

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;
static int64_t s_boot_time_us = 0; ///< Boot timestamp recorded by sas_sysinfo_init(), used to
                                   ///< derive uptime_seconds (no other state retained).

/* 5. Forward declarations */
/* (none: no private helper functions yet) */

/* 6. Public functions */

sas_status_t sas_sysinfo_init(void)
{
    /* TODO:
     * 1. Record the current timestamp into s_boot_time_us (e.g. via
     *    esp_timer_get_time()) for later uptime_seconds computation.
     * 2. Retain no other internal state. */
    s_initialized = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_sysinfo_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. No resources to start; module is stateless besides the boot
     *    timestamp. */
    s_started = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_sysinfo_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. No active resources to stop. */
    s_started = false;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_sysinfo_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Reset s_boot_time_us.
     * 2. No other resources were acquired by sas_sysinfo_init(). */
    s_initialized = false;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_sysinfo_get_chip_info(sas_chip_info_t *out)
{
    if (out == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Call esp_chip_info().
     * 2. Populate chip_model, chip_revision, and flash_size_bytes from the
     *    result.
     * 3. This is a thin read-only wrapper and must complete within 50 ms. */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_sysinfo_get_runtime_info(sas_runtime_info_t *out)
{
    if (out == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Call esp_get_free_heap_size() for free_heap_bytes.
     * 2. Call esp_get_minimum_free_heap_size() for min_free_heap_bytes.
     * 3. Derive uptime_seconds from s_boot_time_us.
     * 4. Must complete within 50 ms. */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_sysinfo_get_reset_reason(esp_reset_reason_t *out)
{
    if (out == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Call esp_reset_reason().
     * 2. Copy the result into *out.
     * 3. Must complete within 50 ms. */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_sysinfo_get_task_snapshot(sas_task_snapshot_t *out_array, uint8_t max_entries,
                                           uint8_t *out_count)
{
    if ((out_array == NULL) || (out_count == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Call uxTaskGetSystemState() to enumerate active tasks.
     * 2. If the active task count exceeds max_entries, set *out_count to
     *    the required size and return SAS_SYSINFO_ERR_BUFFER_TOO_SMALL
     *    without partially filling out_array.
     * 3. Otherwise populate out_array (task_name, state,
     *    stack_high_water_mark per task) and set *out_count to the number
     *    of entries written.
     * 4. That count must match uxTaskGetNumberOfTasks() at call time. */
    return SAS_ERR_NOT_SUPPORTED;
}

/* 7. Private functions */
/* (none yet: all getters above are direct stubs; private helpers may be
 * introduced during implementation) */
