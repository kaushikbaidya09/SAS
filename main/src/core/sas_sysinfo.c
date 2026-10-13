//=============================================================================
/*!
    \file    sas_sysinfo.c
    \brief   Implementation of the System Information module.
*/
//=============================================================================

// sas_sysinfo_get_task_snapshot() needs CONFIG_FREERTOS_USE_TRACE_FACILITY=y;
// without it the call returns SAS_ERR_NOT_SUPPORTED.

#include "sas_sysinfo.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

/*! \brief Bounded wait for s_task_lock; the snapshot itself is microsecond-scale. */
#define SAS_SYSINFO_LOCK_WAIT_TICKS pdMS_TO_TICKS(50)

static bool s_initialized     = false;
static bool s_started         = false;
static int64_t s_boot_time_us = 0; ///< Boot timestamp recorded by sas_sysinfo_init(), used to
                                   ///< derive uptime_seconds.

// Not cleared by init() or deinit(): modules that boot before this one are
// recorded first.
static sas_boot_record_t s_boot_records[SAS_SYSINFO_MAX_BOOT_RECORDS];
static uint8_t s_boot_record_count = 0U;

#if (configUSE_TRACE_FACILITY == 1)
static TaskStatus_t s_task_scratch[SAS_SYSINFO_MAX_TASKS]; ///< uxTaskGetSystemState() target.
static SemaphoreHandle_t s_task_lock = NULL;               ///< Guards s_task_scratch.
static StaticSemaphore_t s_task_lock_storage;
#endif

static const char *chip_model_name(esp_chip_model_t model);

