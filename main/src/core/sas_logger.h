//=============================================================================
/*!
    \file    sas_logger.h
    \brief   Public interface for the Logger module.
*/
//=============================================================================

// Thread-safe, level-filtered logging used by every module in place of the
// ESP-IDF ESP_LOGx macros. Define a file-local TAG, then call
// sas_log_error/warn/info/debug/verbose(fmt, ...).
//
// Accepted entries are also kept in a small in-RAM history and published as
// SAS_EVENT_LOG_ENTRY, so the dashboard can show recent and live log output.

#ifndef SAS_LOGGER_H
#define SAS_LOGGER_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "sas_status.h"

/*! \brief Logging severity levels, most to least severe. */
typedef enum
{
    SAS_LOG_LEVEL_ERROR = 0,
    SAS_LOG_LEVEL_WARN,
    SAS_LOG_LEVEL_INFO,
    SAS_LOG_LEVEL_DEBUG,
    SAS_LOG_LEVEL_VERBOSE,
} sas_log_level_t;

/*! \brief Logger module configuration supplied to sas_logger_init(). */
typedef struct
{
    sas_log_level_t min_level; ///< Lowest severity emitted.
    bool persist_enabled;      ///< Enable the File Manager sink.
    char log_file_path[64];    ///< Persisted log file path.
} sas_logger_config_t;

/*! \brief One entry in the in-RAM history returned by sas_log_get_entries(), and the payload shape
   of SAS_EVENT_LOG_ENTRY. */
typedef struct
{
    int64_t timestamp_us;  ///< esp_timer_get_time() at the moment this entry was accepted.
    sas_log_level_t level; ///< Severity this entry was emitted at.
    char tag[16];          ///< Emitting module's TAG, truncated if longer.
    char message[100];     ///< Formatted message, truncated if longer.
} sas_log_entry_t;

#define SAS_LOGGER_ERR_BASE (0x8100U) ///< Custom status base for this module.
/*! \brief Ring buffer full; entry dropped, counter incremented, never returned to the caller. */
#define SAS_LOGGER_ERR_RING_FULL (SAS_LOGGER_ERR_BASE + 1U)

/*! \brief Number of entries kept in the in-RAM history; the oldest is dropped when full. */
#define SAS_LOGGER_MAX_HISTORY_ENTRIES (64U)

sas_status_t sas_logger_init(const sas_logger_config_t *cfg);

sas_status_t sas_logger_start(void);

sas_status_t sas_logger_stop(void);

sas_status_t sas_logger_deinit(void);

sas_status_t sas_log_emit_error(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

sas_status_t sas_log_emit_warn(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

sas_status_t sas_log_emit_info(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

sas_status_t sas_log_emit_debug(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

sas_status_t sas_log_emit_verbose(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*!
    \brief   Log a message at ERROR severity, tagged with the calling file's
             file-local TAG.
    \param[in] fmt  Non-NULL printf-style format string.
    \return  See sas_log_emit_error().
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

sas_status_t sas_logger_set_level(sas_log_level_t level);

sas_log_level_t sas_logger_get_level(void);

uint32_t sas_logger_get_dropped_count(void);

sas_status_t sas_log_get_entries(sas_log_entry_t *out_array, uint16_t max_entries,
                                 uint16_t *out_count);

#endif /* SAS_LOGGER_H */
