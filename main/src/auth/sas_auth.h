//=============================================================================
/*!
    \file    sas_auth.h
    \brief   Public interface for the Authentication module.
*/
//=============================================================================

// Session-token authentication for the HTTP Server and Terminal: verifies a
// username and password, issues session tokens and validates them.
//
// The stored credential is HMAC-SHA256(password || salt). The key comes from
// the ESP32-S3 HMAC peripheral when an eFuse key block with purpose HMAC_UP is
// provisioned, so the application never sees it. Otherwise a random per-device
// software key from the configuration (auth.sw_key) is used and a warning is
// logged; that fallback does not meet SAS-AUTH-NFR-003.
//
// On first start the default credential SAS_AUTH_DEFAULT_USERNAME /
// SAS_AUTH_DEFAULT_PASSWORD is stored; change it with
// sas_auth_change_credential().

#ifndef SAS_AUTH_H
#define SAS_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/sas_status.h"

/*! \brief An issued authentication session. */
typedef struct
{
    char token[33];        ///< 32 hex-digit session token, NUL-terminated.
    int64_t issued_at_us;  ///< Issue timestamp, microseconds since boot.
    int64_t expires_at_us; ///< Expiry timestamp, microseconds since boot.
} sas_auth_session_t;

#define SAS_AUTH_ERR_BASE (0x8D00U) ///< Custom status base for this module.
/*! \brief Login attempted with a wrong username/password. */
#define SAS_AUTH_ERR_INVALID_CREDENTIAL (SAS_AUTH_ERR_BASE + 1U)
/*! \brief sas_auth_validate_token() called with a token past its expiry time. */
#define SAS_AUTH_ERR_TOKEN_EXPIRED (SAS_AUTH_ERR_BASE + 2U)
/*!
    \brief   sas_auth_validate_token()/sas_auth_logout() called with a token
             not in the session table.
*/
#define SAS_AUTH_ERR_TOKEN_UNKNOWN (SAS_AUTH_ERR_BASE + 3U)
/*!
    \brief   Login attempt arrived before auth.failed_login_delay_ms elapsed
             since the client's last failure.
*/
#define SAS_AUTH_ERR_RATE_LIMITED (SAS_AUTH_ERR_BASE + 4U)

/*!
    \brief   Static session pool size; auth.max_sessions may lower, but not
             raise, the number of concurrently valid sessions.
*/
#define SAS_AUTH_MAX_SESSIONS (4U)

#define SAS_AUTH_DEFAULT_USERNAME "admin" ///< Username seeded when auth.username is unset.
#define SAS_AUTH_DEFAULT_PASSWORD "admin" ///< Password seeded when no credential is stored.
#define SAS_AUTH_MIN_PASSWORD_LEN (8U)    ///< Shortest password change_credential() accepts.
#define SAS_AUTH_MAX_PASSWORD_LEN (64U)   ///< Longest password accepted, excluding the NUL.

sas_status_t sas_auth_init(void);

sas_status_t sas_auth_start(void);

sas_status_t sas_auth_stop(void);

sas_status_t sas_auth_deinit(void);

bool sas_auth_is_enabled(void);

sas_status_t sas_auth_login(const char *username, const char *password,
                            sas_auth_session_t *out_session);

sas_status_t sas_auth_login_client(uint32_t client_id, const char *username, const char *password,
                                   sas_auth_session_t *out_session, uint32_t *retry_after_ms);

sas_status_t sas_auth_validate_token(const char *token);

sas_status_t sas_auth_logout(const char *token);

sas_status_t sas_auth_change_credential(const char *current_password, const char *new_password);

sas_status_t sas_auth_change_username(const char *current_password, const char *new_username);

sas_status_t sas_auth_get_username(char *out, size_t out_len);

#endif /* SAS_AUTH_H */
