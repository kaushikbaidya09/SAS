/*!
    \file    sas_auth.h
    \brief   Public interface for the Authentication module.

    \description
    Session-token based authentication for the SAS HTTP Server and Terminal.
    Verifies user credentials against a hardware-HMAC-protected hash, issues
    bearer session tokens, and validates tokens presented by callers on
    routes/commands that require an authenticated caller.

    The credential hash is HMAC-SHA256(password || salt). It is keyed by the
    ESP32-S3 HMAC peripheral when an eFuse key block with purpose HMAC_UP has
    been provisioned; application code then never sees the key. On a device
    with no such key block the module falls back to a random per-device
    software key held in the Configuration Manager (auth.sw_key) and logs a
    warning at start: that fallback keeps an unprovisioned board usable but
    does not meet SAS-AUTH-NFR-003. This module never burns eFuses itself.

    On first start with no stored credential, the default credential
    SAS_AUTH_DEFAULT_USERNAME / SAS_AUTH_DEFAULT_PASSWORD is seeded; change it
    with sas_auth_change_credential().

    \dependencies
    - Configuration Manager (main/src/core/sas_config.h): auth.enabled,
      auth.username, auth.password_hash, auth.password_salt,
      auth.session_timeout_s, auth.failed_login_delay_ms, auth.max_sessions,
      auth.sw_key (software-fallback HMAC key, see above).
    - HTTP Server (main/src/web/sas_http_server.h): calls
      sas_auth_validate_token() for routes with requires_auth = true.
    - Event Bus (main/src/core/sas_event_bus.h): publishes login/logout/
      lockout events for other modules to observe.
*/

#pragma once

/* 1. Standard includes */
#include <stdbool.h>
#include <stdint.h>

/* 2. ESP-IDF includes */
/* (none) */

/* 3. Project includes */
#include "../core/sas_status.h"

/* 4. Public types */

/*! An issued authentication session. */
typedef struct {
    char token[33];        ///< 32 hex-digit session token, NUL-terminated.
    int64_t issued_at_us;  ///< Issue timestamp, microseconds since boot.
    int64_t expires_at_us; ///< Expiry timestamp, microseconds since boot.
} sas_auth_session_t;

/* 5. Public constants */

#define SAS_AUTH_ERR_BASE (0x8D00U) ///< Custom status base for this module.
#define SAS_AUTH_ERR_INVALID_CREDENTIAL                                                            \
    (SAS_AUTH_ERR_BASE + 1U) ///< Login attempted with a wrong username/password.
#define SAS_AUTH_ERR_TOKEN_EXPIRED                                                                 \
    (SAS_AUTH_ERR_BASE +                                                                           \
     2U) ///< sas_auth_validate_token() called with a token past its expiry time.
#define SAS_AUTH_ERR_TOKEN_UNKNOWN                                                                 \
    (SAS_AUTH_ERR_BASE + 3U) ///< sas_auth_validate_token()/sas_auth_logout() called with a token
                             ///< not in the session table.
#define SAS_AUTH_ERR_RATE_LIMITED                                                                  \
    (SAS_AUTH_ERR_BASE + 4U) ///< Login attempt arrived before auth.failed_login_delay_ms elapsed
                             ///< since the client's last failure.

#define SAS_AUTH_MAX_SESSIONS                                                                      \
    (4U) ///< Static session pool size; auth.max_sessions may lower, but not raise, the number of
         ///< concurrently valid sessions.

#define SAS_AUTH_DEFAULT_USERNAME "admin" ///< Username seeded when auth.username is unset.
#define SAS_AUTH_DEFAULT_PASSWORD "admin" ///< Password seeded when no credential is stored.
#define SAS_AUTH_MIN_PASSWORD_LEN (8U)    ///< Shortest password change_credential() accepts.
#define SAS_AUTH_MAX_PASSWORD_LEN (64U)   ///< Longest password accepted, excluding the NUL.

/* 6. Public API declarations */

/*!
    \brief   Initialize the Authentication module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already initialized.
*/
sas_status_t sas_auth_init(void);

/*!
    \brief   Start active authentication operation (accept logins and
             validate tokens).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_auth_start(void);

/*!
    \brief   Stop active authentication operation without releasing
             resources or clearing existing sessions.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_auth_stop(void);

/*!
    \brief   Release all resources acquired by sas_auth_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_auth_deinit(void);

/*!
    \brief   Query whether protected routes/commands must present a token.
    \return  false only once the module has started with auth.enabled set
             to false; true otherwise, including before start, so a module
             that failed to boot leaves protected resources closed rather
             than open.
*/
bool sas_auth_is_enabled(void);

/*!
    \brief   Verify a username/password pair and, on success, issue a new
             session token.
    \param   username    Non-NULL username to authenticate.
    \param   password    Non-NULL plaintext password; never persisted. Every
                          internal copy is cleared before returning; the
                          caller clears its own buffer.
    \param   out_session Non-NULL, receives the issued session on success.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if any argument
             is NULL; SAS_ERR_INVALID_STATE if not started;
             SAS_AUTH_ERR_INVALID_CREDENTIAL on a wrong username/password;
             SAS_AUTH_ERR_RATE_LIMITED if throttled per
             auth.failed_login_delay_ms.
*/
sas_status_t sas_auth_login(const char *username, const char *password,
                            sas_auth_session_t *out_session);

/*!
    \brief   sas_auth_login() with an explicit caller identity, so failed
             attempts are throttled per client (SAS-AUTH-NFR-001) rather
             than for every caller at once. sas_auth_login() is this call
             with client_id 0.
    \param   client_id   Opaque identity of the remote client, e.g. its IPv4
                          address.
    \param   retry_after_ms Optional (may be NULL); on
                          SAS_AUTH_ERR_RATE_LIMITED receives the remaining
                          delay in milliseconds.
*/
sas_status_t sas_auth_login_client(uint32_t client_id, const char *username, const char *password,
                                   sas_auth_session_t *out_session, uint32_t *retry_after_ms);

/*!
    \brief   Validate a caller-presented session token.
    \param   token Non-NULL, NUL-terminated token string.
    \return  SAS_STATUS_OK if the token is known and unexpired;
             SAS_ERR_INVALID_ARG if token is NULL;
             SAS_AUTH_ERR_TOKEN_UNKNOWN if the token is not in the session
             table; SAS_AUTH_ERR_TOKEN_EXPIRED if the token has expired.
*/
sas_status_t sas_auth_validate_token(const char *token);

/*!
    \brief   Invalidate a session token, ending that session.
    \param   token Non-NULL, NUL-terminated token string.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if token is NULL;
             SAS_AUTH_ERR_TOKEN_UNKNOWN if the token is not in the session
             table.
*/
sas_status_t sas_auth_logout(const char *token);

/*!
    \brief   Change the stored credential, verifying the current password
             first. On success every existing session is invalidated.
    \param   current_password Non-NULL current plaintext password; never
                               persisted (see sas_auth_login()).
    \param   new_password      Non-NULL new plaintext password of
                               SAS_AUTH_MIN_PASSWORD_LEN to
                               SAS_AUTH_MAX_PASSWORD_LEN characters; never
                               persisted.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if any argument
             is NULL or new_password's length is out of range;
             SAS_ERR_INVALID_STATE if not started;
             SAS_AUTH_ERR_INVALID_CREDENTIAL if current_password is wrong.
*/
sas_status_t sas_auth_change_credential(const char *current_password, const char *new_password);
