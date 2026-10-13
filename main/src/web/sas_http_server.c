/*!
    \file    sas_http_server.c
    \brief   Implementation of the HTTP Server module.

    \description
    See sas_http_server.h for the public contract. Wraps a single
    esp_https_server (httpd_ssl) instance; the underlying esp-idf httpd
    dispatches by first-registration-match against
    httpd_uri_match_wildcard(), so every route accepted by
    sas_http_register_route()/sas_http_register_ws_handler() is registered
    with the real server before default_static_handler()'s "/x" wildcard
    catch-all, in that order, in sas_http_start().

    \note    httpd_ssl_config_t's server-certificate field is named
             servercert/servercert_len as of ESP-IDF v6.0.2 (this project's
             pinned version); cacert_pem/cacert_len now holds the CA cert
             used to verify client certificates, not the server cert.
             Re-check this if the pinned ESP-IDF version changes. Same
             caveat already flagged in sas_ble_scanner.c for NimBLE.

    Configuration keys read by this implementation (Configuration Manager):
    - http.max_connections (default 4)
    - http.request_timeout_ms (default 5000)

    Event Bus lifecycle/connection event publishing mentioned in this
    module's header \dependencies is deferred: no event ID for it exists
    yet in sas_event_bus.h and nothing in the current dashboard feature
    (Wi-Fi/BLE/log display) consumes it, so it is intentionally not
    implemented here rather than speculatively invented.
*/

/* 1. Includes */
#include "sas_http_server.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_https_server.h"

#include "../auth/sas_auth.h"
#include "../core/sas_config.h"
#include "../core/sas_logger.h"
#include "../storage/sas_file_manager.h"

static const char *TAG = "sas_http";

/* 2. Private macros */
#define SAS_HTTP_MAX_ROUTES (40U)   ///< Static route-table capacity; covers the full REST API.
#define SAS_HTTP_MAX_WS_ROUTES (2U) ///< Static WebSocket-route-table capacity; placeholder sizing.
#define SAS_HTTP_STATIC_PATH_MAX_LEN \
    (128U)                                 ///< Max length of a File-Manager path built from a request URI in
                                           ///< default_static_handler(), including the "/www" prefix and NUL.
#define SAS_HTTP_STATIC_READ_CHUNK (2048U) ///< Chunk size streaming a static file to the client.
#define SAS_HTTP_TASK_STACK_SIZE \
    (8192U) ///< httpd task stack: handlers run flash writes, HMAC and JSON building on it.
#define SAS_HTTP_PRIVATE_PATH_PREFIX \
    "/config" ///< URI prefix never served statically: Configuration Manager's backing store.

/* Embedded self-signed TLS cert/key (main/certs/, EMBED_TXTFILES in
 * main/CMakeLists.txt); symbol names are the file's basename with '.'
 * replaced by '_', plus _start/_end, per ESP-IDF's EMBED_TXTFILES
 * convention. */
extern const uint8_t server_cert_pem_start[] asm("_binary_server_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_server_cert_pem_end");
extern const uint8_t server_key_pem_start[] asm("_binary_server_key_pem_start");
extern const uint8_t server_key_pem_end[] asm("_binary_server_key_pem_end");

/* 3. Private typedefs */

/*! One stored WebSocket route, pending registration with the real server in sas_http_start(). */
typedef struct
{
    char uri[32];            ///< WebSocket endpoint path.
    httpd_uri_t uri_handler; ///< Caller-supplied descriptor, copied in.
} sas_http_ws_route_t;

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;

static httpd_handle_t s_server = NULL;
static bool s_fallback_registered = false; ///< See reregister_fallback_last()'s doc comment.

static sas_http_route_t s_routes[SAS_HTTP_MAX_ROUTES];
static size_t s_route_count = 0U;

static sas_http_ws_route_t s_ws_routes[SAS_HTTP_MAX_WS_ROUTES];
static size_t s_ws_route_count = 0U;

