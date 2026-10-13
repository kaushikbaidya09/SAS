//=============================================================================
/*!
    \file    sas_http_server.h
    \brief   Public interface for the HTTP Server module.
*/
//=============================================================================

// Owns the single HTTPS server and lets other modules register routes and
// WebSocket handlers on it. Requests that match no route are served as static
// files from /www/. Plain HTTP is never served.

#ifndef SAS_HTTP_SERVER_H
#define SAS_HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#include "../core/sas_status.h"

/*! \brief Signature for a route handler registered with sas_http_register_route().
    Must stay esp_err_t: this is the one ESP-IDF-mandated exception to the
    project's sas_status_t convention, required by esp_https_server's
    httpd_uri_t.handler contract. */
typedef esp_err_t (*sas_http_handler_fn)(httpd_req_t *req);

/*! \brief A single route registered with the HTTP Server module. */
typedef struct
{
    const char *uri;             ///< Route path, e.g. "/api/status".
    httpd_method_t method;       ///< HTTP method this route matches.
    sas_http_handler_fn handler; ///< Handler invoked on a match.
    bool requires_auth;          ///< True to require a valid auth token before dispatch.
} sas_http_route_t;

/*! \brief Custom sas_status_t base for this module. */
#define SAS_HTTP_ERR_BASE (0x8700U)
/*! \brief register_route() called with a (uri, method) pair that is already registered. */
#define SAS_HTTP_ERR_ROUTE_CONFLICT (SAS_HTTP_ERR_BASE + 1U)
/*! \brief A client TLS handshake failed; the affected connection is closed, others are unaffected.
 */
#define SAS_HTTP_ERR_TLS_HANDSHAKE_FAILED (SAS_HTTP_ERR_BASE + 2U)

sas_status_t sas_http_init(void);

sas_status_t sas_http_start(void);

sas_status_t sas_http_stop(void);

sas_status_t sas_http_deinit(void);

sas_status_t sas_http_register_route(const sas_http_route_t *route);

sas_status_t sas_http_register_ws_handler(const char *uri, httpd_uri_t *ws_uri_handler);

httpd_handle_t sas_http_get_server_handle(void);

sas_status_t sas_http_get_bearer_token(httpd_req_t *req, char *out, size_t out_len);

#endif /* SAS_HTTP_SERVER_H */
