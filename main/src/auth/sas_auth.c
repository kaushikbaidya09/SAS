//=============================================================================
/*!
    \file    sas_auth.c
    \brief   Implementation of the Authentication module.
*/
//=============================================================================

// No code in this file reads an eFuse HMAC key into application memory; the
// key block is only ever named as the key selector of esp_hmac_calculate()
// (SAS-AUTH-NFR-003).

#include "sas_auth.h"

#include <stddef.h>
#include <string.h>

#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "psa/crypto.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "../core/sas_config.h"
#include "../core/sas_event_bus.h"
#include "../core/sas_logger.h"

static const char *TAG = "sas_auth";

/*! \brief Bounded wait for s_lock; a login holds it across one HMAC computation. */
#define SAS_AUTH_LOCK_WAIT_TICKS  pdMS_TO_TICKS(200)
#define SAS_AUTH_HASH_LEN         (32U) ///< HMAC-SHA256 output length in bytes.
#define SAS_AUTH_SALT_LEN         (16U) ///< Per-device salt length in bytes.
#define SAS_AUTH_TOKEN_BYTES      (16U) ///< Random bytes behind the 32-hex-digit session token.
#define SAS_AUTH_MAX_FAIL_CLIENTS (8U)  ///< Clients tracked for failed-login throttling.
#define SAS_AUTH_MAX_USERNAME_LEN (32U) ///< Longest username accepted, excluding the NUL.

#define SAS_AUTH_DEFAULT_SESSION_TIMEOUT_S     (3600)
#define SAS_AUTH_DEFAULT_FAILED_LOGIN_DELAY_MS (2000)

/*! \brief One slot of the static session pool. */
typedef struct
{
    bool in_use;                ///< false if this slot is free.
    sas_auth_session_t session; ///< Issued session.
} sas_auth_slot_t;

/*! \brief Last failed login of one client, for throttling. */
typedef struct
{
    bool in_use;             ///< false if this slot is free.
    uint32_t client_id;      ///< Caller-supplied client identity.
    int64_t last_failure_us; ///< esp_timer_get_time() at the client's last failure.
} sas_auth_fail_record_t;

static bool s_initialized = false;
static bool s_started     = false;
static bool s_enabled     = true;

static SemaphoreHandle_t s_lock = NULL;
static StaticSemaphore_t s_lock_storage;

static sas_auth_slot_t s_sessions[SAS_AUTH_MAX_SESSIONS];
static sas_auth_fail_record_t s_fail_records[SAS_AUTH_MAX_FAIL_CLIENTS];

static uint32_t s_max_sessions         = SAS_AUTH_MAX_SESSIONS;
static int64_t s_session_timeout_us    = (int64_t)SAS_AUTH_DEFAULT_SESSION_TIMEOUT_S * 1000000LL;
static int64_t s_failed_login_delay_us = (int64_t)SAS_AUTH_DEFAULT_FAILED_LOGIN_DELAY_MS * 1000LL;

static bool s_use_hw_key         = false;     ///< true: eFuse key via the HMAC peripheral.
static hmac_key_id_t s_hw_key_id = HMAC_KEY0; ///< Key selector only; never the key value.
static uint8_t s_sw_key[SAS_AUTH_HASH_LEN];   ///< Software-fallback key (auth.sw_key).

static void load_config(void);
static sas_status_t select_hmac_key(void);
static sas_status_t seed_default_credential_if_missing(void);
static sas_status_t store_credential(const char *password);
static sas_status_t compute_hash(const char *password, const uint8_t *salt, uint8_t *out_hash);
static sas_status_t verify_password(const char *password);
static bool constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len);
static void bytes_to_hex(const uint8_t *in, size_t len, char *out);
static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len);
static void wipe(void *buf, size_t len);
static void get_string_config(const char *key, const char *default_val, char *out, size_t out_len);
static int32_t get_int_config(const char *key, int32_t default_val);
static sas_status_t set_string_config(const char *key, const char *value);
static sas_auth_fail_record_t *find_fail_record(uint32_t client_id);
static void record_failure(uint32_t client_id, int64_t now_us);
static void issue_session(int64_t now_us, sas_auth_session_t *out_session);
static sas_auth_slot_t *find_session(const char *token);

