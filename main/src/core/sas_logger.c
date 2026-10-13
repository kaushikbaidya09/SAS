//=============================================================================
/*!
    \file    sas_logger.c
    \brief   Implementation of the Logger module.
*/
//=============================================================================

#include "sas_logger.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

#include "sas_event_bus.h"

/*! \brief Ring buffer size in bytes for the persisted-logging path. */
#define SAS_LOGGER_RING_BUFFER_SIZE (2048U)
/*! \brief Max formatted line length (timestamp+level+tag+message) placed in the ring buffer. */
#define SAS_LOGGER_LINE_MAX_LEN (160U)

#define SAS_LOGGER_DRAIN_TASK_STACK_DEPTH (4096U) ///< Drain task stack depth, in StackType_t units.
/*! \brief Low-priority background drain task. */
#define SAS_LOGGER_DRAIN_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)

/*! \brief Bounded wait for the internal state mutex; critical sections are microsecond-scale. */
#define SAS_LOGGER_LOCK_WAIT_TICKS pdMS_TO_TICKS(50)
/*! \brief Drain task's ring-buffer receive timeout; bounds shutdown latency. */
#define SAS_LOGGER_DRAIN_BLOCK_TICKS pdMS_TO_TICKS(200)
/*! \brief Bounded wait in sas_logger_stop() for the drain task to exit. */
#define SAS_LOGGER_STOP_WAIT_TICKS pdMS_TO_TICKS(1000)

static bool s_initialized = false;
static bool s_started     = false;

static sas_logger_config_t s_cfg;
static sas_log_level_t s_min_level = SAS_LOG_LEVEL_ERROR;
static uint32_t s_dropped_count    = 0U;

static SemaphoreHandle_t s_lock = NULL;
static StaticSemaphore_t s_lock_storage;

static RingbufHandle_t s_ring_buf = NULL;
static StaticRingbuffer_t s_ring_buf_struct;
static uint8_t s_ring_buf_storage[SAS_LOGGER_RING_BUFFER_SIZE];

static TaskHandle_t s_drain_task_handle = NULL;
static StaticTask_t s_drain_task_tcb;
static StackType_t s_drain_task_stack[SAS_LOGGER_DRAIN_TASK_STACK_DEPTH];
static SemaphoreHandle_t s_drain_stopped_sem = NULL;
static StaticSemaphore_t s_drain_stopped_sem_storage;
static volatile bool s_drain_task_should_run = false;

// Recent log entries for sas_log_get_entries(), kept whether or not
// persistence is enabled. A circular buffer guarded by s_lock; the oldest
// entry is overwritten when it is full.
static sas_log_entry_t s_history[SAS_LOGGER_MAX_HISTORY_ENTRIES];
static uint16_t s_history_head  = 0U; ///< Index of the oldest entry currently stored.
static uint16_t s_history_count = 0U; ///< Number of valid entries in s_history (<= capacity).

static sas_status_t log_write(sas_log_level_t level, const char *tag, const char *fmt,
                              va_list args);
static esp_log_level_t level_to_esp_log_level(sas_log_level_t level);
static char level_to_char(sas_log_level_t level);
static void drain_task_fn(void *arg);
static void record_history_and_publish(const sas_log_entry_t *entry);

/*!
    \brief   Initialize the Logger module.
    \param[in] cfg  Non-NULL configuration; copied internally.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if cfg is NULL;
             SAS_ERR_INVALID_STATE if already initialized.
*/
sas_status_t sas_logger_init(const sas_logger_config_t *cfg)
{
    if (cfg == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    s_cfg           = *cfg;
    s_min_level     = cfg->min_level;
    s_dropped_count = 0U;

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    configASSERT(s_lock != NULL);

    if (s_cfg.persist_enabled)
    {
        s_ring_buf = xRingbufferCreateStatic(SAS_LOGGER_RING_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF,
                                             s_ring_buf_storage, &s_ring_buf_struct);
        configASSERT(s_ring_buf != NULL);

        s_drain_stopped_sem = xSemaphoreCreateBinaryStatic(&s_drain_stopped_sem_storage);
        configASSERT(s_drain_stopped_sem != NULL);
    }

    s_started     = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Start active logging operation (ring buffer drain task, if
             persist_enabled).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized
             or already started.
*/
sas_status_t sas_logger_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_cfg.persist_enabled && (s_ring_buf != NULL))
    {
        s_drain_task_should_run = true;
        s_drain_task_handle     = xTaskCreateStatic(
            drain_task_fn, "sas_logger_drain", SAS_LOGGER_DRAIN_TASK_STACK_DEPTH, NULL,
            SAS_LOGGER_DRAIN_TASK_PRIORITY, s_drain_task_stack, &s_drain_task_tcb);
        configASSERT(s_drain_task_handle != NULL);
    }

    s_started = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Stop active logging operation without releasing resources.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_logger_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_drain_task_handle != NULL)
    {
        s_drain_task_should_run = false;
        (void)xSemaphoreTake(s_drain_stopped_sem, SAS_LOGGER_STOP_WAIT_TICKS);
        s_drain_task_handle = NULL;
    }

    s_started = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Release all resources acquired by sas_logger_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized
             or still started (call sas_logger_stop() first).
