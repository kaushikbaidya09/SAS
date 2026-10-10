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

#include "../core/sas_config.h"
#include "../core/sas_logger.h"
#include "../storage/sas_file_manager.h"

static const char *TAG = "sas_http";

/* 2. Private macros */
#define SAS_HTTP_MAX_ROUTES (16U)   ///< Static route-table capacity; placeholder sizing.
#define SAS_HTTP_MAX_WS_ROUTES (2U) ///< Static WebSocket-route-table capacity; placeholder sizing.
#define SAS_HTTP_STATIC_PATH_MAX_LEN \
    (128U)                                 ///< Max length of a File-Manager path built from a request URI in
                                           ///< default_static_handler(), including the "/www" prefix and NUL.
#define SAS_HTTP_STATIC_READ_CHUNK (2048U) ///< Chunk size streaming a static file to the client.

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
        (void)httpd_ssl_stop(s_server);
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
        register_ws_route_with_server(s_ws_route_count - 1U);
        reregister_fallback_last();
    }
    return SAS_STATUS_OK;
}

httpd_handle_t sas_http_get_server_handle(void)
{
    return s_server;
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
    \note    Authentication is out of scope for the current build (not
             wired up: HTTP Server declares no dependency on it), so a
             requires_auth route responds 501 rather than either silently
             allowing access or adding an undeclared module dependency.
             Every route registered by this change sets requires_auth =
             false and never hits that path.
*/
static esp_err_t dispatch_route(httpd_req_t *req)
{
    const sas_http_route_t *route = (const sas_http_route_t *)req->user_ctx;
    if (route == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (route->requires_auth)
    {
        httpd_resp_set_status(req, "501 Not Implemented");
        (void)httpd_resp_send(req, "Authentication is not enabled on this build.",
                              HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
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

    uint8_t chunk[SAS_HTTP_STATIC_READ_CHUNK];
    size_t offset = 0U;
    size_t total_read = 0U;
    sas_status_t status = sas_fs_read(path, offset, chunk, sizeof(chunk), &total_read);
    if (status != SAS_STATUS_OK)
    {
        /* No File Manager match (missing file, or the LittleFS mount itself
         * failed): minimal embedded fallback rather than a full duplicate
         * copy of the dashboard's static assets. */
        static const char fallback_body[] =
            "<!doctype html><title>SAS</title>SAS device: filesystem unavailable.";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, fallback_body, sizeof(fallback_body) - 1U);
        return ESP_OK;
    }

    /* Stream the file in SAS_HTTP_STATIC_READ_CHUNK-sized pieces regardless
     * of size: a short read (total_read < sizeof(chunk)) marks end of file. */
    httpd_resp_set_type(req, content_type_for_path(path));
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
    if (s_fallback_registered)
    {
        (void)httpd_unregister_uri_handler(s_server, "/*", HTTP_GET);
    }
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
