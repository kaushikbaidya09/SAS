/*!
    \file    sas_config.h
    \brief   Public interface for the Configuration Manager module.

    \description
    Persistent key/value configuration store. Values are typed
    (string/int/bool), validated against a per-key schema on write, and
    mirrored in a fixed-size in-RAM table for fast reads. Changes are
    persisted to the File Manager LittleFS partition and announced on the
    Event Bus so other modules can react without polling.

    \dependencies
    - File Manager (main/src/storage/sas_file_manager.h): LittleFS-backed
      atomic (temp-file-then-rename) persistence of /config/sas.cfg.
    - Event Bus (main/src/core/sas_event_bus.h): publishes
      SAS_EVENT_CONFIG_CHANGED whenever sas_config_set() accepts a new value.
*/

#pragma once

/* 1. Standard includes */
#include <stdbool.h>
#include <stdint.h>

/* 2. ESP-IDF includes */
/* (none) */

/* 3. Project includes */
#include "sas_status.h"

/* 4. Public types */

/*! Maximum length, including the null terminator, of a
    sas_config_value_t.value.str_val entry. Defined here (ahead of the
    Public constants section) because sas_config_value_t below requires it;
    placeholder size, to be tuned during implementation. */
#define SAS_CFG_MAX_STRING_LEN (64U)

/*! Discriminator for the value union carried by sas_config_value_t. */
typedef enum {
    SAS_CFG_TYPE_STRING = 0, ///< value.str_val is valid.
    SAS_CFG_TYPE_INT,        ///< value.int_val is valid.
    SAS_CFG_TYPE_BOOL,       ///< value.bool_val is valid.
} sas_config_value_type_t;

/*! A single typed configuration value. */
typedef struct {
    sas_config_value_type_t type; ///< Which union member is valid.
    union {
        char    str_val[SAS_CFG_MAX_STRING_LEN]; ///< SAS_CFG_TYPE_STRING.
        int32_t int_val;                         ///< SAS_CFG_TYPE_INT.
        bool    bool_val;                        ///< SAS_CFG_TYPE_BOOL.
    } value;                      ///< Type-selected value storage.
} sas_config_value_t;

/* 5. Public constants */

#define SAS_CFG_ERR_BASE (0x8200U) ///< Custom status base for this module.
#define SAS_CFG_ERR_NOT_FOUND (SAS_CFG_ERR_BASE + 1U) ///< Internal only: resolved transparently by sas_config_get() returning default_val to the caller; never propagated as a failure.
#define SAS_CFG_ERR_INVALID_VALUE (SAS_CFG_ERR_BASE + 2U) ///< Returned by sas_config_set() when val fails schema validation (range/length); the store is left unmodified.
#define SAS_CFG_ERR_STORE_FULL (SAS_CFG_ERR_BASE + 3U) ///< Returned by sas_config_set() when key is new and SAS_CFG_MAX_ENTRIES would be exceeded.

#define SAS_CFG_MAX_ENTRIES (64U) ///< Maximum number of distinct keys held in the in-RAM table / backing file. Placeholder size, to be tuned during implementation.
/*! \see SAS_CFG_MAX_STRING_LEN, defined above in Public types (needed by
    sas_config_value_t). Listed here for discoverability alongside the
    other sizing constant. */

/* 6. Public API declarations */

/*!
    \brief   Initialize the Configuration Manager module.
    \return  SAS_STATUS_OK on success.
*/
sas_status_t sas_config_init(void);

/*!
    \brief   Start active operation: load the backing file into the in-RAM
             table (or seed compiled-in defaults if absent).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_config_start(void);

/*!
    \brief   Stop active operation without releasing resources acquired by
             sas_config_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_config_stop(void);

/*!
    \brief   Release all resources acquired by sas_config_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_config_deinit(void);

/*!
    \brief   Retrieve the value stored for a configuration key.
    \param   key         Non-NULL, null-terminated key string.
    \param   out         Non-NULL destination for the retrieved value.
    \param   default_val Non-NULL value copied into *out if key is not
                         present in the store; a missing key is not treated
                         as an error.
    \return  SAS_STATUS_OK on success, including the default-value fallback
             case; SAS_ERR_INVALID_ARG if key, out, or default_val is NULL.
*/
sas_status_t sas_config_get(const char *key, sas_config_value_t *out,
                            const sas_config_value_t *default_val);

/*!
    \brief   Set (create or update) the value for a configuration key.
    \param   key Non-NULL, null-terminated key string.
    \param   val Non-NULL value to store; validated against the key's
                 registered schema before acceptance.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if key or val is
             NULL, or val->type is out of range; SAS_CFG_ERR_INVALID_VALUE
             if val fails schema validation (store left unmodified);
             SAS_CFG_ERR_STORE_FULL if key is new and SAS_CFG_MAX_ENTRIES is
             already reached.
*/
sas_status_t sas_config_set(const char *key, const sas_config_value_t *val);

/*!
    \brief   Restore every configuration entry to its compiled-in default.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_config_factory_reset(void);
