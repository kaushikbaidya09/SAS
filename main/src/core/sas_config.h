//=============================================================================
/*!
    \file    sas_config.h
    \brief   Public interface for the Configuration Manager module.
*/
//=============================================================================

// Typed key/value settings store (string, int, bool). Values are checked on
// write, saved to the filesystem and announced with SAS_EVENT_CONFIG_CHANGED.

#ifndef SAS_CONFIG_H
#define SAS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "sas_status.h"

/*! \brief Maximum length, including the null terminator, of a
    sas_config_value_t.value.str_val entry. Defined here (ahead of the
    Public constants section) because sas_config_value_t below requires it.
    Sized to hold a 64-character value plus NUL with headroom: a 64-hex-digit
    Wi-Fi PSK and Authentication's hex-encoded SHA-256 HMAC are both exactly
    64 characters. */
#define SAS_CFG_MAX_STRING_LEN (80U)

/*! \brief Discriminator for the value union carried by sas_config_value_t. */
typedef enum
{
    SAS_CFG_TYPE_STRING = 0, ///< value.str_val is valid.
    SAS_CFG_TYPE_INT,        ///< value.int_val is valid.
    SAS_CFG_TYPE_BOOL,       ///< value.bool_val is valid.
} sas_config_value_type_t;

/*! \brief A single typed configuration value. */
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

#define SAS_CFG_ERR_BASE (0x8200U) ///< Custom status base for this module.
/*! \brief Internal only; sas_config_get() handles a missing key itself. */
#define SAS_CFG_ERR_NOT_FOUND (SAS_CFG_ERR_BASE + 1U)
/*! \brief sas_config_set(): val fails schema validation. */
#define SAS_CFG_ERR_INVALID_VALUE (SAS_CFG_ERR_BASE + 2U)
/*! \brief sas_config_set(): SAS_CFG_MAX_ENTRIES already reached. */
#define SAS_CFG_ERR_STORE_FULL (SAS_CFG_ERR_BASE + 3U)

/*!
    \brief   Maximum number of keys held in the table and its backing file.
*/
#define SAS_CFG_MAX_ENTRIES (64U)
/*! \see SAS_CFG_MAX_STRING_LEN, defined above in Public types (needed by
    sas_config_value_t). Listed here for discoverability alongside the
    other sizing constant. */

/*!
    \brief   Maximum length of a configuration key, including the null
             terminator.
*/
#define SAS_CFG_MAX_KEY_LEN (32U)

sas_status_t sas_config_init(void);

sas_status_t sas_config_start(void);

sas_status_t sas_config_stop(void);

sas_status_t sas_config_deinit(void);

sas_status_t sas_config_reload(void);

bool sas_config_has_key(const char *key);

sas_status_t sas_config_get(const char *key, sas_config_value_t *out,
                            const sas_config_value_t *default_val);

sas_status_t sas_config_set(const char *key, const sas_config_value_t *val);

sas_status_t sas_config_factory_reset(void);

#endif /* SAS_CONFIG_H */
