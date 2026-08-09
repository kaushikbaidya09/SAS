/*!
    \file    sas_auth.h
    \brief   Public interface for the Authentication module.

    \description
    Session-token based authentication for the SAS HTTP Server and Terminal.
    Verifies user credentials against a hardware-HMAC-protected hash, issues
    bearer session tokens, and validates tokens presented by callers on
    routes/commands that require an authenticated caller.

    \dependencies
    - Configuration Manager (main/src/core/sas_config.h): auth.password_hash,
      auth.password_salt, auth.session_timeout_s, auth.failed_login_delay_ms,
      auth.max_sessions.
    - HTTP Server (main/src/web/sas_http_server.h): calls
      sas_auth_validate_token() for routes with requires_auth = true.
    - Event Bus (main/src/core/sas_event_bus.h): publishes login/logout/
      lockout events for other modules to observe.
*/

#pragma once

/* 1. Standard includes */
#include <stdint.h>

/* 2. ESP-IDF includes */
/* (none) */

/* 3. Project includes */
#include "../core/sas_status.h"

/* 4. Public types */

/*! An issued authentication session. */
typedef struct
{
    char token[33];        ///< 32 hex-digit session token, NUL-terminated.
    int64_t issued_at_us;  ///< Issue timestamp, microseconds since boot/epoch.
    int64_t expires_at_us; ///< Expiry timestamp, microseconds since boot/epoch.
} sas_auth_session_t;

/* 5. Public constants */

#define SAS_AUTH_ERR_BASE (0x8D00U) ///< Custom status base for this module.
#define SAS_AUTH_ERR_INVALID_CREDENTIAL \
    (SAS_AUTH_ERR_BASE + 1U) ///< Login attempted with a wrong username/password.
#define SAS_AUTH_ERR_TOKEN_EXPIRED \
    (SAS_AUTH_ERR_BASE +           \
     2U) ///< sas_auth_validate_token() called with a token past its expiry time.
#define SAS_AUTH_ERR_TOKEN_UNKNOWN \
    (SAS_AUTH_ERR_BASE + 3U) ///< sas_auth_validate_token()/sas_auth_logout() called with a token
                             ///< not in the session table.
#define SAS_AUTH_ERR_RATE_LIMITED \
    (SAS_AUTH_ERR_BASE + 4U) ///< Login attempt arrived before auth.failed_login_delay_ms elapsed
                             ///< since the client's last failure.

#define SAS_AUTH_MAX_SESSIONS \
    (4U) ///< Session pool size placeholder; reconcile with auth.max_sessions at implementation
         ///< time.

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
    \brief   Verify a username/password pair and, on success, issue a new
             session token.
    \param   username    Non-NULL username to authenticate.
    \param   password    Non-NULL plaintext password; cleared by the callee
                          immediately after use, never persisted.
    \param   out_session Non-NULL, receives the issued session on success.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if any argument
             is NULL; SAS_AUTH_ERR_INVALID_CREDENTIAL on a wrong
             username/password; SAS_AUTH_ERR_RATE_LIMITED if throttled per
             auth.failed_login_delay_ms.
*/
sas_status_t sas_auth_login(const char *username, const char *password,
                            sas_auth_session_t *out_session);

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
             first.
    \param   current_password Non-NULL current plaintext password; cleared
                               by the callee immediately after use, never
                               persisted.
    \param   new_password      Non-NULL new plaintext password; cleared by
                               the callee immediately after use, never
                               persisted.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if any argument
             is NULL; SAS_AUTH_ERR_INVALID_CREDENTIAL if current_password
             is wrong.
*/
sas_status_t sas_auth_change_credential(const char *current_password, const char *new_password);
