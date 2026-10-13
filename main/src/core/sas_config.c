//=============================================================================
/*!
    \file    sas_config.c
    \brief   Implementation of the Configuration Manager module.
*/
//=============================================================================

#include "sas_config.h"

#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sas_event_bus.h"
#include "sas_file_manager.h"
#include "sas_logger.h"

static const char *TAG = "sas_config";

/*!
    \brief   Backing store path on the File Manager LittleFS partition (mounted
             at /www; the HTTP Server and REST API refuse to serve /config).
*/
#define SAS_CFG_STORE_FILE_PATH "/www/config/sas.cfg"

/*! \brief Bounded wait for the internal state mutex; critical sections are microsecond-scale. */
#define SAS_CFG_LOCK_WAIT_TICKS pdMS_TO_TICKS(50)

// Worst-case serialized record: 1 (key_len) + key + 1 (type) + worst-case
// value payload (a SAS_CFG_TYPE_STRING value: 1 length byte + string).
#define SAS_CFG_RECORD_MAX_LEN (1U + SAS_CFG_MAX_KEY_LEN + 1U + 1U + SAS_CFG_MAX_STRING_LEN)
/*! \brief Scratch buffer sized for every entry serialized at once. */
#define SAS_CFG_STORE_BUFFER_LEN (SAS_CFG_MAX_ENTRIES * SAS_CFG_RECORD_MAX_LEN)

/*! \brief One in-RAM table slot. */
typedef struct
{
    bool in_use;                   ///< false if this slot is free.
    char key[SAS_CFG_MAX_KEY_LEN]; ///< NUL-terminated key.
    sas_config_value_t value;      ///< Stored value.
} sas_config_entry_t;

/*! \brief One compiled-in per-key validation rule for validate_value_against_schema(). */
typedef struct
{
    const char *key;              ///< Key this rule applies to; "" marks an unused sentinel row.
    sas_config_value_type_t type; ///< Required value type for this key.
    int32_t min_int;              ///< Inclusive minimum, for SAS_CFG_TYPE_INT.
    int32_t max_int;              ///< Inclusive maximum, for SAS_CFG_TYPE_INT.
    size_t max_str_len; ///< Maximum string length (excluding NUL), for SAS_CFG_TYPE_STRING.
} sas_config_schema_entry_t;

/*! \brief One compiled-in factory-default key/value pair for seed_defaults(). */
typedef struct
{
    const char *key;          ///< Default's key; "" marks an unused sentinel row.
    sas_config_value_t value; ///< Default's value.
} sas_config_default_entry_t;

static bool s_initialized = false;
static bool s_started     = false;

static sas_config_entry_t s_table[SAS_CFG_MAX_ENTRIES];

static SemaphoreHandle_t s_lock = NULL;
static StaticSemaphore_t s_lock_storage;

static uint8_t s_store_buf[SAS_CFG_STORE_BUFFER_LEN];

// No schema is registered yet. The empty sentinel entry keeps the array
// non-empty, as ISO C requires.
static const sas_config_schema_entry_t s_schema_table[] = {
    {"", SAS_CFG_TYPE_STRING, 0, 0, 0U},
};
#define SAS_CFG_SCHEMA_COUNT (sizeof(s_schema_table) / sizeof(s_schema_table[0]))

// No defaults are registered yet; the sentinel serves the same purpose as
// above.
static const sas_config_default_entry_t s_default_table[] = {
    {"", {SAS_CFG_TYPE_STRING, {""}}},
};
#define SAS_CFG_DEFAULT_COUNT (sizeof(s_default_table) / sizeof(s_default_table[0]))

static sas_status_t validate_value_against_schema(const char *key, const sas_config_value_t *val);
static const sas_config_schema_entry_t *find_schema(const char *key);
static sas_config_entry_t *find_entry(const char *key);
static sas_config_entry_t *find_free_slot(void);
static void seed_defaults(void);
static void clear_table(void);
static sas_status_t save_to_file(void);
static sas_status_t load_from_file(void);
static size_t serialize_entry(const sas_config_entry_t *entry, uint8_t *out, size_t out_cap);
static bool deserialize_entry(const uint8_t *buf, size_t buf_len, size_t *inout_offset,
                              sas_config_entry_t *out_entry);
static bool is_key_valid(const char *key);
static size_t bounded_strlen(const char *s, size_t max_len);