static uint32_t s_max_connections = 4U;
static uint32_t s_request_timeout_ms = 5000U;

/* 5. Forward declarations */
/* default_static_handler() and dispatch_route() stay esp_err_t: the same ESP-IDF-mandated
   exception as sas_http_handler_fn, see sas_http_server.h. */
static esp_err_t default_static_handler(httpd_req_t *req);
static esp_err_t dispatch_route(httpd_req_t *req);
static void load_config(void);
static const char *content_type_for_path(const char *path);
static void register_route_with_server(size_t index);
static void register_ws_route_with_server(size_t index);
static void remove_fallback(void);
static void reregister_fallback_last(void);

/* 6. Public functions */

sas_status_t sas_http_init(void)
{
    if (s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    (void)memset(s_routes, 0, sizeof(s_routes));
    (void)memset(s_ws_routes, 0, sizeof(s_ws_routes));
    s_route_count = 0U;
    s_ws_route_count = 0U;
    s_server = NULL;

    load_config();

    s_started = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

#define SAS_HTTP_ENABLE_SSL 0U

sas_status_t sas_http_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    esp_err_t err;

#if SAS_HTTP_ENABLE_SSL

    /* HTTPS */
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();

    conf.servercert = server_cert_pem_start;
    conf.servercert_len =
        (size_t)(server_cert_pem_end - server_cert_pem_start);

    conf.prvtkey_pem = server_key_pem_start;
    conf.prvtkey_len =
        (size_t)(server_key_pem_end - server_key_pem_start);

    conf.httpd.max_open_sockets = (uint8_t)s_max_connections;
    conf.httpd.stack_size = SAS_HTTP_TASK_STACK_SIZE;
    conf.httpd.lru_purge_enable = true;
    conf.httpd.max_uri_handlers =
        SAS_HTTP_MAX_ROUTES + SAS_HTTP_MAX_WS_ROUTES + 1U;
    conf.httpd.uri_match_fn = httpd_uri_match_wildcard;
    conf.httpd.recv_wait_timeout =
        (uint16_t)((s_request_timeout_ms + 999U) / 1000U);
    conf.httpd.send_wait_timeout =
        (uint16_t)((s_request_timeout_ms + 999U) / 1000U);

    err = httpd_ssl_start(&s_server, &conf);

    if (err != ESP_OK)
    {
        sas_log_error("httpd_ssl_start failed (0x%x)",
                      (unsigned int)err);
        return SAS_ERR_INVALID_STATE;
    }

#else

    /* HTTP */
    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();

    conf.max_open_sockets = (uint8_t)s_max_connections;
    conf.stack_size = SAS_HTTP_TASK_STACK_SIZE;
    conf.lru_purge_enable = true;
    conf.max_uri_handlers =
        SAS_HTTP_MAX_ROUTES + SAS_HTTP_MAX_WS_ROUTES + 1U;
    conf.uri_match_fn = httpd_uri_match_wildcard;
    conf.recv_wait_timeout =
        (uint16_t)((s_request_timeout_ms + 999U) / 1000U);
    conf.send_wait_timeout =
        (uint16_t)((s_request_timeout_ms + 999U) / 1000U);

    err = httpd_start(&s_server, &conf);

    if (err != ESP_OK)
    {
        sas_log_error("httpd_start failed (0x%x)",
                      (unsigned int)err);
        return SAS_ERR_INVALID_STATE;
    }

#endif

    for (size_t i = 0U; i < s_route_count; i++)
    {
        register_route_with_server(i);
    }

    for (size_t i = 0U; i < s_ws_route_count; i++)
    {
        register_ws_route_with_server(i);
    }

    reregister_fallback_last();

    s_started = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_http_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_server != NULL)
    {
#if SAS_HTTP_ENABLE_SSL
        (void)httpd_ssl_stop(s_server);
#else
        (void)httpd_stop(s_server);
#endif
        s_server = NULL;
    }
    s_fallback_registered = false;

    s_started = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_http_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    (void)memset(s_routes, 0, sizeof(s_routes));
    (void)memset(s_ws_routes, 0, sizeof(s_ws_routes));
    s_route_count = 0U;
    s_ws_route_count = 0U;

    s_initialized = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_http_register_route(const sas_http_route_t *route)
{
    if ((route == NULL) || (route->uri == NULL) || (route->handler == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    for (size_t i = 0U; i < s_route_count; i++)
    {
        if ((s_routes[i].method == route->method) && (strcmp(s_routes[i].uri, route->uri) == 0))
        {
            return SAS_HTTP_ERR_ROUTE_CONFLICT;
        }
    }
    if (s_route_count >= SAS_HTTP_MAX_ROUTES)
    {
        return SAS_ERR_NO_MEM;
    }

    s_routes[s_route_count] = *route;
    s_route_count++;
    if (s_started)
    {
        /* Server is already live (e.g. REST API/WebSocket register after
         * sas_http_start(), per the project's fixed Development Order in
         * main.c): apply immediately rather than only queuing for a batch
         * pass that already ran. httpd_register_uri_handler() is safe to
         * call on a running server. */
        remove_fallback();
        register_route_with_server(s_route_count - 1U);
        reregister_fallback_last();
    }
    return SAS_STATUS_OK;
}

sas_status_t sas_http_register_ws_handler(const char *uri, httpd_uri_t *ws_uri_handler)
{
    if ((uri == NULL) || (ws_uri_handler == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    for (size_t i = 0U; i < s_ws_route_count; i++)
    {
        if (strcmp(s_ws_routes[i].uri, uri) == 0)
        {
            return SAS_HTTP_ERR_ROUTE_CONFLICT;
        }
    }
    if (s_ws_route_count >= SAS_HTTP_MAX_WS_ROUTES)
    {
        return SAS_ERR_NO_MEM;
    }

    (void)strncpy(s_ws_routes[s_ws_route_count].uri, uri,
                  sizeof(s_ws_routes[s_ws_route_count].uri) - 1U);
    s_ws_routes[s_ws_route_count].uri[sizeof(s_ws_routes[s_ws_route_count].uri) - 1U] = '\0';
    s_ws_routes[s_ws_route_count].uri_handler = *ws_uri_handler;
    s_ws_routes[s_ws_route_count].uri_handler.uri = s_ws_routes[s_ws_route_count].uri;
    s_ws_route_count++;
    if (s_started)
    {
        /* See the matching note in sas_http_register_route(). */
        remove_fallback();
        register_ws_route_with_server(s_ws_route_count - 1U);
        reregister_fallback_last();
    }
    return SAS_STATUS_OK;
}

httpd_handle_t sas_http_get_server_handle(void)
{
    return s_server;
}

sas_status_t sas_http_get_bearer_token(httpd_req_t *req, char *out, size_t out_len)
{
    if ((req == NULL) || (out == NULL) || (out_len == 0U))
    {
        return SAS_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    static const char scheme[] = "Bearer ";
    char header[64];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (strncmp(header, scheme, sizeof(scheme) - 1U) != 0)
    {
        return SAS_ERR_INVALID_STATE;
    }

    const char *token = &header[sizeof(scheme) - 1U];
    if (strlen(token) >= out_len)
    {
        return SAS_ERR_INVALID_STATE;
    }
    (void)strcpy(out, token);
    return SAS_STATUS_OK;
}

/* 7. Private functions */

/*! \brief Load http.max_connections / http.request_timeout_ms. Caller: sas_http_init(), before
 * s_initialized is set. */
static void load_config(void)
{
    sas_config_value_t val;
    sas_config_value_t def;

    def.type = SAS_CFG_TYPE_INT;
    def.value.int_val = (int32_t)s_max_connections;
    (void)sas_config_get("http.max_connections", &val, &def);
    s_max_connections = (uint32_t)val.value.int_val;

    def.value.int_val = (int32_t)s_request_timeout_ms;
    (void)sas_config_get("http.request_timeout_ms", &val, &def);
    s_request_timeout_ms = (uint32_t)val.value.int_val;
}

/*!
    \brief   httpd dispatch trampoline for every route accepted by
             sas_http_register_route(): enforces requires_auth, then calls
             through to the owning module's handler.
    \note    A requires_auth route is only dispatched when the request
             carries a bearer token the Authentication module accepts;
             otherwise it is answered 401 with the REST API's error envelope
             and the handler is never invoked (SAS-HTTP-FR-005,
             SAS-AUTH-FR-004). Where Authentication is disabled
             (auth.enabled = false) the check is skipped.
*/
static esp_err_t dispatch_route(httpd_req_t *req)
{
    const sas_http_route_t *route = (const sas_http_route_t *)req->user_ctx;
    if (route == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (route->requires_auth && sas_auth_is_enabled())
    {
        char token[33];
        if ((sas_http_get_bearer_token(req, token, sizeof(token)) != SAS_STATUS_OK) ||
            (sas_auth_validate_token(token) != SAS_STATUS_OK))
        {
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_set_type(req, "application/json");
            (void)httpd_resp_sendstr(req, "{\"error_code\":\"unauthorized\","
                                          "\"message\":\"Missing, invalid or expired token\"}");
            return ESP_OK;
        }
    }

    return route->handler(req);
}

/*!
    \brief   Fallback URI handler for requests that do not match a
             registered route: serves /www/<uri> via the File Manager.
*/
static esp_err_t default_static_handler(httpd_req_t *req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char path[SAS_HTTP_STATIC_PATH_MAX_LEN];
    const char *uri_path = (strcmp(req->uri, "/") == 0) ? "/index.html" : req->uri;
    /* Strip a query string, if any, before mapping to a File Manager path. */
    size_t uri_len = strcspn(uri_path, "?");
    if ((uri_len + 4U /* "/www" */) >= sizeof(path))
    {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, NULL);
        return ESP_OK;
    }
    (void)snprintf(path, sizeof(path), "/www%.*s", (int)uri_len, uri_path);

    /* Never served: Configuration Manager's store (holds credentials), dotfiles, and any path
     * that tries to climb out of the web root. */
    const char *relative = &path[4]; /* past "/www" */
    size_t private_len = sizeof(SAS_HTTP_PRIVATE_PATH_PREFIX) - 1U;
    bool is_private = (strncmp(relative, SAS_HTTP_PRIVATE_PATH_PREFIX, private_len) == 0) &&
                      ((relative[private_len] == '\0') || (relative[private_len] == '/'));
    if (is_private || (strstr(path, "/.") != NULL))
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }

    /* static, not stack-local: keeps the httpd task's stack footprint small; safe because
     * esp_http_server runs one handler to completion at a time on that task. */
    static uint8_t chunk[SAS_HTTP_STATIC_READ_CHUNK];
    size_t offset = 0U;
    size_t total_read = 0U;
    sas_status_t status = sas_fs_read(path, offset, chunk, sizeof(chunk), &total_read);
    if (status != SAS_STATUS_OK)
    {
        if (strcmp(path, "/www/index.html") != 0)
        {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL); /* SAS-HTTP-FR-004 */
            return ESP_OK;
        }
        /* The dashboard itself is missing (or the LittleFS mount failed): minimal embedded
         * fallback rather than a full duplicate copy of the dashboard's static assets. */
        static const char fallback_body[] =
            "<!doctype html><title>SAS</title>SAS device: filesystem unavailable.";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, fallback_body, sizeof(fallback_body) - 1U);
        return ESP_OK;
    }

    /* Stream the file in SAS_HTTP_STATIC_READ_CHUNK-sized pieces regardless
     * of size: a short read (total_read < sizeof(chunk)) marks end of file. */
    httpd_resp_set_type(req, content_type_for_path(path));
    /* The dashboard's files change with every filesystem flash; without this a browser keeps
     * running its cached copy of the old ones. */
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    while (total_read > 0U)
    {
        if (httpd_resp_send_chunk(req, (const char *)chunk, total_read) != ESP_OK)
        {
            (void)httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
        if (total_read < sizeof(chunk))
        {
            break;
        }
        offset += total_read;
        status = sas_fs_read(path, offset, chunk, sizeof(chunk), &total_read);
        if (status != SAS_STATUS_OK)
        {
            break;
        }
    }
    (void)httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/*! \brief Register s_routes[index] with the live httpd_ssl instance. Caller: sas_http_start()'s
 * batch pass, or sas_http_register_route() when called after the server has already started. */
static void register_route_with_server(size_t index)
{
    if (s_server == NULL)
    {
        return;
    }
    httpd_uri_t u = {
        .uri = s_routes[index].uri,
        .method = s_routes[index].method,
        .handler = dispatch_route,
        .user_ctx = &s_routes[index],
    };
    if (httpd_register_uri_handler(s_server, &u) != ESP_OK)
    {
        sas_log_warn("failed to register route %s", s_routes[index].uri);
    }
}

/*! \brief Register s_ws_routes[index] with the live httpd_ssl instance. Caller: sas_http_start()'s
 * batch pass, or sas_http_register_ws_handler() when called after the server has already
 * started. */
static void register_ws_route_with_server(size_t index)
{
    if (s_server == NULL)
    {
        return;
    }
    if (httpd_register_uri_handler(s_server, &s_ws_routes[index].uri_handler) != ESP_OK)
    {
        sas_log_warn("failed to register ws route %s", s_ws_routes[index].uri);
    }
}

/*!
    \brief   Unregister the "/x" static-fallback handler, if present.
    \note    Must run before any GET route is registered on a live server:
             httpd_register_uri_handler() rejects a new URI that an
             already-registered wildcard pattern matches
             (ESP_ERR_HTTPD_HANDLER_EXISTS), and the fallback's wildcard
             matches every GET URI.
*/
static void remove_fallback(void)
{
    if ((s_server != NULL) && s_fallback_registered)
    {
        (void)httpd_unregister_uri_handler(s_server, "/*", HTTP_GET);
        s_fallback_registered = false;
    }
}

/*!
    \brief   Register (or move) the "/x" static-fallback handler so it is
             always the most-recently-registered handler.
    \note    esp-idf httpd dispatches by first-registration-match, not by
             pattern specificity: a "/x" wildcard registered before a more
             specific route (e.g. "/api/v1/wifi/scan") would shadow it
             forever. Since sas_http_register_route()/
             sas_http_register_ws_handler() can be called after
             sas_http_start() (REST API and WebSocket both do, per the
             project's fixed Development Order in main.c), this
             unregisters and re-adds the fallback on every such call so it
             always ends up last, regardless of what gets registered after
             it.
*/
static void reregister_fallback_last(void)
{
    if (s_server == NULL)
    {
        return;
    }
    remove_fallback();
    httpd_uri_t fallback = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = default_static_handler,
        .user_ctx = NULL,
    };
    if (httpd_register_uri_handler(s_server, &fallback) == ESP_OK)
    {
        s_fallback_registered = true;
    }
    else
    {
        sas_log_warn("failed to register static fallback handler");
        s_fallback_registered = false;
    }
}

/*! \brief Minimal extension-based MIME type lookup for static assets served from /www/. */
static const char *content_type_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL)
    {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".html") == 0)
    {
        return "text/html";
    }
    if (strcmp(dot, ".css") == 0)
    {
        return "text/css";
    }
    if (strcmp(dot, ".js") == 0)
    {
        return "application/javascript";
    }
    if (strcmp(dot, ".json") == 0)
    {
        return "application/json";
    }
    return "application/octet-stream";
}
