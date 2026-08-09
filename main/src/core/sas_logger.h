/*!
    \file    sas_logger.h
    \brief   Public interface for the Logger module.

    \details
    Thread-safe, level-filtered logging used by every module instead of
    ESP-IDF's ESP_LOGx macros. Define a file-local TAG, then call
    sas_log_error/warn/info/debug/verbose(fmt, ...) - these add TAG and
    forward to sas_log_emit_*() below.
*/

#pragma once

/* 1. Standard includes */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* 2. ESP-IDF includes */
/* (none) */

/* 3. Project includes */
#include "sas_status.h"

/* 4. Public types */

/*! Logging severity levels, most to least severe. */
typedef enum
{
    SAS_LOG_LEVEL_ERROR = 0,
    SAS_LOG_LEVEL_WARN,
    SAS_LOG_LEVEL_INFO,
    SAS_LOG_LEVEL_DEBUG,
    SAS_LOG_LEVEL_VERBOSE,
} sas_log_level_t;

/*! Logger module configuration supplied to sas_logger_init(). */
typedef struct
{
    sas_log_level_t min_level; ///< Lowest severity emitted.
    bool persist_enabled;      ///< Enable the File Manager sink.
    char log_file_path[64];    ///< Persisted log file path.
} sas_logger_config_t;

/* 5. Public constants */

#define SAS_LOGGER_ERR_BASE (0x8100U) ///< Custom status base for this module.
#define SAS_LOGGER_ERR_RING_FULL \
    (SAS_LOGGER_ERR_BASE +       \
     1U) ///< Ring buffer full; entry dropped, counter incremented, never returned to the caller.

/* 6. Public API declarations */

/*!
    \brief   Initialize the Logger module.
    \param   cfg Non-NULL configuration; copied internally.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if cfg is NULL;
             SAS_ERR_INVALID_STATE if already initialized.
*/
sas_status_t sas_logger_init(const sas_logger_config_t *cfg);

/*!
    \brief   Start active logging operation (ring buffer drain task, if
             persist_enabled).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized or already started.
*/
sas_status_t sas_logger_start(void);

/*!
    \brief   Stop active logging operation without releasing resources.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_logger_stop(void);

/*!
    \brief   Release all resources acquired by sas_logger_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized or still started (call sas_logger_stop() first).
*/
sas_status_t sas_logger_deinit(void);

/*!
    \brief   Tagged ERROR entry point; call sas_log_error() instead.
    \param   tag Non-NULL module tag.
    \param   fmt Non-NULL printf-style format string.
    \return  SAS_STATUS_OK if accepted (SAS_LOGGER_ERR_RING_FULL is never
             returned; a full ring buffer just increments the drop count).
*/
sas_status_t sas_log_emit_error(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*! \brief Tagged WARN-severity entry point; see sas_log_emit_error(). */
sas_status_t sas_log_emit_warn(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*! \brief Tagged INFO-severity entry point; see sas_log_emit_error(). */
sas_status_t sas_log_emit_info(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*! \brief Tagged DEBUG-severity entry point; see sas_log_emit_error(). */
sas_status_t sas_log_emit_debug(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*! \brief Tagged VERBOSE-severity entry point; see sas_log_emit_error(). */
sas_status_t sas_log_emit_verbose(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*!
    \brief Log a message at ERROR severity, tagged with the calling file's
           file-local TAG. \param fmt Non-NULL printf-style format string.
           \return See sas_log_emit_error().
*/
#define sas_log_error(fmt, ...) sas_log_emit_error(TAG, fmt, ##__VA_ARGS__)
/*! \brief Log a message at WARN severity. See sas_log_error(). */
#define sas_log_warn(fmt, ...) sas_log_emit_warn(TAG, fmt, ##__VA_ARGS__)
/*! \brief Log a message at INFO severity. See sas_log_error(). */
#define sas_log_info(fmt, ...) sas_log_emit_info(TAG, fmt, ##__VA_ARGS__)
/*! \brief Log a message at DEBUG severity. See sas_log_error(). */
#define sas_log_debug(fmt, ...) sas_log_emit_debug(TAG, fmt, ##__VA_ARGS__)
/*! \brief Log a message at VERBOSE severity. See sas_log_error(). */
#define sas_log_verbose(fmt, ...) sas_log_emit_verbose(TAG, fmt, ##__VA_ARGS__)

/*!
    \brief   Set the minimum severity level emitted.
    \param   level New minimum level.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if level is out of range.
*/
sas_status_t sas_logger_set_level(sas_log_level_t level);

/*!
    \brief   Get the current minimum severity level.
    \return  Current sas_log_level_t.
*/
sas_log_level_t sas_logger_get_level(void);

/*!
    \brief   Get the count of entries dropped due to a full ring buffer.
    \return  Cumulative dropped-entry count since init.
*/
uint32_t sas_logger_get_dropped_count(void);