/*!
    \brief   Initialize the Configuration Manager module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already
             initialized.
*/
sas_status_t sas_config_init(void)
{
    if (s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    clear_table();

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    configASSERT(s_lock != NULL);

    s_started     = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Start active operation: load the backing file into the in-RAM
             table (or seed compiled-in defaults if absent).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized
             or already started.
*/
sas_status_t sas_config_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, SAS_CFG_LOCK_WAIT_TICKS) == pdTRUE)
    {
        sas_status_t load_status = load_from_file();
        if (load_status != SAS_STATUS_OK)
        {
            // Expected at boot: the filesystem is not mounted yet. See
            // sas_config_reload().
            sas_log_info("config not loaded yet (0x%x); using compiled-in defaults",
                         (unsigned int)load_status);
            clear_table();
            seed_defaults();
        }
        xSemaphoreGive(s_lock);
    }

    s_started = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Stop active operation without releasing resources acquired by
             sas_config_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_config_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    s_started = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Release all resources acquired by sas_config_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized
             or still started (call sas_config_stop() first).
*/
sas_status_t sas_config_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_lock != NULL)
    {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    clear_table();
    s_initialized = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Retrieve the value stored for a configuration key.
    \param[in]  key          Non-NULL, non-empty key shorter than
                             SAS_CFG_MAX_KEY_LEN.
    \param[out] out          Non-NULL destination for the retrieved value.
    \param[in]  default_val  Non-NULL; copied into *out if key isn't present
                             (not an error).
    \return  SAS_STATUS_OK (including the default-fallback case);
             SAS_ERR_INVALID_ARG for a NULL, empty, or too-long key.
*/
sas_status_t sas_config_get(const char *key, sas_config_value_t *out,
                            const sas_config_value_t *default_val)
{
    if ((key == NULL) || (out == NULL) || (default_val == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!is_key_valid(key))
    {
        return SAS_ERR_INVALID_ARG;
    }
    configASSERT(s_initialized);

    if (xSemaphoreTake(s_lock, SAS_CFG_LOCK_WAIT_TICKS) == pdTRUE)
    {
        const sas_config_entry_t *entry = find_entry(key);
        *out                            = (entry != NULL) ? entry->value : *default_val;
        xSemaphoreGive(s_lock);
    }
    else
    {
        *out = *default_val;
    }

    return SAS_STATUS_OK;
}

/*!
    \brief   Set (create or update) the value for a configuration key.
    \param[in] key  Non-NULL, non-empty key shorter than SAS_CFG_MAX_KEY_LEN.
    \param[in] val  Non-NULL value; validated against the key's schema.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_ARG (bad key/val);
             SAS_CFG_ERR_INVALID_VALUE (fails schema); SAS_CFG_ERR_STORE_FULL
             (table full); SAS_ERR_TIMEOUT (lock busy). A persist/publish
             failure after acceptance is only logged, not returned.
*/
sas_status_t sas_config_set(const char *key, const sas_config_value_t *val)
{
    if ((key == NULL) || (val == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (val->type > SAS_CFG_TYPE_BOOL)
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!is_key_valid(key))
    {
        return SAS_ERR_INVALID_ARG;
    }
    if ((val->type == SAS_CFG_TYPE_STRING) &&
        (bounded_strlen(val->value.str_val, SAS_CFG_MAX_STRING_LEN) >= SAS_CFG_MAX_STRING_LEN))
    {
        return SAS_ERR_INVALID_ARG; // not NUL-terminated within bounds
    }
    configASSERT(s_initialized);

    sas_status_t status = SAS_STATUS_OK;

    if (xSemaphoreTake(s_lock, SAS_CFG_LOCK_WAIT_TICKS) == pdTRUE)
    {
        status = validate_value_against_schema(key, val);
        if (status == SAS_STATUS_OK)
        {
            sas_config_entry_t *entry = find_entry(key);
            if (entry == NULL)
            {
                entry = find_free_slot();
                if (entry == NULL)
                {
                    status = SAS_CFG_ERR_STORE_FULL;
                }
                else
                {
                    (void)strncpy(entry->key, key, SAS_CFG_MAX_KEY_LEN - 1U);
                    entry->key[SAS_CFG_MAX_KEY_LEN - 1U] = '\0';
                    entry->in_use                        = true;
                }
            }

            if (status == SAS_STATUS_OK)
            {
                entry->value = *val;

                sas_status_t persist_status = save_to_file();
                if (persist_status != SAS_STATUS_OK)
                {
                    sas_log_warn("'%s' applied in RAM only; persist failed (0x%x)", key,
                                 (unsigned int)persist_status);
                }
            }
        }
        xSemaphoreGive(s_lock);
    }
    else
    {
        status = SAS_ERR_TIMEOUT;
    }

    if (status == SAS_STATUS_OK)
    {
        sas_status_t publish_status =
            sas_event_bus_publish(SAS_EVENT_CONFIG_CHANGED, key, strlen(key) + 1U);
        if (publish_status != SAS_STATUS_OK)
        {
            sas_log_warn("SAS_EVENT_CONFIG_CHANGED publish failed for '%s' (0x%x)", key,
                         (unsigned int)publish_status);
        }
    }

    return status;
}

/*!
    \brief   Re-read the backing file into the in-RAM table.
    \return  SAS_STATUS_OK if the file was loaded; SAS_ERR_INVALID_STATE if not
             started; SAS_ERR_TIMEOUT (lock busy); any other status if the file
             is missing or corrupt, in which case the compiled-in defaults are
             (re)seeded.
*/
sas_status_t sas_config_reload(void)
{
    // Called at boot once the filesystem is mounted, because the load
    // attempted by sas_config_start() runs before that.

    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    sas_status_t status = SAS_ERR_TIMEOUT;
    if (xSemaphoreTake(s_lock, SAS_CFG_LOCK_WAIT_TICKS) == pdTRUE)
    {
        status = load_from_file();
        if (status != SAS_STATUS_OK)
        {
            clear_table();
            seed_defaults();
        }
        xSemaphoreGive(s_lock);
    }
    return status;
}

/*!
    \brief   Query whether a key currently holds a stored value.
    \param[in] key  Non-NULL key.
    \return  true if key is present in the table; false otherwise (including a
             NULL/invalid key or a busy lock).
*/
bool sas_config_has_key(const char *key)
{
    if ((key == NULL) || !is_key_valid(key) || !s_initialized)
    {
        return false;
    }

    bool present = false;
    if (xSemaphoreTake(s_lock, SAS_CFG_LOCK_WAIT_TICKS) == pdTRUE)
    {
        present = (find_entry(key) != NULL);
        xSemaphoreGive(s_lock);
    }
    return present;
}

/*!
    \brief   Restore every configuration entry to its compiled-in default.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_config_factory_reset(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, SAS_CFG_LOCK_WAIT_TICKS) == pdTRUE)
    {
        clear_table();
        seed_defaults();

        sas_status_t persist_status = save_to_file();
        if (persist_status != SAS_STATUS_OK)
        {
            sas_log_warn("factory reset applied in RAM only; persist failed (0x%x)",
                         (unsigned int)persist_status);
        }
        xSemaphoreGive(s_lock);
    }

    for (size_t i = 0U; i < SAS_CFG_DEFAULT_COUNT; i++)
    {
        if (s_default_table[i].key[0] != '\0')
        {
            sas_status_t publish_status =
                sas_event_bus_publish(SAS_EVENT_CONFIG_CHANGED, s_default_table[i].key,
                                      strlen(s_default_table[i].key) + 1U);
            if (publish_status != SAS_STATUS_OK)
            {
                sas_log_warn("SAS_EVENT_CONFIG_CHANGED publish failed for '%s' (0x%x)",
                             s_default_table[i].key, (unsigned int)publish_status);
            }
        }
    }

    return SAS_STATUS_OK;
}

/*!
    \brief   Length of s, capped at max_len (avoids relying on POSIX strnlen(),
             not guaranteed under strict C11).
*/
static size_t bounded_strlen(const char *s, size_t max_len)
{
    size_t len = 0U;
    while ((len < max_len) && (s[len] != '\0'))
    {
        len++;
    }
    return len;
}

/*!
    \brief   True if key is non-empty and short enough to fit in
             SAS_CFG_MAX_KEY_LEN (including the NUL).
*/
static bool is_key_valid(const char *key)
{
    size_t len = bounded_strlen(key, SAS_CFG_MAX_KEY_LEN);
    return (len > 0U) && (len < SAS_CFG_MAX_KEY_LEN);
}

/*!
    \brief   Validate val against key's registered schema (range for INT,
             length for STRING) before sas_config_set() commits it.
*/
static sas_status_t validate_value_against_schema(const char *key, const sas_config_value_t *val)
{
    const sas_config_schema_entry_t *schema = find_schema(key);

    if (schema == NULL)
    {
        return SAS_STATUS_OK; // no registered schema: accept as-is
    }
    if (schema->type != val->type)
    {
        return SAS_CFG_ERR_INVALID_VALUE;
    }

    switch (val->type)
    {
        case SAS_CFG_TYPE_INT:
            if ((val->value.int_val < schema->min_int) || (val->value.int_val > schema->max_int))
            {
                return SAS_CFG_ERR_INVALID_VALUE;
            }
            break;
        case SAS_CFG_TYPE_STRING:
            if (bounded_strlen(val->value.str_val, SAS_CFG_MAX_STRING_LEN) > schema->max_str_len)
            {
                return SAS_CFG_ERR_INVALID_VALUE;
            }
            break;
        case SAS_CFG_TYPE_BOOL:
        default:
            break;
    }

    return SAS_STATUS_OK;
}

/*!
    \brief   Look up a key's compiled-in schema row, if any.
*/
static const sas_config_schema_entry_t *find_schema(const char *key)
{
    for (size_t i = 0U; i < SAS_CFG_SCHEMA_COUNT; i++)
    {
        if ((s_schema_table[i].key[0] != '\0') && (strcmp(s_schema_table[i].key, key) == 0))
        {
            return &s_schema_table[i];
        }
    }
    return NULL;
}

/*!
    \brief   Find the in-use table slot for key, or NULL if not present.
*/
static sas_config_entry_t *find_entry(const char *key)
{
    for (size_t i = 0U; i < SAS_CFG_MAX_ENTRIES; i++)
    {
        if (s_table[i].in_use && (strcmp(s_table[i].key, key) == 0))
        {
            return &s_table[i];
        }
    }
    return NULL;
}

/*!
    \brief   Find the first free table slot, or NULL if the table is full.
*/
static sas_config_entry_t *find_free_slot(void)
{
    for (size_t i = 0U; i < SAS_CFG_MAX_ENTRIES; i++)
    {
        if (!s_table[i].in_use)
        {
            return &s_table[i];
        }
    }
    return NULL;
}

/*!
    \brief   Mark every table slot free. Caller holds s_lock (or is
             init/deinit, where no other task can be running yet).
*/
static void clear_table(void)
{
    (void)memset(s_table, 0, sizeof(s_table));
}

/*!
    \brief   Populate the table from the compiled-in defaults. Caller holds
             s_lock.
*/
static void seed_defaults(void)
{
    for (size_t i = 0U; i < SAS_CFG_DEFAULT_COUNT; i++)
    {
        if (s_default_table[i].key[0] == '\0')
        {
            continue;
        }

        sas_config_entry_t *slot = find_free_slot();
        if (slot == NULL)
        {
            // The table is too small for the defaults: a build-time sizing
            // bug.
            configASSERT(slot != NULL);
            break;
        }

        (void)strncpy(slot->key, s_default_table[i].key, SAS_CFG_MAX_KEY_LEN - 1U);
        slot->key[SAS_CFG_MAX_KEY_LEN - 1U] = '\0';
        slot->value                         = s_default_table[i].value;
        slot->in_use                        = true;
    }
}

/*!
    \brief   Serialize one entry as [key_len][key][type][value] into out;
             returns bytes written, or 0 if it wouldn't fit in out_cap.
*/
static size_t serialize_entry(const sas_config_entry_t *entry, uint8_t *out, size_t out_cap)
{
    size_t key_len = strlen(entry->key);
    size_t needed  = 1U + key_len + 1U;

    switch (entry->value.type)
    {
        case SAS_CFG_TYPE_STRING:
            needed += 1U + bounded_strlen(entry->value.value.str_val, SAS_CFG_MAX_STRING_LEN);
            break;
        case SAS_CFG_TYPE_INT:
            needed += sizeof(int32_t);
            break;
        case SAS_CFG_TYPE_BOOL:
        default:
            needed += 1U;
            break;
    }

    if (needed > out_cap)
    {
        return 0U;
    }

    size_t pos = 0U;
    out[pos]   = (uint8_t)key_len;
    pos++;
    (void)memcpy(&out[pos], entry->key, key_len);
    pos += key_len;
    out[pos] = (uint8_t)entry->value.type;
    pos++;

    switch (entry->value.type)
    {
        case SAS_CFG_TYPE_STRING:
        {
            size_t val_len = bounded_strlen(entry->value.value.str_val, SAS_CFG_MAX_STRING_LEN);
            out[pos]       = (uint8_t)val_len;
            pos++;
            (void)memcpy(&out[pos], entry->value.value.str_val, val_len);
            pos += val_len;
            break;
        }
        case SAS_CFG_TYPE_INT:
        {
            uint32_t u    = (uint32_t)entry->value.value.int_val;
            out[pos]      = (uint8_t)(u & 0xFFU);
            out[pos + 1U] = (uint8_t)((u >> 8) & 0xFFU);
            out[pos + 2U] = (uint8_t)((u >> 16) & 0xFFU);
            out[pos + 3U] = (uint8_t)((u >> 24) & 0xFFU);
            pos += sizeof(int32_t);
            break;
        }
        case SAS_CFG_TYPE_BOOL:
        default:
            out[pos] = entry->value.value.bool_val ? 1U : 0U;
            pos++;
            break;
    }

    return pos;
}

/*!
    \brief   Deserialize one record at *inout_offset, advancing it on success.
*/
static bool deserialize_entry(const uint8_t *buf, size_t buf_len, size_t *inout_offset,
                              sas_config_entry_t *out_entry)
{
    // Returns false if the record is truncated or malformed.

    size_t pos = *inout_offset;

    if (pos >= buf_len)
    {
        return false;
    }
    uint8_t key_len = buf[pos];
    pos++;
    if ((key_len >= SAS_CFG_MAX_KEY_LEN) || ((buf_len - pos) < key_len))
    {
        return false;
    }
    (void)memcpy(out_entry->key, &buf[pos], key_len);
    out_entry->key[key_len] = '\0';
    pos += key_len;

    if (pos >= buf_len)
    {
        return false;
    }
    uint8_t type = buf[pos];
    pos++;
    if (type > (uint8_t)SAS_CFG_TYPE_BOOL)
    {
        return false;
    }
    out_entry->value.type = (sas_config_value_type_t)type;

    switch (out_entry->value.type)
    {
        case SAS_CFG_TYPE_STRING:
        {
            if (pos >= buf_len)
            {
                return false;
            }
            uint8_t val_len = buf[pos];
            pos++;
            if ((val_len >= SAS_CFG_MAX_STRING_LEN) || ((buf_len - pos) < val_len))
            {
                return false;
            }
            (void)memcpy(out_entry->value.value.str_val, &buf[pos], val_len);
            out_entry->value.value.str_val[val_len] = '\0';
            pos += val_len;
            break;
        }
        case SAS_CFG_TYPE_INT:
        {
            if ((buf_len - pos) < sizeof(int32_t))
            {
                return false;
            }
            uint32_t u = (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1U] << 8) |
                         ((uint32_t)buf[pos + 2U] << 16) | ((uint32_t)buf[pos + 3U] << 24);
            out_entry->value.value.int_val = (int32_t)u;
            pos += sizeof(int32_t);
            break;
        }
        case SAS_CFG_TYPE_BOOL:
        default:
            if (pos >= buf_len)
            {
                return false;
            }
            out_entry->value.value.bool_val = (buf[pos] != 0U);
            pos++;
            break;
    }

    out_entry->in_use = true;
    *inout_offset     = pos;
    return true;
}

/*!
    \brief   Serialize every in-use table entry and write the whole store file
             atomically. Caller holds s_lock.
*/
static sas_status_t save_to_file(void)
{
    size_t total = 0U;

    for (size_t i = 0U; i < SAS_CFG_MAX_ENTRIES; i++)
    {
        if (!s_table[i].in_use)
        {
            continue;
        }
        size_t written =
            serialize_entry(&s_table[i], &s_store_buf[total], sizeof(s_store_buf) - total);
        if (written == 0U)
        {
            return SAS_ERR_NO_MEM; // cannot happen: the buffer fits every entry
        }
        total += written;
    }

    return sas_fs_write(SAS_CFG_STORE_FILE_PATH, s_store_buf, total);
}

/*!
    \brief   Read the store file into s_table. May leave it partially populated
             on a bad file; safe since both callers (sas_config_start(),
             sas_config_reload()) clear it on any failure, and this avoids a
             large stack copy.
*/
static sas_status_t load_from_file(void)
{
    size_t read_len = 0U;
    sas_status_t status =
        sas_fs_read(SAS_CFG_STORE_FILE_PATH, 0U, s_store_buf, sizeof(s_store_buf), &read_len);
    if (status != SAS_STATUS_OK)
    {
        return status;
    }

    clear_table();
    size_t offset = 0U;
    size_t count  = 0U;

    while (offset < read_len)
    {
        if (count >= SAS_CFG_MAX_ENTRIES)
        {
            return SAS_CFG_ERR_STORE_FULL; // corrupt file, or more entries than this build supports
        }
        if (!deserialize_entry(s_store_buf, read_len, &offset, &s_table[count]))
        {
            return SAS_ERR_INVALID_ARG; // corrupt record
        }
        count++;
    }

    return SAS_STATUS_OK;
}