/*!
    \brief   Initialize the Authentication module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already
             initialized.
*/
sas_status_t sas_auth_init(void)
{
    if (s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    (void)memset(s_sessions, 0, sizeof(s_sessions));
    (void)memset(s_fail_records, 0, sizeof(s_fail_records));

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    configASSERT(s_lock != NULL);

    load_config();

    s_started     = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Start active authentication operation (accept logins and validate
             tokens).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_auth_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    sas_status_t status = select_hmac_key();
    if (status == SAS_STATUS_OK)
    {
        status = seed_default_credential_if_missing();
    }
    if (status != SAS_STATUS_OK)
    {
        return status;
    }

    if (!s_enabled)
    {
        sas_log_warn("auth.enabled is false: protected routes and commands are open");
    }

    s_started = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Stop active authentication operation without releasing resources
             or clearing existing sessions.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_auth_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    // Sessions and failed-attempt records are deliberately kept.
    s_started = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Release all resources acquired by sas_auth_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_auth_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_lock != NULL)
    {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    wipe(s_sessions, sizeof(s_sessions));
    wipe(s_sw_key, sizeof(s_sw_key));
    (void)memset(s_fail_records, 0, sizeof(s_fail_records));

    s_started     = false;
    s_initialized = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Query whether protected routes/commands must present a token.
    \return  false only once the module has started with auth.enabled set to
             false; true otherwise, including before start, so a module that
             failed to boot leaves protected resources closed rather than open.
*/
bool sas_auth_is_enabled(void)
{
    return !(s_started && !s_enabled);
}

/*!
    \brief   Verify a username/password pair and, on success, issue a new
             session token.
    \param[in]  username     Non-NULL username to authenticate.
    \param[in]  password     Non-NULL plaintext password; never persisted.
                             Every internal copy is cleared before returning;
                             the caller clears its own buffer.
    \param[out] out_session  Non-NULL, receives the issued session on success.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if any argument is
             NULL; SAS_ERR_INVALID_STATE if not started;
             SAS_AUTH_ERR_INVALID_CREDENTIAL on a wrong username/password;
             SAS_AUTH_ERR_RATE_LIMITED if throttled per
             auth.failed_login_delay_ms.
*/
sas_status_t sas_auth_login(const char *username, const char *password,
                            sas_auth_session_t *out_session)
{
    return sas_auth_login_client(0U, username, password, out_session, NULL);
}

/*!
    \brief   sas_auth_login() with an explicit caller identity, so failed
             attempts are throttled per client (SAS-AUTH-NFR-001) rather than
             for every caller at once. sas_auth_login() is this call with
             client_id 0.
    \param[in]  client_id       Opaque identity of the remote client, e.g. its
                                IPv4 address.
    \param[in]  username        Non-NULL username to authenticate.
    \param[in]  password        Non-NULL plaintext password; never persisted.
    \param[out] out_session     Non-NULL, receives the issued session on success.
    \param[out] retry_after_ms  Optional (may be NULL); on
                                SAS_AUTH_ERR_RATE_LIMITED receives the remaining
                                delay in milliseconds.
    \return  Same codes as sas_auth_login(), plus SAS_ERR_TIMEOUT if the module
             lock is busy.
*/
sas_status_t sas_auth_login_client(uint32_t client_id, const char *username, const char *password,
                                   sas_auth_session_t *out_session, uint32_t *retry_after_ms)
{
    if ((username == NULL) || (password == NULL) || (out_session == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, SAS_AUTH_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    int64_t now_us      = esp_timer_get_time();

    const sas_auth_fail_record_t *record = find_fail_record(client_id);
    if ((record != NULL) && ((now_us - record->last_failure_us) < s_failed_login_delay_us))
    {
        if (retry_after_ms != NULL)
        {
            int64_t remaining_us = s_failed_login_delay_us - (now_us - record->last_failure_us);
            *retry_after_ms      = (uint32_t)((remaining_us + 999LL) / 1000LL);
        }
        status = SAS_AUTH_ERR_RATE_LIMITED;
    }

    if (status == SAS_STATUS_OK)
    {
        char expected_username[SAS_AUTH_MAX_USERNAME_LEN + 1U];
        get_string_config("auth.username", SAS_AUTH_DEFAULT_USERNAME, expected_username,
                          sizeof(expected_username));

        // Evaluate both checks unconditionally so a wrong username costs the same as a wrong
        // password.
        bool username_ok = (strcmp(username, expected_username) == 0);
        bool password_ok = (verify_password(password) == SAS_STATUS_OK);

        if (username_ok && password_ok)
        {
            issue_session(now_us, out_session);
        }
        else
        {
            record_failure(client_id, now_us);
            status = SAS_AUTH_ERR_INVALID_CREDENTIAL;
        }
    }

    xSemaphoreGive(s_lock);

    // Publish outside the lock; never include the token or credential.
    if (status == SAS_STATUS_OK)
    {
        (void)sas_event_bus_publish(SAS_EVENT_AUTH_LOGIN, NULL, 0U);
    }
    else if (status == SAS_AUTH_ERR_RATE_LIMITED)
    {
        (void)sas_event_bus_publish(SAS_EVENT_AUTH_LOCKOUT, NULL, 0U);
    }
    else
    {
        // invalid credential: nothing to publish
    }
    return status;
}

/*!
    \brief   Validate a caller-presented session token.
    \param[in] token  Non-NULL, NUL-terminated token string.
    \return  SAS_STATUS_OK if the token is known and unexpired;
             SAS_ERR_INVALID_ARG if token is NULL; SAS_AUTH_ERR_TOKEN_UNKNOWN
             if the token is not in the session table;
             SAS_AUTH_ERR_TOKEN_EXPIRED if the token has expired.
*/
sas_status_t sas_auth_validate_token(const char *token)
{
    if (token == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, SAS_AUTH_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status   = SAS_STATUS_OK;
    sas_auth_slot_t *slot = find_session(token);
    if (slot == NULL)
    {
        status = SAS_AUTH_ERR_TOKEN_UNKNOWN;
    }
    else if (esp_timer_get_time() >= slot->session.expires_at_us)
    {
        // An expired token is invalidated, not just rejected
        // (SAS-AUTH-FR-007).
        wipe(slot, sizeof(*slot));
        status = SAS_AUTH_ERR_TOKEN_EXPIRED;
    }
    else
    {
        // known and unexpired
    }

    xSemaphoreGive(s_lock);
    return status;
}

/*!
    \brief   Invalidate a session token, ending that session.
    \param[in] token  Non-NULL, NUL-terminated token string.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if token is NULL;
             SAS_AUTH_ERR_TOKEN_UNKNOWN if the token is not in the session
             table.
*/
sas_status_t sas_auth_logout(const char *token)
{
    if (token == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, SAS_AUTH_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status   = SAS_STATUS_OK;
    sas_auth_slot_t *slot = find_session(token);
    if (slot == NULL)
    {
        status = SAS_AUTH_ERR_TOKEN_UNKNOWN;
    }
    else
    {
        wipe(slot, sizeof(*slot));
    }

    xSemaphoreGive(s_lock);

    if (status == SAS_STATUS_OK)
    {
        (void)sas_event_bus_publish(SAS_EVENT_AUTH_LOGOUT, NULL, 0U);
    }
    return status;
}

/*!
    \brief   Change the stored credential, verifying the current password
             first. On success every existing session is invalidated.
    \param[in] current_password  Non-NULL current plaintext password; never
                                 persisted (see sas_auth_login()).
    \param[in] new_password      Non-NULL new plaintext password of
                                 SAS_AUTH_MIN_PASSWORD_LEN to
                                 SAS_AUTH_MAX_PASSWORD_LEN characters; never
                                 persisted.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if any argument is
             NULL or new_password's length is out of range;
             SAS_ERR_INVALID_STATE if not started;
             SAS_AUTH_ERR_INVALID_CREDENTIAL if current_password is wrong.
*/
sas_status_t sas_auth_change_credential(const char *current_password, const char *new_password)
{
    if ((current_password == NULL) || (new_password == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }
    size_t new_len = strlen(new_password);
    if ((new_len < SAS_AUTH_MIN_PASSWORD_LEN) || (new_len > SAS_AUTH_MAX_PASSWORD_LEN))
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, SAS_AUTH_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = verify_password(current_password);
    if (status == SAS_STATUS_OK)
    {
        status = store_credential(new_password);
    }
    if (status == SAS_STATUS_OK)
    {
        // A credential change ends every session issued under the old one.
        wipe(s_sessions, sizeof(s_sessions));
    }

    xSemaphoreGive(s_lock);
    return status;
}

/*!
    \brief   Read the auth.x tuning keys. Caller: sas_auth_init().
*/
static void load_config(void)
{
    sas_config_value_t val;
    sas_config_value_t def;

    def.type           = SAS_CFG_TYPE_BOOL;
    def.value.bool_val = true;
    (void)sas_config_get("auth.enabled", &val, &def);
    s_enabled = (val.type == SAS_CFG_TYPE_BOOL) ? val.value.bool_val : true;

    int32_t timeout_s =
        get_int_config("auth.session_timeout_s", SAS_AUTH_DEFAULT_SESSION_TIMEOUT_S);
    if (timeout_s < 1)
    {
        timeout_s = SAS_AUTH_DEFAULT_SESSION_TIMEOUT_S;
    }
    s_session_timeout_us = (int64_t)timeout_s * 1000000LL;

    int32_t delay_ms =
        get_int_config("auth.failed_login_delay_ms", SAS_AUTH_DEFAULT_FAILED_LOGIN_DELAY_MS);
    if (delay_ms < 0)
    {
        delay_ms = SAS_AUTH_DEFAULT_FAILED_LOGIN_DELAY_MS;
    }
    s_failed_login_delay_us = (int64_t)delay_ms * 1000LL;

    int32_t max_sessions = get_int_config("auth.max_sessions", (int32_t)SAS_AUTH_MAX_SESSIONS);
    if ((max_sessions < 1) || (max_sessions > (int32_t)SAS_AUTH_MAX_SESSIONS))
    {
        max_sessions = (int32_t)SAS_AUTH_MAX_SESSIONS;
    }
    s_max_sessions = (uint32_t)max_sessions;
}

/*!
    \brief   Decide which key the credential HMAC uses: an eFuse key block with
             purpose HMAC_UP if one is provisioned, otherwise a random software
             key persisted as auth.sw_key.
*/
static sas_status_t select_hmac_key(void)
{
    esp_efuse_block_t block = EFUSE_BLK_KEY0;
    if (esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &block))
    {
        s_hw_key_id  = (hmac_key_id_t)((int)block - (int)EFUSE_BLK_KEY0);
        s_use_hw_key = true;
        return SAS_STATUS_OK;
    }

    s_use_hw_key = false;
    sas_log_warn("no eFuse HMAC_UP key provisioned: using a software key (SAS-AUTH-NFR-003 unmet)");

    if (psa_crypto_init() != PSA_SUCCESS)
    {
        sas_log_error("psa_crypto_init failed");
        return SAS_ERR_INVALID_STATE;
    }

    char key_hex[(SAS_AUTH_HASH_LEN * 2U) + 1U];
    get_string_config("auth.sw_key", "", key_hex, sizeof(key_hex));
    if (!hex_to_bytes(key_hex, s_sw_key, sizeof(s_sw_key)))
    {
        // First start (or unreadable value): generate and persist a fresh key. Any stored
        // credential hash was keyed by the old one and is discarded with it.
        esp_fill_random(s_sw_key, sizeof(s_sw_key));
        bytes_to_hex(s_sw_key, sizeof(s_sw_key), key_hex);
        (void)set_string_config("auth.sw_key", key_hex);
        (void)set_string_config("auth.password_hash", "");
    }
    wipe(key_hex, sizeof(key_hex));
    return SAS_STATUS_OK;
}

/*!
    \brief   Seed the default credential when auth.password_hash is absent or
             malformed.
*/
static sas_status_t seed_default_credential_if_missing(void)
{
    char hash_hex[(SAS_AUTH_HASH_LEN * 2U) + 1U];
    uint8_t hash[SAS_AUTH_HASH_LEN];
    get_string_config("auth.password_hash", "", hash_hex, sizeof(hash_hex));
    if (hex_to_bytes(hash_hex, hash, sizeof(hash)))
    {
        return SAS_STATUS_OK;
    }

    sas_log_warn("no credential stored: seeding default login '%s' - change the password",
                 SAS_AUTH_DEFAULT_USERNAME);
    return store_credential(SAS_AUTH_DEFAULT_PASSWORD);
}

/*!
    \brief   Generate a fresh salt, hash password with it, and persist both.
*/
static sas_status_t store_credential(const char *password)
{
    uint8_t salt[SAS_AUTH_SALT_LEN];
    uint8_t hash[SAS_AUTH_HASH_LEN];
    esp_fill_random(salt, sizeof(salt));

    sas_status_t status = compute_hash(password, salt, hash);
    if (status != SAS_STATUS_OK)
    {
        return status;
    }

    char salt_hex[(SAS_AUTH_SALT_LEN * 2U) + 1U];
    char hash_hex[(SAS_AUTH_HASH_LEN * 2U) + 1U];
    bytes_to_hex(salt, sizeof(salt), salt_hex);
    bytes_to_hex(hash, sizeof(hash), hash_hex);

    status = set_string_config("auth.password_salt", salt_hex);
    if (status == SAS_STATUS_OK)
    {
        status = set_string_config("auth.password_hash", hash_hex);
    }
    return status;
}

/*!
    \brief   out_hash = HMAC-SHA256(key, password || salt), via the HMAC
             peripheral (eFuse key) or PSA (software-fallback key).
*/
static sas_status_t compute_hash(const char *password, const uint8_t *salt, uint8_t *out_hash)
{
    size_t password_len = strlen(password);
    if (password_len > SAS_AUTH_MAX_PASSWORD_LEN)
    {
        return SAS_AUTH_ERR_INVALID_CREDENTIAL;
    }

    uint8_t message[SAS_AUTH_MAX_PASSWORD_LEN + SAS_AUTH_SALT_LEN];
    (void)memcpy(message, password, password_len);
    (void)memcpy(&message[password_len], salt, SAS_AUTH_SALT_LEN);
    size_t message_len = password_len + SAS_AUTH_SALT_LEN;

    sas_status_t status = SAS_STATUS_OK;
    if (s_use_hw_key)
    {
        if (esp_hmac_calculate(s_hw_key_id, message, message_len, out_hash) != ESP_OK)
        {
            status = SAS_ERR_INVALID_STATE;
        }
    }
    else
    {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
        psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));

        psa_key_id_t key_id = 0;
        size_t mac_len      = 0U;
        if (psa_import_key(&attributes, s_sw_key, sizeof(s_sw_key), &key_id) != PSA_SUCCESS)
        {
            status = SAS_ERR_INVALID_STATE;
        }
        else
        {
            if ((psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), message, message_len,
                                 out_hash, SAS_AUTH_HASH_LEN, &mac_len) != PSA_SUCCESS) ||
                (mac_len != SAS_AUTH_HASH_LEN))
            {
                status = SAS_ERR_INVALID_STATE;
            }
            (void)psa_destroy_key(key_id);
        }
    }

    wipe(message, sizeof(message)); // held the plaintext password
    return status;
}

/*!
    \brief   Recompute the hash of password with the stored salt and compare
             it, in constant time, to the stored hash.
*/
static sas_status_t verify_password(const char *password)
{
    char salt_hex[(SAS_AUTH_SALT_LEN * 2U) + 1U];
    char hash_hex[(SAS_AUTH_HASH_LEN * 2U) + 1U];
    uint8_t salt[SAS_AUTH_SALT_LEN];
    uint8_t stored[SAS_AUTH_HASH_LEN];
    uint8_t computed[SAS_AUTH_HASH_LEN];

    get_string_config("auth.password_salt", "", salt_hex, sizeof(salt_hex));
    get_string_config("auth.password_hash", "", hash_hex, sizeof(hash_hex));
    if (!hex_to_bytes(salt_hex, salt, sizeof(salt)) ||
        !hex_to_bytes(hash_hex, stored, sizeof(stored)))
    {
        return SAS_AUTH_ERR_INVALID_CREDENTIAL;
    }

    if (compute_hash(password, salt, computed) != SAS_STATUS_OK)
    {
        return SAS_AUTH_ERR_INVALID_CREDENTIAL;
    }

    bool match = constant_time_equal(stored, computed, sizeof(stored));
    wipe(computed, sizeof(computed));
    return match ? SAS_STATUS_OK : SAS_AUTH_ERR_INVALID_CREDENTIAL;
}

/*!
    \brief   Compare two buffers without an early exit on the first difference.
*/
static bool constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0U;
    for (size_t i = 0U; i < len; i++)
    {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0U;
}

/*!
    \brief   Hex-encode len bytes of in into out (out holds 2 * len + 1
             characters).
*/
static void bytes_to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0U; i < len; i++)
    {
        out[i * 2U]        = digits[in[i] >> 4];
        out[(i * 2U) + 1U] = digits[in[i] & 0x0FU];
    }
    out[len * 2U] = '\0';
}

