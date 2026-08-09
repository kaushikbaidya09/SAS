/*!
    \file    sas_config.h
    \brief   Public interface for the Configuration Manager module.

    \details
    A key/value settings store used instead of hardcoding values. Values
    are typed (string/int/bool) and schema-checked on write, saved to
    LittleFS via File Manager, and announced as SAS_EVENT_CONFIG_CHANGED
    on the Event Bus.
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
typedef enum
{
    SAS_CFG_TYPE_STRING = 0, ///< value.str_val is valid.
    SAS_CFG_TYPE_INT,        ///< value.int_val is valid.
    SAS_CFG_TYPE_BOOL,       ///< value.bool_val is valid.
} sas_config_value_type_t;

/*! A single typed configuration value. */
typedef struct
{
    sas_config_value_type_t type; ///< Which union member is valid.
    union
    {
        char str_val[SAS_CFG_MAX_STRING_LEN]; ///< SAS_CFG_TYPE_STRING.
        int32_t int_val;                      ///< SAS_CFG_TYPE_INT.
        bool bool_val;                        ///< SAS_CFG_TYPE_BOOL.
    } value;                                  ///< Type-selected value storage.
} sas_config_value_t;

/* 5. Public constants */

#define SAS_CFG_ERR_BASE (0x8200U) ///< Custom status base for this module.
#define SAS_CFG_ERR_NOT_FOUND \
    (SAS_CFG_ERR_BASE + 1U) ///< Internal only; sas_config_get() handles a missing key itself.
#define SAS_CFG_ERR_INVALID_VALUE \
    (SAS_CFG_ERR_BASE + 2U) ///< sas_config_set(): val fails schema validation.
#define SAS_CFG_ERR_STORE_FULL \
    (SAS_CFG_ERR_BASE + 3U) ///< sas_config_set(): SAS_CFG_MAX_ENTRIES already reached.

#define SAS_CFG_MAX_ENTRIES \
    (64U) ///< Maximum number of distinct keys held in the in-RAM table / backing file. Placeholder
          ///< size, to be tuned during implementation.
/*! \see SAS_CFG_MAX_STRING_LEN, defined above in Public types (needed by
    sas_config_value_t). Listed here for discoverability alongside the
    other sizing constant. */

#define SAS_CFG_MAX_KEY_LEN \
    (32U) ///< Maximum length, including the null terminator, of a configuration key. Placeholder
          ///< size, to be tuned during implementation.

/* 6. Public API declarations */

/*!
    \brief   Initialize the Configuration Manager module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already
             initialized.
*/
sas_status_t sas_config_init(void);

/*!
    \brief   Start active operation: load the backing file into the in-RAM
             table (or seed compiled-in defaults if absent).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized or already started.
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
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized or still started (call sas_config_stop() first).
*/
sas_status_t sas_config_deinit(void);

/*!
    \brief   Retrieve the value stored for a configuration key.
    \param   key         Non-NULL, non-empty key shorter than SAS_CFG_MAX_KEY_LEN.
    \param   out         Non-NULL destination for the retrieved value.
    \param   default_val Non-NULL; copied into *out if key isn't present (not an error).
    \return  SAS_STATUS_OK (including the default-fallback case);
             SAS_ERR_INVALID_ARG for a NULL, empty, or too-long key.
*/
sas_status_t sas_config_get(const char *key, sas_config_value_t *out,
                            const sas_config_value_t *default_val);

/*!
    \brief   Set (create or update) the value for a configuration key.
    \param   key Non-NULL, non-empty key shorter than SAS_CFG_MAX_KEY_LEN.
    \param   val Non-NULL value; validated against the key's schema.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_ARG (bad key/val);
             SAS_CFG_ERR_INVALID_VALUE (fails schema); SAS_CFG_ERR_STORE_FULL
             (table full); SAS_ERR_TIMEOUT (lock busy). A persist/publish
             failure after acceptance is only logged, not returned.
*/
sas_status_t sas_config_set(const char *key, const sas_config_value_t *val);

/*!
    \brief   Restore every configuration entry to its compiled-in default.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_config_factory_reset(void);
