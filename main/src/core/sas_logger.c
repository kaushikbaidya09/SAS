/*!
    \file    sas_logger.c
    \brief   Implementation of the Logger module.
*/

/* 1. Includes */
#include "sas_logger.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

/* 2. Private macros */
#define SAS_LOGGER_RING_BUFFER_SIZE \
    (2048U) ///< Ring buffer size in bytes for the persisted-logging path.
#define SAS_LOGGER_LINE_MAX_LEN \
    (160U) ///< Max formatted line length (timestamp+level+tag+message) placed in the ring buffer.

/* Placeholder tunables; revisit once the drain task's real stack
 * high-water mark is measured via the System Information module. */
#define SAS_LOGGER_DRAIN_TASK_STACK_DEPTH (4096U) ///< Drain task stack depth, in StackType_t units.
#define SAS_LOGGER_DRAIN_TASK_PRIORITY \
    (tskIDLE_PRIORITY + 1U) ///< Low-priority background drain task.

#define SAS_LOGGER_LOCK_WAIT_TICKS \
    pdMS_TO_TICKS(50) ///< Bounded wait for the internal state mutex; critical sections are
                      ///< microsecond-scale.
#define SAS_LOGGER_DRAIN_BLOCK_TICKS \
    pdMS_TO_TICKS(200) ///< Drain task's ring-buffer receive timeout; bounds shutdown latency.
#define SAS_LOGGER_STOP_WAIT_TICKS \
    pdMS_TO_TICKS(1000) ///< Bounded wait in sas_logger_stop() for the drain task to exit.

/* 3. Private typedefs */
/* (none) */

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;

static sas_logger_config_t s_cfg;
static sas_log_level_t s_min_level = SAS_LOG_LEVEL_ERROR;
static uint32_t s_dropped_count = 0U;

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

/* 5. Forward declarations */
static sas_status_t log_write(sas_log_level_t level, const char *tag, const char *fmt,
                              va_list args);
static esp_log_level_t level_to_esp_log_level(sas_log_level_t level);
static char level_to_char(sas_log_level_t level);
static void drain_task_fn(void *arg);

/* 6. Public functions */

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

    s_cfg = *cfg;
    s_min_level = cfg->min_level;
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

    s_started = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

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
        s_drain_task_handle = xTaskCreateStatic(
            drain_task_fn, "sas_logger_drain", SAS_LOGGER_DRAIN_TASK_STACK_DEPTH, NULL,
            SAS_LOGGER_DRAIN_TASK_PRIORITY, s_drain_task_stack, &s_drain_task_tcb);
        configASSERT(s_drain_task_handle != NULL);
    }

    s_started = true;
    return SAS_STATUS_OK;
}

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

    s_min_level = SAS_LOG_LEVEL_ERROR;
    s_dropped_count = 0U;
    (void)memset(&s_cfg, 0, sizeof(s_cfg));
    s_initialized = false;
    return SAS_STATUS_OK;
}

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

/* 7. Private functions */

/*!
    \brief Format and route one log entry; the single entry point used by
           all sas_log_emit_*() wrappers.
*/
static sas_status_t log_write(sas_log_level_t level, const char *tag, const char *fmt, va_list args)
{
    configASSERT(s_initialized);

    sas_log_level_t min_level = SAS_LOG_LEVEL_ERROR;
    bool persist = false;

    if (xSemaphoreTake(s_lock, SAS_LOGGER_LOCK_WAIT_TICKS) == pdTRUE)
    {
        min_level = s_min_level;
        persist = s_cfg.persist_enabled && (s_ring_buf != NULL);
        xSemaphoreGive(s_lock);
    }

    if (level > min_level)
    {
        return SAS_STATUS_OK; /* SAS-LOG-FR-003: suppressed before formatting */
    }

    if (!persist)
    {
        esp_log_writev(level_to_esp_log_level(level), tag, fmt, args);
        return SAS_STATUS_OK;
    }

    char line[SAS_LOGGER_LINE_MAX_LEN];
    int hdr_written = snprintf(line, sizeof(line), "%u %c %s: ", (unsigned int)esp_log_timestamp(),
                               level_to_char(level), tag);
    size_t pos = ((hdr_written > 0) && ((size_t)hdr_written < sizeof(line))) ? (size_t)hdr_written
                                                                             : (sizeof(line) - 1U);

    int msg_written = vsnprintf(&line[pos], sizeof(line) - pos, fmt, args);
    size_t total = pos + ((msg_written > 0) ? (size_t)msg_written : 0U);
    if (total >= sizeof(line))
    {
        total = sizeof(line) - 1U;
    }

    if (xRingbufferSend(s_ring_buf, line, total, 0) != pdTRUE)
    {
        /* SAS-LOG-NFR-003 / AC-LOG-3: never block the caller; drop and count instead. */
        if (xSemaphoreTake(s_lock, SAS_LOGGER_LOCK_WAIT_TICKS) == pdTRUE)
        {
            s_dropped_count++;
            xSemaphoreGive(s_lock);
        }
    }

    return SAS_STATUS_OK; /* SAS_LOGGER_ERR_RING_FULL is never returned to the caller. */
}

/*! \brief Map sas_log_level_t to the matching esp_log_level_t for the console sink. */
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

/*! \brief Map sas_log_level_t to a single-character severity marker for persisted lines. */
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

/*! \brief Drain task body: echoes ring-buffer entries to the console for now; swap in a real file
 * write once File Manager can append. */
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