/*!
    \brief   Decode exactly out_len bytes from hex; false if hex is not 2 *
             out_len hex digits.
*/
static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != (out_len * 2U))
    {
        return false;
    }
    for (size_t i = 0U; i < (out_len * 2U); i++)
    {
        char c = hex[i];
        uint8_t nibble;
        if ((c >= '0') && (c <= '9'))
        {
            nibble = (uint8_t)(c - '0');
        }
        else if ((c >= 'a') && (c <= 'f'))
        {
            nibble = (uint8_t)(c - 'a' + 10);
        }
        else if ((c >= 'A') && (c <= 'F'))
        {
            nibble = (uint8_t)(c - 'A' + 10);
        }
        else
        {
            return false;
        }
        if ((i % 2U) == 0U)
        {
            out[i / 2U] = (uint8_t)(nibble << 4);
        }
        else
        {
            out[i / 2U] |= nibble;
        }
    }
    return true;
}

/*!
    \brief   memset() that the compiler may not elide, for buffers that held
             secrets.
*/
static void wipe(void *buf, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;
    for (size_t i = 0U; i < len; i++)
    {
        p[i] = 0U;
    }
}

/*!
    \brief   Read a string config key into out, falling back to default_val.
*/
static void get_string_config(const char *key, const char *default_val, char *out, size_t out_len)
{
    sas_config_value_t val;
    sas_config_value_t def;

    def.type = SAS_CFG_TYPE_STRING;
    (void)strncpy(def.value.str_val, default_val, sizeof(def.value.str_val) - 1U);
    def.value.str_val[sizeof(def.value.str_val) - 1U] = '\0';

    (void)sas_config_get(key, &val, &def);
    const char *src = (val.type == SAS_CFG_TYPE_STRING) ? val.value.str_val : default_val;
    (void)strncpy(out, src, out_len - 1U);
    out[out_len - 1U] = '\0';

    wipe(&val, sizeof(val));
    wipe(&def, sizeof(def));
}