*/
sas_status_t sas_logger_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_drain_stopped_sem != NULL)
    {
        vSemaphoreDelete(s_drain_stopped_sem);
        s_drain_stopped_sem = NULL;
    }
    if (s_ring_buf != NULL)
    {
        vRingbufferDelete(s_ring_buf);
        s_ring_buf = NULL;
    }
    if (s_lock != NULL)
    {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }

    s_min_level     = SAS_LOG_LEVEL_ERROR;
    s_dropped_count = 0U;
    (void)memset(&s_cfg, 0, sizeof(s_cfg));
    (void)memset(s_history, 0, sizeof(s_history));
    s_history_head  = 0U;
    s_history_count = 0U;
    s_initialized   = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Tagged ERROR entry point; call sas_log_error() instead.
    \param[in] tag  Non-NULL module tag.
    \param[in] fmt  Non-NULL printf-style format string.
    \return  SAS_STATUS_OK if accepted (SAS_LOGGER_ERR_RING_FULL is never
             returned; a full ring buffer just increments the drop count).
*/
sas_status_t sas_log_emit_error(const char *tag, const char *fmt, ...)
{
    if ((tag == NULL) || (fmt == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    va_list args;
    va_start(args, fmt);
    sas_status_t status = log_write(SAS_LOG_LEVEL_ERROR, tag, fmt, args);
    va_end(args);
    return status;
}

/*!
    \brief   Tagged WARN-severity entry point; see sas_log_emit_error().
*/
sas_status_t sas_log_emit_warn(const char *tag, const char *fmt, ...)
{
    if ((tag == NULL) || (fmt == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    va_list args;
    va_start(args, fmt);
    sas_status_t status = log_write(SAS_LOG_LEVEL_WARN, tag, fmt, args);
    va_end(args);
    return status;
}

/*!
    \brief   Tagged INFO-severity entry point; see sas_log_emit_error().
*/
sas_status_t sas_log_emit_info(const char *tag, const char *fmt, ...)
{
    if ((tag == NULL) || (fmt == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    va_list args;
    va_start(args, fmt);
    sas_status_t status = log_write(SAS_LOG_LEVEL_INFO, tag, fmt, args);
    va_end(args);
    return status;
}

/*!
    \brief   Tagged DEBUG-severity entry point; see sas_log_emit_error().
*/
sas_status_t sas_log_emit_debug(const char *tag, const char *fmt, ...)
{
    if ((tag == NULL) || (fmt == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    va_list args;
    va_start(args, fmt);
    sas_status_t status = log_write(SAS_LOG_LEVEL_DEBUG, tag, fmt, args);
    va_end(args);
    return status;
}

/*!
    \brief   Tagged VERBOSE-severity entry point; see sas_log_emit_error().
*/
sas_status_t sas_log_emit_verbose(const char *tag, const char *fmt, ...)
{
    if ((tag == NULL) || (fmt == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    va_list args;
    va_start(args, fmt);
    sas_status_t status = log_write(SAS_LOG_LEVEL_VERBOSE, tag, fmt, args);
    va_end(args);
    return status;
}

/*!
    \brief   Set the minimum severity level emitted.
    \param[in] level  New minimum level.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if level is out of
             range.
*/
sas_status_t sas_logger_set_level(sas_log_level_t level)
{
    if (level > SAS_LOG_LEVEL_VERBOSE)
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, SAS_LOGGER_LOCK_WAIT_TICKS) == pdTRUE)
    {
        s_min_level = level;
        xSemaphoreGive(s_lock);
    }
    return SAS_STATUS_OK;
}

/*!
    \brief   Get the current minimum severity level.
    \return  Current sas_log_level_t.
*/
sas_log_level_t sas_logger_get_level(void)
{
    sas_log_level_t level = SAS_LOG_LEVEL_ERROR;

    if (s_initialized && (xSemaphoreTake(s_lock, SAS_LOGGER_LOCK_WAIT_TICKS) == pdTRUE))
    {
        level = s_min_level;
        xSemaphoreGive(s_lock);
    }
    return level;
}

/*!
    \brief   Get the count of entries dropped due to a full ring buffer.
    \return  Cumulative dropped-entry count since init.
*/
uint32_t sas_logger_get_dropped_count(void)
{
    uint32_t count = 0U;

    if (s_initialized && (xSemaphoreTake(s_lock, SAS_LOGGER_LOCK_WAIT_TICKS) == pdTRUE))
    {
        count = s_dropped_count;
        xSemaphoreGive(s_lock);
    }
    return count;
}

/*!
    \brief   Copy out the most recent entries from the in-RAM history, oldest
             first.
    \param[out] out_array    Non-NULL destination array of at least max_entries
                             elements.
    \param[in]  max_entries  Capacity of out_array, in entries.
    \param[out] out_count    Non-NULL; receives the number of entries written.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_ARG (bad args);
             SAS_ERR_INVALID_STATE (not initialized); SAS_ERR_TIMEOUT (lock
             busy - *out_count left unset).
*/
sas_status_t sas_log_get_entries(sas_log_entry_t *out_array, uint16_t max_entries,
                                 uint16_t *out_count)
{
    if ((out_array == NULL) || (out_count == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, SAS_LOGGER_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    uint16_t available = s_history_count;
    uint16_t to_copy   = (available < max_entries) ? available : max_entries;
    // When out_array is smaller than the history, skip the oldest entries so
    // that the most recent ones are returned, oldest first.
    uint16_t skip = (uint16_t)(available - to_copy);
    for (uint16_t i = 0U; i < to_copy; i++)
    {
        uint16_t src_index =
            (uint16_t)((s_history_head + skip + i) % SAS_LOGGER_MAX_HISTORY_ENTRIES);
        out_array[i] = s_history[src_index];
    }

    xSemaphoreGive(s_lock);

    *out_count = to_copy;
    return SAS_STATUS_OK;
}

/*!
    \brief   Format and route one log entry; the single entry point used by all
             sas_log_emit_*() wrappers.
*/
static sas_status_t log_write(sas_log_level_t level, const char *tag, const char *fmt, va_list args)
{
    configASSERT(s_initialized);

    sas_log_level_t min_level = SAS_LOG_LEVEL_ERROR;
    bool persist              = false;

    if (xSemaphoreTake(s_lock, SAS_LOGGER_LOCK_WAIT_TICKS) == pdTRUE)
    {
        min_level = s_min_level;
        persist   = s_cfg.persist_enabled && (s_ring_buf != NULL);
        xSemaphoreGive(s_lock);
    }

    if (level > min_level)
    {
        return SAS_STATUS_OK; // below the minimum level (SAS-LOG-FR-003)
    }

    // Always record the entry in the history (SAS-LOG-FR-004). The arguments
    // are copied because they are used again below.
    {
        sas_log_entry_t entry;
        (void)memset(&entry, 0, sizeof(entry));
        entry.timestamp_us = esp_timer_get_time();
        entry.level        = level;
        (void)strncpy(entry.tag, tag, sizeof(entry.tag) - 1U);
        entry.tag[sizeof(entry.tag) - 1U] = '\0';

        va_list history_args;
        va_copy(history_args, args);
        (void)vsnprintf(entry.message, sizeof(entry.message), fmt, history_args);
        va_end(history_args);

        record_history_and_publish(&entry);
    }

    if (!persist)
    {
        // esp_log_writev() prints only the format string, so the usual ESP-IDF
        // line ("W (1234) tag: message") is built here.
        char message[SAS_LOGGER_LINE_MAX_LEN];
        (void)vsnprintf(message, sizeof(message), fmt, args);
        esp_log_write(level_to_esp_log_level(level), tag, "%c (%u) %s: %s\n", level_to_char(level),
                      (unsigned int)esp_log_timestamp(), tag, message);
        return SAS_STATUS_OK;
    }

    char line[SAS_LOGGER_LINE_MAX_LEN];
    int hdr_written = snprintf(line, sizeof(line), "%u %c %s: ", (unsigned int)esp_log_timestamp(),
                               level_to_char(level), tag);
    size_t pos = ((hdr_written > 0) && ((size_t)hdr_written < sizeof(line))) ? (size_t)hdr_written
                                                                             : (sizeof(line) - 1U);

    int msg_written = vsnprintf(&line[pos], sizeof(line) - pos, fmt, args);
    size_t total    = pos + ((msg_written > 0) ? (size_t)msg_written : 0U);
    if (total >= sizeof(line))
    {
        total = sizeof(line) - 1U;
    }

    if (xRingbufferSend(s_ring_buf, line, total, 0) != pdTRUE)
    {
        // Never block the caller: drop the entry and count it
        // (SAS-LOG-NFR-003).
        if (xSemaphoreTake(s_lock, SAS_LOGGER_LOCK_WAIT_TICKS) == pdTRUE)
        {
            s_dropped_count++;
            xSemaphoreGive(s_lock);
        }
    }

    return SAS_STATUS_OK; // a dropped entry is not reported as an error
}

/*!
    \brief   Map sas_log_level_t to the matching esp_log_level_t for the
             console sink.
*/
static esp_log_level_t level_to_esp_log_level(sas_log_level_t level)
{
    switch (level)
    {
        case SAS_LOG_LEVEL_ERROR:
            return ESP_LOG_ERROR;
        case SAS_LOG_LEVEL_WARN:
            return ESP_LOG_WARN;
        case SAS_LOG_LEVEL_INFO:
            return ESP_LOG_INFO;
        case SAS_LOG_LEVEL_DEBUG:
            return ESP_LOG_DEBUG;
        case SAS_LOG_LEVEL_VERBOSE:
        default:
            return ESP_LOG_VERBOSE;
    }
}

/*!
    \brief   Map sas_log_level_t to a single-character severity marker for
             persisted lines.
*/
static char level_to_char(sas_log_level_t level)
{
    switch (level)
    {
        case SAS_LOG_LEVEL_ERROR:
            return 'E';
        case SAS_LOG_LEVEL_WARN:
            return 'W';
        case SAS_LOG_LEVEL_INFO:
            return 'I';
        case SAS_LOG_LEVEL_DEBUG:
            return 'D';
        case SAS_LOG_LEVEL_VERBOSE:
        default:
            return 'V';
    }
}

/*!
    \brief   Append entry to the circular history under s_lock, then publish
             SAS_EVENT_LOG_ENTRY outside the lock.
*/
static void record_history_and_publish(const sas_log_entry_t *entry)
{
    // Entries logged before the Event Bus has started are recorded but not
    // published.

    if (xSemaphoreTake(s_lock, SAS_LOGGER_LOCK_WAIT_TICKS) == pdTRUE)
    {
        uint16_t write_index;
        if (s_history_count < SAS_LOGGER_MAX_HISTORY_ENTRIES)
        {
            write_index =
                (uint16_t)((s_history_head + s_history_count) % SAS_LOGGER_MAX_HISTORY_ENTRIES);
            s_history_count++;
        }
        else
        {
            write_index    = s_history_head;
            s_history_head = (uint16_t)((s_history_head + 1U) % SAS_LOGGER_MAX_HISTORY_ENTRIES);
        }
        s_history[write_index] = *entry;
        xSemaphoreGive(s_lock);
    }

    if (sas_event_bus_is_started())
    {
        sas_status_t publish_status =
            sas_event_bus_publish(SAS_EVENT_LOG_ENTRY, entry, sizeof(*entry));
        if (publish_status != SAS_STATUS_OK)
        {
            ESP_LOGW("sas_logger", "SAS_EVENT_LOG_ENTRY publish failed (0x%x)",
                     (unsigned int)publish_status);
        }
    }
}

/*!
    \brief   Drain task body: echoes ring-buffer entries to the console for
             now; swap in a real file write once File Manager can append.
*/
static void drain_task_fn(void *arg)
{
    (void)arg;

    while (s_drain_task_should_run)
    {
        size_t item_len = 0U;
        uint8_t *item =
            (uint8_t *)xRingbufferReceive(s_ring_buf, &item_len, SAS_LOGGER_DRAIN_BLOCK_TICKS);

        if (item != NULL)
        {
            esp_log_write(ESP_LOG_INFO, "sas_logger_drain", "%.*s", (int)item_len,
                          (const char *)item);
            vRingbufferReturnItem(s_ring_buf, (void *)item);
        }
    }

    xSemaphoreGive(s_drain_stopped_sem);
    vTaskDelete(NULL);
}