/*!
    \brief   Initialize the System Information module.
    \return  SAS_STATUS_OK on success.
*/
sas_status_t sas_sysinfo_init(void)
{
    s_boot_time_us = esp_timer_get_time();

#if (configUSE_TRACE_FACILITY == 1)
    if (s_task_lock == NULL)
    {
        s_task_lock = xSemaphoreCreateMutexStatic(&s_task_lock_storage);
        configASSERT(s_task_lock != NULL);
    }
#endif

    s_started     = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Start active operation of the module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_sysinfo_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    s_started = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Stop active operation without releasing resources.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_sysinfo_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    s_started = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Release all resources acquired by sas_sysinfo_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_sysinfo_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    s_boot_time_us = 0;
    s_started      = false;
    s_initialized  = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Get static chip identity information.
    \param[out] out  Non-NULL destination for the chip info.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out is NULL.
*/
sas_status_t sas_sysinfo_get_chip_info(sas_chip_info_t *out)
{
    if (out == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    esp_chip_info_t info;
    esp_chip_info(&info);

    (void)memset(out, 0, sizeof(*out));
    (void)strncpy(out->chip_model, chip_model_name(info.model), sizeof(out->chip_model) - 1U);
    out->chip_revision = (uint8_t)info.revision;

    uint32_t flash_size = 0U;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK)
    {
        out->flash_size_bytes = flash_size;
    }
    return SAS_STATUS_OK;
}

/*!
    \brief   Get current runtime memory/uptime statistics.
    \param[out] out  Non-NULL destination for the runtime info.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out is NULL.
*/
sas_status_t sas_sysinfo_get_runtime_info(sas_runtime_info_t *out)
{
    if (out == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    out->free_heap_bytes     = esp_get_free_heap_size();
    out->min_free_heap_bytes = esp_get_minimum_free_heap_size();
    out->uptime_seconds      = (uint64_t)((esp_timer_get_time() - s_boot_time_us) / 1000000LL);
    return SAS_STATUS_OK;
}

/*!
    \brief   Get the reason for the most recent chip reset.
    \param[out] out  Non-NULL destination for the reset reason.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out is NULL.
*/
sas_status_t sas_sysinfo_get_reset_reason(esp_reset_reason_t *out)
{
    if (out == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    *out = esp_reset_reason();
    return SAS_STATUS_OK;
}

/*!
    \brief   Get a point-in-time snapshot of all active FreeRTOS tasks.
    \param[out] out_array    Non-NULL destination array of at least max_entries
                             elements.
    \param[in]  max_entries  Capacity of out_array, in elements.
    \param[out] out_count    Non-NULL destination for the number of entries
                             written (or, on SAS_SYSINFO_ERR_BUFFER_TOO_SMALL,
                             the number of entries required).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out_array or
             out_count is NULL; SAS_SYSINFO_ERR_BUFFER_TOO_SMALL if max_entries
             is smaller than the active task count; SAS_ERR_TIMEOUT if another
             snapshot is in progress.
*/
sas_status_t sas_sysinfo_get_task_snapshot(sas_task_snapshot_t *out_array, uint8_t max_entries,
                                           uint8_t *out_count)
{
    if ((out_array == NULL) || (out_count == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

#if (configUSE_TRACE_FACILITY == 1)
    if (s_task_lock == NULL)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_task_lock, SAS_SYSINFO_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status    = SAS_STATUS_OK;
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if ((task_count > max_entries) || (task_count > SAS_SYSINFO_MAX_TASKS))
    {
        // Report the required size without partially filling out_array.
        *out_count = (task_count > UINT8_MAX) ? UINT8_MAX : (uint8_t)task_count;
        status     = SAS_SYSINFO_ERR_BUFFER_TOO_SMALL;
    }
    else
    {
        UBaseType_t written = uxTaskGetSystemState(s_task_scratch, SAS_SYSINFO_MAX_TASKS, NULL);
        for (UBaseType_t i = 0U; i < written; i++)
        {
            (void)strncpy(out_array[i].task_name, s_task_scratch[i].pcTaskName,
                          sizeof(out_array[i].task_name) - 1U);
            out_array[i].task_name[sizeof(out_array[i].task_name) - 1U] = '\0';
            out_array[i].state                 = (uint8_t)s_task_scratch[i].eCurrentState;
            out_array[i].stack_high_water_mark = (uint32_t)s_task_scratch[i].usStackHighWaterMark;
        }
        *out_count = (uint8_t)written;
    }

    xSemaphoreGive(s_task_lock);
    return status;
#else
    (void)max_entries;
    *out_count = 0U;
    return SAS_ERR_NOT_SUPPORTED;
#endif
}

/*!
    \brief   Record one module's boot outcome. Called by the boot sequence
             (main/src/main.c) once per module, in boot order.
    \param[in] module_name  Non-NULL module name with static storage duration
                            (stored by pointer, not copied).
    \param[in] status       Outcome to record for the module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if module_name is
             NULL; SAS_ERR_NO_MEM if SAS_SYSINFO_MAX_BOOT_RECORDS entries are
             already recorded.
*/
sas_status_t sas_sysinfo_record_boot_status(const char *module_name, sas_boot_status_t status)
{
    // Works before sas_sysinfo_init(), because modules that boot earlier are
    // recorded too.

    if (module_name == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (s_boot_record_count >= SAS_SYSINFO_MAX_BOOT_RECORDS)
    {
        return SAS_ERR_NO_MEM;
    }

    s_boot_records[s_boot_record_count].module_name = module_name;
    s_boot_records[s_boot_record_count].status      = status;
    s_boot_record_count++;
    return SAS_STATUS_OK;
}

/*!
    \brief   Get the per-module boot status report, in boot order.
    \param[out] out_array    Non-NULL destination array of at least max_entries
                             elements.
    \param[in]  max_entries  Capacity of out_array, in elements.
    \param[out] out_count    Non-NULL destination for the number of entries
                             written.
    \return  SAS_STATUS_OK on success (the report is truncated to max_entries
             if smaller than the recorded count); SAS_ERR_INVALID_ARG if
             out_array or out_count is NULL.
*/
sas_status_t sas_sysinfo_get_boot_status(sas_boot_record_t *out_array, uint8_t max_entries,
                                         uint8_t *out_count)
{
    if ((out_array == NULL) || (out_count == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    uint8_t count = (s_boot_record_count < max_entries) ? s_boot_record_count : max_entries;
    for (uint8_t i = 0U; i < count; i++)
    {
        out_array[i] = s_boot_records[i];
    }
    *out_count = count;
    return SAS_STATUS_OK;
}

/*!
    \brief   Map esp_chip_model_t to a human-readable model string.
*/
static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model)
    {
        case CHIP_ESP32:
            return "ESP32";
        case CHIP_ESP32S2:
            return "ESP32-S2";
        case CHIP_ESP32S3:
            return "ESP32-S3";
        case CHIP_ESP32C3:
            return "ESP32-C3";
        case CHIP_ESP32C6:
            return "ESP32-C6";
        case CHIP_ESP32H2:
            return "ESP32-H2";
        default:
            return "Unknown";
    }
}