/*!
    \brief   Read an int config key, falling back to default_val.
*/
static int32_t get_int_config(const char *key, int32_t default_val)
{
    sas_config_value_t val;
    sas_config_value_t def;

    def.type          = SAS_CFG_TYPE_INT;
    def.value.int_val = default_val;
    (void)sas_config_get(key, &val, &def);
    return (val.type == SAS_CFG_TYPE_INT) ? val.value.int_val : default_val;
}

/*!
    \brief   Store a string config key.
*/
static sas_status_t set_string_config(const char *key, const char *value)
{
    sas_config_value_t val;
    val.type = SAS_CFG_TYPE_STRING;
    (void)strncpy(val.value.str_val, value, sizeof(val.value.str_val) - 1U);
    val.value.str_val[sizeof(val.value.str_val) - 1U] = '\0';

    sas_status_t status = sas_config_set(key, &val);
    wipe(&val, sizeof(val));
    return status;
}

/*!
    \brief   Find client_id's failed-attempt record, or NULL. Caller holds
             s_lock.
*/
static sas_auth_fail_record_t *find_fail_record(uint32_t client_id)
{
    for (size_t i = 0U; i < SAS_AUTH_MAX_FAIL_CLIENTS; i++)
    {
        if (s_fail_records[i].in_use && (s_fail_records[i].client_id == client_id))
        {
            return &s_fail_records[i];
        }
    }
    return NULL;
}

