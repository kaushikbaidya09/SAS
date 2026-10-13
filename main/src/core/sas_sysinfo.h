//=============================================================================
/*!
    \file    sas_sysinfo.h
    \brief   Public interface for the System Information module.
*/
//=============================================================================

// Read-only reporting of chip identity, memory and uptime statistics, the last
// reset reason, a task snapshot and the per-module boot status.

#ifndef SAS_SYSINFO_H
#define SAS_SYSINFO_H

#include <stdint.h>

#include "esp_system.h"        // esp_reset_reason_t
#include "freertos/FreeRTOS.h" // configMAX_TASK_NAME_LEN

#include "sas_status.h"

/*! \brief Static chip identity, sampled from esp_chip_info(). */
typedef struct
{
    char chip_model[16];       ///< Human-readable chip model string.
    uint8_t chip_revision;     ///< Silicon revision number.
    uint32_t flash_size_bytes; ///< Detected flash size, in bytes.
} sas_chip_info_t;

/*! \brief Runtime memory/uptime snapshot. */
typedef struct
{
    uint32_t free_heap_bytes;     ///< Current free heap, in bytes.
    uint32_t min_free_heap_bytes; ///< Minimum free heap ever observed, in bytes.
    uint64_t uptime_seconds;      ///< Seconds elapsed since module init.
} sas_runtime_info_t;

/*! \brief Single task's state, as reported by uxTaskGetSystemState(). */
typedef struct
{
    char task_name[configMAX_TASK_NAME_LEN]; ///< Task name (NUL-terminated).
    uint8_t state;                           ///< eTaskState value of the task.
    uint32_t stack_high_water_mark;          ///< Minimum remaining stack, in words.
} sas_task_snapshot_t;

/*! \brief Boot outcome of one module. */
typedef enum
{
    SAS_BOOT_STATUS_OK = 0,
    SAS_BOOT_STATUS_FAILED,
    SAS_BOOT_STATUS_SKIPPED_DEPENDENCY,
} sas_boot_status_t;

/*! \brief One boot-sequence record. */
typedef struct
{
    const char *module_name;  ///< Module name, e.g. "logger".
    sas_boot_status_t status; ///< Outcome recorded for this module.
} sas_boot_record_t;

#define SAS_SYSINFO_MAX_BOOT_RECORDS (16U) ///< Capacity of the boot status report.
#define SAS_SYSINFO_MAX_TASKS        (40U) ///< Largest task count sas_sysinfo_get_task_snapshot() handles.

#define SAS_SYSINFO_ERR_BASE (0x8300U) ///< Custom status base for this module.
/*! \brief out_array smaller than the active task count; out_count set to the required size. */
#define SAS_SYSINFO_ERR_BUFFER_TOO_SMALL (SAS_SYSINFO_ERR_BASE + 1U)

sas_status_t sas_sysinfo_init(void);

sas_status_t sas_sysinfo_start(void);

sas_status_t sas_sysinfo_stop(void);

sas_status_t sas_sysinfo_deinit(void);

sas_status_t sas_sysinfo_get_chip_info(sas_chip_info_t *out);

sas_status_t sas_sysinfo_get_runtime_info(sas_runtime_info_t *out);

sas_status_t sas_sysinfo_get_reset_reason(esp_reset_reason_t *out);

sas_status_t sas_sysinfo_get_task_snapshot(sas_task_snapshot_t *out_array, uint8_t max_entries,
                                           uint8_t *out_count);

sas_status_t sas_sysinfo_record_boot_status(const char *module_name, sas_boot_status_t status);

sas_status_t sas_sysinfo_get_boot_status(sas_boot_record_t *out_array, uint8_t max_entries,
                                         uint8_t *out_count);

#endif /* SAS_SYSINFO_H */
