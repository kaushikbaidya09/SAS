/*!
    \file    sas_http_server.h
    \brief   Public interface for the HTTP Server module.

    \description
    Wraps a single embedded HTTPS server instance (esp_https_server) and
    exposes route/WebSocket-handler registration to the rest of the Web
    layer, so no other module owns its own server instance. Serves HTTPS
    only; no plaintext HTTP listener is ever opened.

    \dependencies
    - File Manager (main/src/storage/sas_file_manager.h): serves static
      assets from /www/ for requests that fall through to the default
      handler.
    - Event Bus (main/src/core/sas_event_bus.h): publishes server
      lifecycle and connection events for other modules to consume.
    - Authentication (main/src/auth/sas_auth.h): validates the bearer token
      on every route registered with requires_auth = true (SAS-HTTP-FR-005).
*/

#pragma once

/* 1. Standard includes */
#include <stdbool.h>
#include <stddef.h>

/* 2. ESP-IDF includes */
#include "esp_err.h"
#include "esp_http_server.h"

/* 3. Project includes */
#include "../core/sas_status.h"

/* 4. Public types */

/*! Signature for a route handler registered with sas_http_register_route().
    Must stay esp_err_t: this is the one ESP-IDF-mandated exception to the
    project's sas_status_t convention, required by esp_https_server's
    httpd_uri_t.handler contract. */
typedef esp_err_t (*sas_http_handler_fn)(httpd_req_t *req);

/*! A single route registered with the HTTP Server module. */
typedef struct {
    const char *uri;             ///< Route path, e.g. "/api/status".
    httpd_method_t method;       ///< HTTP method this route matches.
    sas_http_handler_fn handler; ///< Handler invoked on a match.
    bool requires_auth;          ///< True to require a valid auth token before dispatch.
} sas_http_route_t;

/* 5. Public constants */

/*! Custom sas_status_t base for this module. */
#define SAS_HTTP_ERR_BASE (0x8700U)
/*! register_route() called with a (uri, method) pair that is already registered. */
#define SAS_HTTP_ERR_ROUTE_CONFLICT (SAS_HTTP_ERR_BASE + 1U)
/*! A client TLS handshake failed; the affected connection is closed, others are unaffected. */
#define SAS_HTTP_ERR_TLS_HANDSHAKE_FAILED (SAS_HTTP_ERR_BASE + 2U)

/* 6. Public API declarations */

/*!
    \brief   Initialize the HTTP Server module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already
             initialized.
*/
sas_status_t sas_http_init(void);

/*!
    \brief   Start the HTTPS listener and begin serving registered routes.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_http_start(void);

/*!
    \brief   Stop the HTTPS listener without releasing registered routes.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_http_stop(void);

/*!
    \brief   Release all resources acquired by sas_http_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_http_deinit(void);

/*!
    \brief   Register a route to be served once the HTTPS listener starts.
    \param   route Non-NULL route descriptor; copied internally.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if route (or a
             required field) is NULL; SAS_HTTP_ERR_ROUTE_CONFLICT if the
             (uri, method) pair is already registered.
*/
sas_status_t sas_http_register_route(const sas_http_route_t *route);

/*!
    \brief   Register a WebSocket handler on the shared HTTPS server instance.
    \param   uri             Non-NULL WebSocket endpoint path.
    \param   ws_uri_handler  Non-NULL ESP-IDF httpd_uri_t descriptor
                             (is_websocket set by the caller); copied
                             internally.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if uri or
             ws_uri_handler is NULL; SAS_HTTP_ERR_ROUTE_CONFLICT if uri is
             already registered.
*/
sas_status_t sas_http_register_ws_handler(const char *uri, httpd_uri_t *ws_uri_handler);

/*!
    \brief   Get the shared httpd_ssl server handle, for callers (WebSocket)
             that need it directly - e.g. httpd_queue_work()/
             httpd_ws_send_frame_async() to send from a task other than the
             one currently dispatching a request.
    \return  The server handle if sas_http_start() has been called and
             sas_http_stop() has not since; NULL otherwise.
*/
httpd_handle_t sas_http_get_server_handle(void);

/*!
    \brief   Extract the session token from a request's
             "Authorization: Bearer <token>" header.
    \param   req     Non-NULL request.
    \param   out     Non-NULL destination for the NUL-terminated token.
    \param   out_len Capacity of out in bytes (33 holds any token).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if req or out is
             NULL or out_len is 0; SAS_ERR_INVALID_STATE if the header is
             absent, is not a Bearer credential, or does not fit out.
*/
sas_status_t sas_http_get_bearer_token(httpd_req_t *req, char *out, size_t out_len);