/*!
    \brief   Record a failed login for client_id, reusing the stalest record
             when the table is full. Caller holds s_lock.
*/
static void record_failure(uint32_t client_id, int64_t now_us)
{
    sas_auth_fail_record_t *record = find_fail_record(client_id);
    if (record == NULL)
    {
        record = &s_fail_records[0];
        for (size_t i = 0U; i < SAS_AUTH_MAX_FAIL_CLIENTS; i++)
        {
            if (!s_fail_records[i].in_use)
            {
                record = &s_fail_records[i];
                break;
            }
            if (s_fail_records[i].last_failure_us < record->last_failure_us)
            {
                record = &s_fail_records[i];
            }
        }
    }

    record->in_use          = true;
    record->client_id       = client_id;
    record->last_failure_us = now_us;
}

/*!
    \brief   Allocate a session from the pool - a free or expired slot, else
             the oldest one (SAS-AUTH-NFR-002) - and fill *out_session. Caller
             holds s_lock.
*/
static void issue_session(int64_t now_us, sas_auth_session_t *out_session)
{
    sas_auth_slot_t *slot = NULL;
    for (size_t i = 0U; i < s_max_sessions; i++)
    {
        if (!s_sessions[i].in_use || (now_us >= s_sessions[i].session.expires_at_us))
        {
            slot = &s_sessions[i];
            break;
        }
        if ((slot == NULL) || (s_sessions[i].session.issued_at_us < slot->session.issued_at_us))
        {
            slot = &s_sessions[i];
        }
    }

    uint8_t token_bytes[SAS_AUTH_TOKEN_BYTES];
    esp_fill_random(token_bytes, sizeof(token_bytes));

    slot->in_use = true;
    bytes_to_hex(token_bytes, sizeof(token_bytes), slot->session.token);
    slot->session.issued_at_us  = now_us;
    slot->session.expires_at_us = now_us + s_session_timeout_us;
    *out_session                = slot->session;

    wipe(token_bytes, sizeof(token_bytes));
}

/*!
    \brief   Find the session holding token, or NULL. Caller holds s_lock.
*/
static sas_auth_slot_t *find_session(const char *token)
{
    size_t token_len = strlen(token);
    if (token_len != (SAS_AUTH_TOKEN_BYTES * 2U))
    {
        return NULL;
    }
    for (size_t i = 0U; i < SAS_AUTH_MAX_SESSIONS; i++)
    {
        if (s_sessions[i].in_use &&
            constant_time_equal((const uint8_t *)s_sessions[i].session.token,
                                (const uint8_t *)token, token_len))
        {
            return &s_sessions[i];
        }
    }
    return NULL;
}
