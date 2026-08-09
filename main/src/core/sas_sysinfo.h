/*!
    \file    sas_sysinfo.h
    \brief   Public interface for the System Information module.

    \description
    Read-only reporting of chip identity, runtime memory/uptime statistics,
    last reset reason, and a point-in-time task snapshot. All getters are
    thin wrappers over ESP-IDF system APIs; the module retains no state
    beyond an init-time boot timestamp used to compute uptime.

    \dependencies
    (none: System Information is a Layer-1 module with no compile-time
    header deps).
*/

#pragma once

/* 1. Standard includes */
#include <stdint.h>

/* 2. ESP-IDF includes */
#include "esp_system.h"        /* esp_reset_reason_t */
#include "freertos/FreeRTOS.h" /* configMAX_TASK_NAME_LEN */

/* 3. Project includes */
#include "sas_status.h"

/* 4. Public types */

/*! Static chip identity, sampled from esp_chip_info(). */
typedef struct
{
    char chip_model[16];       ///< Human-readable chip model string.
    uint8_t chip_revision;     ///< Silicon revision number.
    uint32_t flash_size_bytes; ///< Detected flash size, in bytes.
} sas_chip_info_t;

/*! Runtime memory/uptime snapshot. */
typedef struct
{
    uint32_t free_heap_bytes;     ///< Current free heap, in bytes.
    uint32_t min_free_heap_bytes; ///< Minimum free heap ever observed, in bytes.
    uint64_t uptime_seconds;      ///< Seconds elapsed since module init.
} sas_runtime_info_t;

/*! Single task's state, as reported by uxTaskGetSystemState(). */
typedef struct
{
    char task_name[configMAX_TASK_NAME_LEN]; ///< Task name (NUL-terminated).
    uint8_t state;                           ///< eTaskState value of the task.
    uint32_t stack_high_water_mark;          ///< Minimum remaining stack, in words.
} sas_task_snapshot_t;

/* 5. Public constants */

#define SAS_SYSINFO_ERR_BASE (0x8300U) ///< Custom status base for this module.
#define SAS_SYSINFO_ERR_BUFFER_TOO_SMALL \
    (SAS_SYSINFO_ERR_BASE +              \
     1U) ///< out_array smaller than the active task count; out_count set to the required size.

/* 6. Public API declarations */

/*!
    \brief   Initialize the System Information module.
    \return  SAS_STATUS_OK on success.
*/
sas_status_t sas_sysinfo_init(void);

/*!
    \brief   Start active operation of the module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_sysinfo_start(void);

/*!
    \brief   Stop active operation without releasing resources.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_sysinfo_stop(void);

/*!
    \brief   Release all resources acquired by sas_sysinfo_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_sysinfo_deinit(void);

/*!
    \brief   Get static chip identity information.
    \param   out Non-NULL destination for the chip info.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out is NULL.
*/
sas_status_t sas_sysinfo_get_chip_info(sas_chip_info_t *out);

/*!
    \brief   Get current runtime memory/uptime statistics.
    \param   out Non-NULL destination for the runtime info.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out is NULL.
*/
sas_status_t sas_sysinfo_get_runtime_info(sas_runtime_info_t *out);

/*!
    \brief   Get the reason for the most recent chip reset.
    \param   out Non-NULL destination for the reset reason.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out is NULL.
*/
sas_status_t sas_sysinfo_get_reset_reason(esp_reset_reason_t *out);

/*!
    \brief   Get a point-in-time snapshot of all active FreeRTOS tasks.
    \param   out_array    Non-NULL destination array of at least max_entries
                          elements.
    \param   max_entries  Capacity of out_array, in elements.
    \param   out_count    Non-NULL destination for the number of entries
                          written (or, on SAS_SYSINFO_ERR_BUFFER_TOO_SMALL,
                          the number of entries required).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out_array or
             out_count is NULL; SAS_SYSINFO_ERR_BUFFER_TOO_SMALL if
             max_entries is smaller than the active task count.
*/
sas_status_t sas_sysinfo_get_task_snapshot(sas_task_snapshot_t *out_array, uint8_t max_entries,
                                           uint8_t *out_count);

/*
 * TODO: sas_sysinfo_get_boot_status() — a Boot Sequencing extension to this
 * module adds a per-stage boot status report, but the shape of the
 * out-parameter (array of stage/status pairs, count, enum values, etc.) is
 * not yet fixed. Declare this function once that shape is finalized; do not
 * guess a signature here.
 *
 * sas_status_t sas_sysinfo_get_boot_status(void);
 */
