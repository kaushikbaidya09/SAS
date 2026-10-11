/*!
    \file    sas_rest_api.c
    \brief   Implementation of the REST API module.

    \description
    See sas_rest_api.h for the public contract and
    docs/REST_API_Specification.md for the endpoint list. Every handler
    answers through sas_rest_api_send_json()/sas_rest_api_send_error(), so
    the success and error envelopes are uniform across endpoints. Request
    bodies go through read_json_body(), which enforces the body-size limit
    and JSON well-formedness before any owning module is called.

    Handlers run one at a time on the HTTP Server's task, which is why the
    larger scratch buffers below are static rather than stack-local.

    \dependencies
    - HTTP Server (main/src/web/sas_http_server.h): route registration.
    - Configuration Manager, System Information, File Manager, Wi-Fi
      Manager, BLE Scanner, Logger, OTA Manager, Authentication: each backs
      its own endpoint group.
*/

/* 1. Includes */
#include "sas_rest_api.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sas_http_server.h"

#include "../auth/sas_auth.h"
#include "../core/sas_config.h"
#include "../core/sas_logger.h"
#include "../core/sas_sysinfo.h"
#include "../network/sas_ble_scanner.h"
#include "../network/sas_ota_manager.h"
#include "../network/sas_wifi_manager.h"
#include "../storage/sas_file_manager.h"

static const char *TAG = "sas_rest";

/* 2. Private macros */
#define SAS_API_BASE_PATH "/api/v1" ///< Common base path every endpoint is registered under.
#define SAS_API_WIFI_SCAN_MAX_RESULTS (20U) ///< Cap on one GET /wifi/scan response.
#define SAS_API_FILES_MAX_ENTRIES (64U)     ///< Cap on one GET /files directory listing.
#define SAS_API_FS_ROOT "/www"              ///< File Manager mount point the /files API is rooted at.
#define SAS_API_FS_PRIVATE_DIR "/config"    ///< Not reachable through /files: the config store.
#define SAS_API_PATH_MAX_LEN (120U)         ///< Longest File Manager path this module builds.
#define SAS_API_CHUNK_LEN (2048U)           ///< Chunk size for streamed request/response bodies.
#define SAS_API_DEFAULT_MAX_UPLOAD_LEN (4 * 1024 * 1024) ///< Default api.max_upload_len, bytes.
#define SAS_API_URL_MAX_LEN (256U)          ///< Longest POST /ota/url URL, including the NUL.
#define SAS_API_REBOOT_DELAY_US (750000ULL) ///< Lets the reboot response reach the client first.
#define SAS_API_WORKER_STACK_SIZE (8192U)   ///< Stack of the OTA-from-URL / Wi-Fi-connect tasks.
#define SAS_API_RECV_RETRIES (3)            ///< Socket-timeout retries while receiving a body.

/* 3. Private typedefs */
/* (none) */

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;

static char s_body[SAS_API_MAX_BODY_LEN + 1U]; ///< read_json_body() scratch buffer.
static uint8_t s_chunk[SAS_API_CHUNK_LEN];     ///< Streamed upload/download scratch buffer.
static uint32_t s_max_body_len = SAS_API_MAX_BODY_LEN;
static uint32_t s_max_upload_len = (uint32_t)SAS_API_DEFAULT_MAX_UPLOAD_LEN;

static char s_ota_url[SAS_API_URL_MAX_LEN];     ///< URL handed to ota_url_task().
static volatile bool s_ota_url_busy = false;    ///< An ota_url_task() is running.
static sas_wifi_credentials_t s_wifi_creds;     ///< Credentials handed to wifi_connect_task().
static volatile bool s_wifi_connect_busy = false; ///< A wifi_connect_task() is running.
static esp_timer_handle_t s_reboot_timer = NULL;

/* 5. Forward declarations */
/* Handlers stay esp_err_t: same ESP-IDF-mandated exception as sas_http_handler_fn, see
   sas_http_server.h. */
static esp_err_t handle_auth_login(httpd_req_t *req);
static esp_err_t handle_auth_logout(httpd_req_t *req);
static esp_err_t handle_auth_change_password(httpd_req_t *req);
static esp_err_t handle_auth_status(httpd_req_t *req);
static esp_err_t handle_system_info(httpd_req_t *req);
static esp_err_t handle_system_runtime(httpd_req_t *req);
static esp_err_t handle_system_reset_reason(httpd_req_t *req);
static esp_err_t handle_system_tasks(httpd_req_t *req);
static esp_err_t handle_system_boot_status(httpd_req_t *req);
static esp_err_t handle_system_reboot(httpd_req_t *req);
static esp_err_t handle_config_get(httpd_req_t *req);
static esp_err_t handle_config_put(httpd_req_t *req);
static esp_err_t handle_config_factory_reset(httpd_req_t *req);
static esp_err_t handle_files_list(httpd_req_t *req);
static esp_err_t handle_files_download(httpd_req_t *req);
static esp_err_t handle_files_upload(httpd_req_t *req);
static esp_err_t handle_files_delete(httpd_req_t *req);
static esp_err_t handle_files_rename(httpd_req_t *req);
static esp_err_t handle_files_usage(httpd_req_t *req);
static esp_err_t handle_wifi_status(httpd_req_t *req);
static esp_err_t handle_wifi_scan(httpd_req_t *req);
static esp_err_t handle_wifi_connect(httpd_req_t *req);
static esp_err_t handle_ble_scan_start(httpd_req_t *req);
static esp_err_t handle_ble_scan_stop(httpd_req_t *req);
static esp_err_t handle_ble_devices(httpd_req_t *req);
static esp_err_t handle_logs(httpd_req_t *req);
static esp_err_t handle_ota_upload(httpd_req_t *req);
static esp_err_t handle_ota_url(httpd_req_t *req);
static esp_err_t handle_ota_status(httpd_req_t *req);

static void load_config(void);
static void send_error(httpd_req_t *req, int http_status, const char *error_code,
                       const char *message);
static void send_status(httpd_req_t *req, const char *http_status_line, const char *status);
static void send_object(httpd_req_t *req, cJSON *body);
static cJSON *read_json_body(httpd_req_t *req);
static const char *require_string(httpd_req_t *req, const cJSON *body, const char *field,
                                  size_t max_len);
static int recv_chunk(httpd_req_t *req, size_t remaining);
static bool get_query_path(httpd_req_t *req, char *out, size_t out_len);
static bool resolve_fs_path(const char *user_path, char *out, size_t out_len);
static void url_decode(char *text);
static bool config_key_is_secret(const char *key);
static uint32_t client_id_of(httpd_req_t *req);
static void schedule_reboot(void);
static void reboot_timer_callback(void *arg);
static void ota_url_task(void *arg);
static void wifi_connect_task(void *arg);
static const char *http_reason(int http_status);
static const char *log_level_name(sas_log_level_t level);
static const char *wifi_state_name(sas_wifi_state_t state);
static const char *ota_state_name(sas_ota_state_t state);
static const char *reset_reason_name(esp_reset_reason_t reason);
static const char *task_state_name(uint8_t state);
static const char *boot_status_name(sas_boot_status_t status);
static void format_mac(const uint8_t mac[6], char *out, size_t out_len);

/* 6. Public functions */

sas_status_t sas_rest_api_init(void)
{
    if (s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    load_config();

    s_started = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_rest_api_start(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    /* requires_auth follows docs/REST_API_Specification.md's "Auth required" line per endpoint. */
    static const sas_http_route_t routes[] = {
        /* Authentication */
        {SAS_API_BASE_PATH "/auth/login", HTTP_POST, handle_auth_login, false},
        {SAS_API_BASE_PATH "/auth/logout", HTTP_POST, handle_auth_logout, true},
        {SAS_API_BASE_PATH "/auth/change-password", HTTP_POST, handle_auth_change_password, true},
        {SAS_API_BASE_PATH "/auth/status", HTTP_GET, handle_auth_status, false},
        /* System Information */
        {SAS_API_BASE_PATH "/system/info", HTTP_GET, handle_system_info, false},
        {SAS_API_BASE_PATH "/system/runtime", HTTP_GET, handle_system_runtime, false},
        {SAS_API_BASE_PATH "/system/reset-reason", HTTP_GET, handle_system_reset_reason, false},
        {SAS_API_BASE_PATH "/system/tasks", HTTP_GET, handle_system_tasks, true},
        {SAS_API_BASE_PATH "/system/boot-status", HTTP_GET, handle_system_boot_status, false},
        {SAS_API_BASE_PATH "/system/reboot", HTTP_POST, handle_system_reboot, true},
        /* Configuration Manager; factory-reset is POST, so it cannot collide with the
         * GET/PUT {key} wildcard. */
        {SAS_API_BASE_PATH "/config/factory-reset", HTTP_POST, handle_config_factory_reset, true},
        {SAS_API_BASE_PATH "/config/*", HTTP_GET, handle_config_get, true},
        {SAS_API_BASE_PATH "/config/*", HTTP_PUT, handle_config_put, true},
        /* File Manager; the specific GET paths precede GET /files (first match wins). */
        {SAS_API_BASE_PATH "/files/download", HTTP_GET, handle_files_download, true},
        {SAS_API_BASE_PATH "/files/usage", HTTP_GET, handle_files_usage, true},
        {SAS_API_BASE_PATH "/files/upload", HTTP_POST, handle_files_upload, true},
        {SAS_API_BASE_PATH "/files/rename", HTTP_POST, handle_files_rename, true},
        {SAS_API_BASE_PATH "/files", HTTP_GET, handle_files_list, true},
        {SAS_API_BASE_PATH "/files", HTTP_DELETE, handle_files_delete, true},
        /* Wi-Fi Manager */
        {SAS_API_BASE_PATH "/wifi/status", HTTP_GET, handle_wifi_status, true},
        {SAS_API_BASE_PATH "/wifi/scan", HTTP_GET, handle_wifi_scan, true},
        {SAS_API_BASE_PATH "/wifi/connect", HTTP_POST, handle_wifi_connect, true},
        /* BLE Scanner */
        {SAS_API_BASE_PATH "/ble/scan/start", HTTP_POST, handle_ble_scan_start, true},
        {SAS_API_BASE_PATH "/ble/scan/stop", HTTP_POST, handle_ble_scan_stop, true},
        {SAS_API_BASE_PATH "/ble/devices", HTTP_GET, handle_ble_devices, true},
        /* Logger */
        {SAS_API_BASE_PATH "/logs", HTTP_GET, handle_logs, true},
        /* OTA Manager */
        {SAS_API_BASE_PATH "/ota/upload", HTTP_POST, handle_ota_upload, true},
        {SAS_API_BASE_PATH "/ota/url", HTTP_POST, handle_ota_url, true},
        {SAS_API_BASE_PATH "/ota/status", HTTP_GET, handle_ota_status, true},
    };

    for (size_t i = 0U; i < (sizeof(routes) / sizeof(routes[0])); i++) {
        sas_status_t status = sas_http_register_route(&routes[i]);
        if (status != SAS_STATUS_OK) {
            sas_log_error("failed to register %s (0x%x)", routes[i].uri, (unsigned int)status);
        }
    }

    s_started = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_rest_api_stop(void)
{
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    s_started = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_rest_api_deinit(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    s_initialized = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_rest_api_send_error(httpd_req_t *req, const sas_api_error_t *err)
{
    if ((req == NULL) || (err == NULL) || (err->error_code == NULL) || (err->message == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }

    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return SAS_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "error_code", err->error_code);
    cJSON_AddStringToObject(body, "message", err->message);

    char status_line[40];
    (void)snprintf(status_line, sizeof(status_line), "%d %s", err->http_status,
                   http_reason(err->http_status));
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");

    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (text == NULL) {
        return SAS_ERR_NO_MEM;
    }
    (void)httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return SAS_STATUS_OK;
}

sas_status_t sas_rest_api_send_json(httpd_req_t *req, cJSON *body)
{
    if ((req == NULL) || (body == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }

    char *text = cJSON_PrintUnformatted(body);
    if (text == NULL) {
        return SAS_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return SAS_STATUS_OK;
}

/* 7. Private functions */

/* ---- Authentication ---- */

/*! \brief POST /api/v1/auth/login: { username, password } -> { token, expires_at, expires_in }. */
static esp_err_t handle_auth_login(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return ESP_OK;
    }

    const char *username = require_string(req, body, "username", 32U);
    const char *password =
        (username != NULL) ? require_string(req, body, "password", SAS_AUTH_MAX_PASSWORD_LEN) : NULL;
    if (password != NULL) {
        sas_auth_session_t session;
        uint32_t retry_after_ms = 0U;
        sas_status_t status =
            sas_auth_login_client(client_id_of(req), username, password, &session, &retry_after_ms);

        if (status == SAS_STATUS_OK) {
            int64_t expires_in_s = (session.expires_at_us - session.issued_at_us) / 1000000LL;
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "token", session.token);
            cJSON_AddNumberToObject(resp, "expires_at", (double)((int64_t)time(NULL) + expires_in_s));
            cJSON_AddNumberToObject(resp, "expires_in", (double)expires_in_s);
            send_object(req, resp);
            (void)memset(&session, 0, sizeof(session));
        } else if (status == SAS_AUTH_ERR_RATE_LIMITED) {
            /* static: httpd keeps the pointer until the response is sent. */
            static char retry_after[12];
            (void)snprintf(retry_after, sizeof(retry_after), "%u",
                           (unsigned int)((retry_after_ms + 999U) / 1000U));
            (void)httpd_resp_set_hdr(req, "Retry-After", retry_after);
            send_error(req, 429, "rate_limited", "Too many failed attempts; retry shortly");
        } else if (status == SAS_AUTH_ERR_INVALID_CREDENTIAL) {
            send_error(req, 401, "unauthorized", "Wrong username or password");
        } else {
            send_error(req, 500, "internal_error", "Authentication is unavailable");
        }
    }

    /* The parsed body and the receive buffer both held the plaintext password. */
    cJSON_Delete(body);
    (void)memset(s_body, 0, sizeof(s_body));
    return ESP_OK;
}

/*! \brief POST /api/v1/auth/logout: end the session of the presented token. */
static esp_err_t handle_auth_logout(httpd_req_t *req)
{
    char token[33];
    if (sas_http_get_bearer_token(req, token, sizeof(token)) == SAS_STATUS_OK) {
        (void)sas_auth_logout(token);
    }
    send_status(req, NULL, "ok");
    return ESP_OK;
}

/*! \brief POST /api/v1/auth/change-password: { current_password, new_password }. */
static esp_err_t handle_auth_change_password(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return ESP_OK;
    }

    const char *current = require_string(req, body, "current_password", SAS_AUTH_MAX_PASSWORD_LEN);
    const char *replacement =
        (current != NULL) ? require_string(req, body, "new_password", SAS_AUTH_MAX_PASSWORD_LEN)
                          : NULL;
    if (replacement != NULL) {
        sas_status_t status = sas_auth_change_credential(current, replacement);
        if (status == SAS_STATUS_OK) {
            send_status(req, NULL, "ok");
        } else if (status == SAS_AUTH_ERR_INVALID_CREDENTIAL) {
            send_error(req, 401, "unauthorized", "current_password is wrong");
        } else if (status == SAS_ERR_INVALID_ARG) {
            send_error(req, 400, "invalid_request", "new_password must be 8 to 64 characters");
        } else {
            send_error(req, 500, "internal_error", "Could not store the new credential");
        }
    }

    cJSON_Delete(body);
    (void)memset(s_body, 0, sizeof(s_body));
    return ESP_OK;
}

/*! \brief GET /api/v1/auth/status: { enabled, authenticated } - lets the dashboard decide whether
 * to show the login view. */
static esp_err_t handle_auth_status(httpd_req_t *req)
{
    bool enabled = sas_auth_is_enabled();
    bool authenticated = !enabled;
    char token[33];
    if (enabled && (sas_http_get_bearer_token(req, token, sizeof(token)) == SAS_STATUS_OK)) {
        authenticated = (sas_auth_validate_token(token) == SAS_STATUS_OK);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "enabled", enabled);
    cJSON_AddBoolToObject(resp, "authenticated", authenticated);
    send_object(req, resp);
    return ESP_OK;
}

/* ---- System Information ---- */

/*! \brief GET /api/v1/system/info: chip identity plus the running firmware version. */
static esp_err_t handle_system_info(httpd_req_t *req)
{
    sas_chip_info_t info;
    if (sas_sysinfo_get_chip_info(&info) != SAS_STATUS_OK) {
        send_error(req, 500, "internal_error", "Failed to read chip information");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "chip_model", info.chip_model);
    cJSON_AddNumberToObject(resp, "chip_revision", info.chip_revision);
    cJSON_AddNumberToObject(resp, "flash_size_bytes", (double)info.flash_size_bytes);
    cJSON_AddStringToObject(resp, "firmware_version", esp_app_get_description()->version);
    send_object(req, resp);
    return ESP_OK;
}

/*! \brief GET /api/v1/system/runtime: heap and uptime. */
static esp_err_t handle_system_runtime(httpd_req_t *req)
{
    sas_runtime_info_t info;
    if (sas_sysinfo_get_runtime_info(&info) != SAS_STATUS_OK) {
        send_error(req, 500, "internal_error", "Failed to read runtime information");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "free_heap_bytes", (double)info.free_heap_bytes);
    cJSON_AddNumberToObject(resp, "min_free_heap_bytes", (double)info.min_free_heap_bytes);
    cJSON_AddNumberToObject(resp, "uptime_seconds", (double)info.uptime_seconds);
    send_object(req, resp);
    return ESP_OK;
}

/*! \brief GET /api/v1/system/reset-reason. */
static esp_err_t handle_system_reset_reason(httpd_req_t *req)
{
    esp_reset_reason_t reason = ESP_RST_UNKNOWN;
    (void)sas_sysinfo_get_reset_reason(&reason);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "reset_reason", reset_reason_name(reason));
    send_object(req, resp);
    return ESP_OK;
}

/*! \brief GET /api/v1/system/tasks: FreeRTOS task snapshot. */
static esp_err_t handle_system_tasks(httpd_req_t *req)
{
    static sas_task_snapshot_t tasks[SAS_SYSINFO_MAX_TASKS];
    uint8_t count = 0U;
    if (sas_sysinfo_get_task_snapshot(tasks, SAS_SYSINFO_MAX_TASKS, &count) != SAS_STATUS_OK) {
        send_error(req, 500, "internal_error", "Task snapshot is unavailable");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(resp, "tasks");
    for (uint8_t i = 0U; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", tasks[i].task_name);
        cJSON_AddStringToObject(item, "state", task_state_name(tasks[i].state));
        cJSON_AddNumberToObject(item, "stack_high_water_mark",
                                (double)tasks[i].stack_high_water_mark);
        cJSON_AddItemToArray(arr, item);
    }
    send_object(req, resp);
    return ESP_OK;
}

/*! \brief GET /api/v1/system/boot-status: per-module boot outcome, in boot order. */
static esp_err_t handle_system_boot_status(httpd_req_t *req)
{
    sas_boot_record_t records[SAS_SYSINFO_MAX_BOOT_RECORDS];
    uint8_t count = 0U;
    (void)sas_sysinfo_get_boot_status(records, SAS_SYSINFO_MAX_BOOT_RECORDS, &count);

    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(resp, "modules");
    for (uint8_t i = 0U; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", records[i].module_name);
        cJSON_AddStringToObject(item, "status", boot_status_name(records[i].status));
        cJSON_AddItemToArray(arr, item);
    }
    send_object(req, resp);
    return ESP_OK;
}

/*! \brief POST /api/v1/system/reboot: restart the device shortly after replying (used by the OTA
 * view once an update is ready_to_reboot). */
static esp_err_t handle_system_reboot(httpd_req_t *req)
{
    sas_log_warn("reboot requested over the REST API");
    send_status(req, NULL, "rebooting");
    schedule_reboot();
    return ESP_OK;
}

/* ---- Configuration Manager ---- */

/*! \brief GET /api/v1/config/{key}: { key, type, value }. */
static esp_err_t handle_config_get(httpd_req_t *req)
{
    const char *key = &req->uri[sizeof(SAS_API_BASE_PATH "/config/") - 1U];
    if (config_key_is_secret(key)) {
        send_error(req, 403, "forbidden", "This key is not readable over the API");
        return ESP_OK;
    }
    if (!sas_config_has_key(key)) {
        send_error(req, 404, "not_found", "No value is stored for this key");
        return ESP_OK;
    }

    sas_config_value_t val;
    sas_config_value_t def;
    def.type = SAS_CFG_TYPE_INT;
    def.value.int_val = 0;
    (void)sas_config_get(key, &val, &def);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "key", key);
    switch (val.type) {
    case SAS_CFG_TYPE_STRING:
        cJSON_AddStringToObject(resp, "type", "string");
        cJSON_AddStringToObject(resp, "value", val.value.str_val);
        break;
    case SAS_CFG_TYPE_BOOL:
        cJSON_AddStringToObject(resp, "type", "bool");
        cJSON_AddBoolToObject(resp, "value", val.value.bool_val);
        break;
    case SAS_CFG_TYPE_INT:
    default:
        cJSON_AddStringToObject(resp, "type", "int");
        cJSON_AddNumberToObject(resp, "value", val.value.int_val);
        break;
    }
    send_object(req, resp);
    return ESP_OK;
}

/*! \brief PUT /api/v1/config/{key}: { type, value }. */
static esp_err_t handle_config_put(httpd_req_t *req)
{
    /* Copy the key first: read_json_body() may reuse the request's buffers. */
    char key[SAS_CFG_MAX_KEY_LEN];
    const char *uri_key = &req->uri[sizeof(SAS_API_BASE_PATH "/config/") - 1U];
    if ((uri_key[0] == '\0') || (strlen(uri_key) >= sizeof(key))) {
        send_error(req, 400, "invalid_request", "Invalid configuration key");
        return ESP_FAIL; /* body left unread: close the connection */
    }
    (void)strcpy(key, uri_key);

    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return ESP_OK;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(body, "type");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(body, "value");
    sas_config_value_t val;
    (void)memset(&val, 0, sizeof(val));
    bool valid = false;

    if (cJSON_IsString(type) && (value != NULL)) {
        if ((strcmp(type->valuestring, "string") == 0) && cJSON_IsString(value) &&
            (strlen(value->valuestring) < SAS_CFG_MAX_STRING_LEN)) {
            val.type = SAS_CFG_TYPE_STRING;
            (void)strcpy(val.value.str_val, value->valuestring);
            valid = true;
        } else if ((strcmp(type->valuestring, "int") == 0) && cJSON_IsNumber(value)) {
            val.type = SAS_CFG_TYPE_INT;
            val.value.int_val = (int32_t)value->valuedouble;
            valid = true;
        } else if ((strcmp(type->valuestring, "bool") == 0) && cJSON_IsBool(value)) {
            val.type = SAS_CFG_TYPE_BOOL;
            val.value.bool_val = cJSON_IsTrue(value);
            valid = true;
        } else {
            /* type/value mismatch */
        }
    }

    if (!valid) {
        send_error(req, 400, "invalid_request",
                   "Expected { \"type\": \"string|int|bool\", \"value\": <matching type> }");
    } else if (config_key_is_secret(key) && (strncmp(key, "auth.", 5U) == 0)) {
        send_error(req, 403, "forbidden", "Change the credential with /auth/change-password");
    } else {
        sas_status_t status = sas_config_set(key, &val);
        if (status == SAS_STATUS_OK) {
            send_status(req, NULL, "ok");
        } else if (status == SAS_CFG_ERR_STORE_FULL) {
            send_error(req, 409, "conflict", "The configuration store is full");
        } else {
            send_error(req, 400, "invalid_request", "Value rejected for this key");
        }
    }

    cJSON_Delete(body);
    return ESP_OK;
}

/*! \brief POST /api/v1/config/factory-reset. The stored credential is cleared too; the default one
 * is seeded again at the next boot. */
static esp_err_t handle_config_factory_reset(httpd_req_t *req)
{
    if (sas_config_factory_reset() != SAS_STATUS_OK) {
        send_error(req, 500, "internal_error", "Factory reset failed");
        return ESP_OK;
    }
    send_status(req, NULL, "ok");
    return ESP_OK;
}

/* ---- File Manager ---- */

/*! \brief GET /api/v1/files?path={dir_path}: directory listing. */
static esp_err_t handle_files_list(httpd_req_t *req)
{
    char path[SAS_API_PATH_MAX_LEN];
    if (!get_query_path(req, path, sizeof(path))) {
        return ESP_OK;
    }

    static sas_file_entry_t entries[SAS_API_FILES_MAX_ENTRIES];
    uint16_t count = 0U;
    if (sas_fs_list(path, entries, SAS_API_FILES_MAX_ENTRIES, &count) != SAS_STATUS_OK) {
        send_error(req, 404, "not_found", "No such directory");
        return ESP_OK;
    }

    bool is_root = (strcmp(path, SAS_API_FS_ROOT) == 0);
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(resp, "entries");
    for (uint16_t i = 0U; i < count; i++) {
        /* Hide what the API refuses to serve anyway: dotfiles and the config store. */
        if ((entries[i].name[0] == '.') ||
            (is_root && (strcmp(entries[i].name, &SAS_API_FS_PRIVATE_DIR[1]) == 0))) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddNumberToObject(item, "size_bytes", (double)entries[i].size_bytes);
        cJSON_AddBoolToObject(item, "is_dir", entries[i].is_dir);
        cJSON_AddItemToArray(arr, item);
    }
    send_object(req, resp);
    return ESP_OK;
}

/*! \brief GET /api/v1/files/download?path={file_path}: raw file bytes. */
static esp_err_t handle_files_download(httpd_req_t *req)
{
    char path[SAS_API_PATH_MAX_LEN];
    if (!get_query_path(req, path, sizeof(path))) {
        return ESP_OK;
    }

    size_t offset = 0U;
    size_t read_len = 0U;
    if (sas_fs_read(path, offset, s_chunk, sizeof(s_chunk), &read_len) != SAS_STATUS_OK) {
        send_error(req, 404, "not_found", "No such file");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    while (read_len > 0U) {
        if (httpd_resp_send_chunk(req, (const char *)s_chunk, (ssize_t)read_len) != ESP_OK) {
            return ESP_FAIL;
        }
        if (read_len < sizeof(s_chunk)) {
            break; /* short read: end of file */
        }
        offset += read_len;
        if (sas_fs_read(path, offset, s_chunk, sizeof(s_chunk), &read_len) != SAS_STATUS_OK) {
            break;
        }
    }
    (void)httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/*! \brief POST /api/v1/files/upload?path={file_path}: raw file bytes, streamed into an atomic
 * File Manager write. */
static esp_err_t handle_files_upload(httpd_req_t *req)
{
    char path[SAS_API_PATH_MAX_LEN];
    if (!get_query_path(req, path, sizeof(path))) {
        return ESP_FAIL; /* body left unread: close the connection */
    }
    if (strcmp(path, SAS_API_FS_ROOT) == 0) {
        send_error(req, 400, "invalid_request", "path must name a file");
        return ESP_FAIL;
    }
    if (req->content_len > s_max_upload_len) {
        send_error(req, 413, "payload_too_large", "File exceeds api.max_upload_len");
        return ESP_FAIL;
    }

    sas_status_t status = sas_fs_write_begin(path, req->content_len);
    if (status != SAS_STATUS_OK) {
        if (status == SAS_FS_ERR_NO_SPACE) {
            send_error(req, 400, "invalid_request", "Insufficient free space");
        } else {
            send_error(req, 409, "conflict", "Another upload is in progress");
        }
        return ESP_FAIL;
    }

    size_t remaining = req->content_len;
    while (remaining > 0U) {
        int received = recv_chunk(req, remaining);
        if (received <= 0) {
            (void)sas_fs_write_abort();
            return ESP_FAIL; /* client went away mid-upload */
        }
        if (sas_fs_write_append(s_chunk, (size_t)received) != SAS_STATUS_OK) {
            /* File Manager already discarded the partial file. */
            send_error(req, 400, "invalid_request", "Insufficient free space");
            return ESP_FAIL;
        }
        remaining -= (size_t)received;
    }

    if (sas_fs_write_commit() != SAS_STATUS_OK) {
        send_error(req, 500, "internal_error", "Could not store the file");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddNumberToObject(resp, "size_bytes", (double)req->content_len);
    send_object(req, resp);
    return ESP_OK;
}

/*! \brief DELETE /api/v1/files?path={file_path}. */
static esp_err_t handle_files_delete(httpd_req_t *req)
{
    char path[SAS_API_PATH_MAX_LEN];
    if (!get_query_path(req, path, sizeof(path))) {
        return ESP_OK;
    }

    if (sas_fs_delete(path) != SAS_STATUS_OK) {
        send_error(req, 404, "not_found", "No such file");
        return ESP_OK;
    }
    send_status(req, NULL, "ok");
    return ESP_OK;
}

/*! \brief POST /api/v1/files/rename: { old_path, new_path }. */
static esp_err_t handle_files_rename(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return ESP_OK;
    }

    const char *old_user = require_string(req, body, "old_path", SAS_API_PATH_MAX_LEN);
    const char *new_user =
        (old_user != NULL) ? require_string(req, body, "new_path", SAS_API_PATH_MAX_LEN) : NULL;
    if (new_user != NULL) {
        char old_path[SAS_API_PATH_MAX_LEN];
        char new_path[SAS_API_PATH_MAX_LEN];
        if (!resolve_fs_path(old_user, old_path, sizeof(old_path)) ||
            !resolve_fs_path(new_user, new_path, sizeof(new_path))) {
            send_error(req, 400, "invalid_request", "Invalid path");
        } else {
            sas_status_t status = sas_fs_rename(old_path, new_path);
            if (status == SAS_STATUS_OK) {
                send_status(req, NULL, "ok");
            } else if (status == SAS_FS_ERR_ALREADY_EXISTS) {
                send_error(req, 409, "conflict", "new_path already exists");
            } else {
                send_error(req, 404, "not_found", "old_path does not exist");
            }
        }
    }

    cJSON_Delete(body);
    return ESP_OK;
}

/*! \brief GET /api/v1/files/usage: { total_bytes, used_bytes, free_bytes }. */
static esp_err_t handle_files_usage(httpd_req_t *req)
{
    sas_fs_usage_t usage;
    if (sas_fs_get_usage(&usage) != SAS_STATUS_OK) {
        send_error(req, 500, "internal_error", "Filesystem usage is unavailable");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "total_bytes", (double)usage.total_bytes);
    cJSON_AddNumberToObject(resp, "used_bytes", (double)usage.used_bytes);
    cJSON_AddNumberToObject(resp, "free_bytes", (double)usage.free_bytes);
    send_object(req, resp);
    return ESP_OK;
}

/* ---- Wi-Fi Manager ---- */

/*! \brief GET /api/v1/wifi/status: { state, ssid, rssi }. */
static esp_err_t handle_wifi_status(httpd_req_t *req)
{
    sas_wifi_status_t wifi;
    (void)memset(&wifi, 0, sizeof(wifi));
    (void)sas_wifi_get_status(&wifi);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "state", wifi_state_name(wifi.state));
    cJSON_AddStringToObject(resp, "ssid", wifi.ssid);
    cJSON_AddNumberToObject(resp, "rssi", wifi.rssi);
    send_object(req, resp);
    return ESP_OK;
}

/*! \brief GET /api/v1/wifi/scan: blocking Wi-Fi scan, { networks: [ { ssid, rssi, auth_mode } ] }. */
static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    static sas_wifi_scan_result_t results[SAS_API_WIFI_SCAN_MAX_RESULTS];
    uint8_t count = 0U;
    if (sas_wifi_scan(results, SAS_API_WIFI_SCAN_MAX_RESULTS, &count) != SAS_STATUS_OK) {
        send_error(req, 500, "internal_error", "Wi-Fi scan failed or timed out");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(resp, "networks");
    for (uint8_t i = 0U; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", results[i].rssi);
        cJSON_AddNumberToObject(item, "auth_mode", results[i].auth_mode);
        cJSON_AddItemToArray(arr, item);
    }
    send_object(req, resp);
    return ESP_OK;
}

/*! \brief POST /api/v1/wifi/connect: { ssid, password }. Answers at once; the connection attempt
 * runs in its own task and its outcome arrives as "wifi_state" WebSocket events. */
static esp_err_t handle_wifi_connect(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return ESP_OK;
    }

    const char *ssid = require_string(req, body, "ssid", sizeof(s_wifi_creds.ssid) - 1U);
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(body, "password");
    if (ssid != NULL) {
        const char *pass = cJSON_IsString(password) ? password->valuestring : "";
        size_t pass_len = strlen(pass);

        if (ssid[0] == '\0') {
            send_error(req, 400, "invalid_request", "ssid must not be empty");
        } else if ((pass_len != 0U) && ((pass_len < 8U) || (pass_len > 64U))) {
            send_error(req, 400, "invalid_request", "password must be empty or 8 to 64 characters");
        } else if (s_wifi_connect_busy) {
            send_error(req, 409, "conflict", "A connection attempt is already in progress");
        } else {
            (void)memset(&s_wifi_creds, 0, sizeof(s_wifi_creds));
            (void)strcpy(s_wifi_creds.ssid, ssid);
            (void)strcpy(s_wifi_creds.password, pass);

            s_wifi_connect_busy = true;
            if (xTaskCreate(wifi_connect_task, "sas_wifi_conn", SAS_API_WORKER_STACK_SIZE, NULL,
                            tskIDLE_PRIORITY + 3U, NULL) != pdPASS) {
                s_wifi_connect_busy = false;
                send_error(req, 500, "internal_error", "Could not start the connection attempt");
            } else {
                send_status(req, NULL, "ok");
            }
        }
    }

    cJSON_Delete(body);
    (void)memset(s_body, 0, sizeof(s_body)); /* held the Wi-Fi passphrase */
    return ESP_OK;
}

/* ---- BLE Scanner ---- */

/*! \brief POST /api/v1/ble/scan/start. */
static esp_err_t handle_ble_scan_start(httpd_req_t *req)
{
    sas_status_t status = sas_ble_scan_start();
    if ((status == SAS_STATUS_OK) || (status == SAS_BLE_ERR_ALREADY_SCANNING)) {
        send_status(req, NULL, "ok"); /* already scanning: the requested state holds */
    } else {
        send_error(req, 500, "internal_error", "BLE scan could not be started");
    }
    return ESP_OK;
}

/*! \brief POST /api/v1/ble/scan/stop. */
static esp_err_t handle_ble_scan_stop(httpd_req_t *req)
{
    sas_status_t status = sas_ble_scan_stop();
    if ((status == SAS_STATUS_OK) || (status == SAS_BLE_ERR_NOT_SCANNING)) {
        send_status(req, NULL, "ok"); /* not scanning: the requested state holds */
    } else {
        send_error(req, 500, "internal_error", "BLE scan could not be stopped");
    }
    return ESP_OK;
}

/*! \brief GET /api/v1/ble/devices: { devices: [ { mac, name, rssi, last_seen_us } ] }. The device
 * has no wall clock, so last_seen_us is microseconds since boot rather than an epoch time. */
static esp_err_t handle_ble_devices(httpd_req_t *req)
{
    static sas_ble_device_t devices[SAS_BLE_MAX_DEVICES];
    uint16_t count = 0U;
    if (sas_ble_get_devices(devices, SAS_BLE_MAX_DEVICES, &count) != SAS_STATUS_OK) {
        send_error(req, 500, "internal_error", "Failed to read the BLE device table");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(resp, "devices");
    for (uint16_t i = 0U; i < count; i++) {
        char mac_str[18];
        format_mac(devices[i].mac, mac_str, sizeof(mac_str));

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", mac_str);
        cJSON_AddStringToObject(item, "name", devices[i].name);
        cJSON_AddNumberToObject(item, "rssi", devices[i].rssi);
        cJSON_AddNumberToObject(item, "last_seen_us", (double)devices[i].last_seen_us);
        cJSON_AddItemToArray(arr, item);
    }
    send_object(req, resp);
    return ESP_OK;
}

/* ---- Logger ---- */

/*! \brief GET /api/v1/logs: { entries: [ { timestamp_us, level, tag, message } ] }. */
static esp_err_t handle_logs(httpd_req_t *req)
{
    static sas_log_entry_t entries[SAS_LOGGER_MAX_HISTORY_ENTRIES];
    uint16_t count = 0U;
    if (sas_log_get_entries(entries, SAS_LOGGER_MAX_HISTORY_ENTRIES, &count) != SAS_STATUS_OK) {
        send_error(req, 500, "internal_error", "Failed to read log history");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(resp, "entries");
    for (uint16_t i = 0U; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "timestamp_us", (double)entries[i].timestamp_us);
        cJSON_AddStringToObject(item, "level", log_level_name(entries[i].level));
        cJSON_AddStringToObject(item, "tag", entries[i].tag);
        cJSON_AddStringToObject(item, "message", entries[i].message);
        cJSON_AddItemToArray(arr, item);
    }
    send_object(req, resp);
    return ESP_OK;
}

/* ---- OTA Manager ---- */

/*! \brief POST /api/v1/ota/upload: raw firmware image, streamed into the inactive partition and
 * verified before this handler answers 202 with the resulting state. */
static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    if (req->content_len == 0U) {
        send_error(req, 400, "invalid_request", "Empty firmware image");
        return ESP_OK;
    }

    sas_status_t status = sas_ota_begin_from_upload(req->content_len);
    if (status != SAS_STATUS_OK) {
        if (status == SAS_OTA_ERR_IMAGE_TOO_LARGE) {
            send_error(req, 400, "invalid_request", "Image is larger than the OTA partition");
        } else if (status == SAS_ERR_INVALID_STATE) {
            send_error(req, 409, "conflict", "Another update is in progress or awaiting reboot");
        } else {
            send_error(req, 500, "internal_error", "OTA is unavailable on this device");
        }
        return ESP_FAIL; /* body left unread: close the connection */
    }

    size_t remaining = req->content_len;
    while (remaining > 0U) {
        int received = recv_chunk(req, remaining);
        if (received <= 0) {
            (void)sas_ota_abort();
            return ESP_FAIL; /* client went away mid-upload */
        }
        if (sas_ota_write_chunk(s_chunk, (size_t)received) != SAS_STATUS_OK) {
            /* OTA Manager already moved to FAILED and discarded the partial image. */
            send_error(req, 400, "invalid_request", "Not a valid firmware image");
            return ESP_FAIL;
        }
        remaining -= (size_t)received;
    }

    if (sas_ota_finish() != SAS_STATUS_OK) {
        send_error(req, 400, "invalid_request", "Firmware image failed verification");
        return ESP_OK;
    }

    send_status(req, "202 Accepted", ota_state_name(sas_ota_get_state()));
    return ESP_OK;
}

/*! \brief POST /api/v1/ota/url: { url }. Answers 202 at once; the download runs in its own task
 * and reports through "ota_progress" WebSocket events and GET /ota/status. */
static esp_err_t handle_ota_url(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return ESP_OK;
    }

    const char *url = require_string(req, body, "url", SAS_API_URL_MAX_LEN - 1U);
    if (url != NULL) {
        sas_ota_state_t state = sas_ota_get_state();
        if ((strncmp(url, "http://", 7U) != 0) && (strncmp(url, "https://", 8U) != 0)) {
            send_error(req, 400, "invalid_request", "url must start with http:// or https://");
        } else if (s_ota_url_busy ||
                   ((state != SAS_OTA_STATE_IDLE) && (state != SAS_OTA_STATE_FAILED))) {
            send_error(req, 409, "conflict", "Another update is in progress or awaiting reboot");
        } else {
            (void)strcpy(s_ota_url, url);
            s_ota_url_busy = true;
            if (xTaskCreate(ota_url_task, "sas_ota_url", SAS_API_WORKER_STACK_SIZE, NULL,
                            tskIDLE_PRIORITY + 3U, NULL) != pdPASS) {
                s_ota_url_busy = false;
                send_error(req, 500, "internal_error", "Could not start the download");
            } else {
                send_status(req, "202 Accepted", "receiving");
            }
        }
    }

    cJSON_Delete(body);
    return ESP_OK;
}

/*! \brief GET /api/v1/ota/status: { state, percent_complete }. */
static esp_err_t handle_ota_status(httpd_req_t *req)
{
    sas_ota_progress_t progress;
    (void)sas_ota_get_progress(&progress);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "state", ota_state_name(progress.state));
    cJSON_AddNumberToObject(resp, "percent_complete", progress.percent_complete);
    send_object(req, resp);
    return ESP_OK;
}

/* ---- shared helpers ---- */

/*! \brief Read api.max_body_len / api.max_upload_len. Caller: sas_rest_api_init(). */
static void load_config(void)
{
    sas_config_value_t val;
    sas_config_value_t def;

    def.type = SAS_CFG_TYPE_INT;
    def.value.int_val = (int32_t)SAS_API_MAX_BODY_LEN;
    (void)sas_config_get("api.max_body_len", &val, &def);
    /* s_body is sized for SAS_API_MAX_BODY_LEN: the key can lower the limit, not raise it. */
    if ((val.type == SAS_CFG_TYPE_INT) && (val.value.int_val > 0) &&
        (val.value.int_val <= (int32_t)SAS_API_MAX_BODY_LEN)) {
        s_max_body_len = (uint32_t)val.value.int_val;
    }

    def.value.int_val = SAS_API_DEFAULT_MAX_UPLOAD_LEN;
    (void)sas_config_get("api.max_upload_len", &val, &def);
    if ((val.type == SAS_CFG_TYPE_INT) && (val.value.int_val > 0)) {
        s_max_upload_len = (uint32_t)val.value.int_val;
    }
}

/*! \brief Send the standard error envelope. */
static void send_error(httpd_req_t *req, int http_status, const char *error_code,
                       const char *message)
{
    const sas_api_error_t err = {
        .http_status = http_status,
        .error_code = error_code,
        .message = message,
    };
    (void)sas_rest_api_send_error(req, &err);
}

/*! \brief Send { "status": status }; http_status_line NULL means 200 OK. */
static void send_status(httpd_req_t *req, const char *http_status_line, const char *status)
{
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return;
    }
    cJSON_AddStringToObject(resp, "status", status);
    if (http_status_line != NULL) {
        httpd_resp_set_status(req, http_status_line);
    }
    (void)sas_rest_api_send_json(req, resp);
    cJSON_Delete(resp);
}

/*! \brief Send body as the 200 response and delete it. */
static void send_object(httpd_req_t *req, cJSON *body)
{
    if (body == NULL) {
        send_error(req, 500, "internal_error", "Out of memory");
        return;
    }
    if (sas_rest_api_send_json(req, body) != SAS_STATUS_OK) {
        send_error(req, 500, "internal_error", "Out of memory");
    }
    cJSON_Delete(body);
}

/*!
    \brief   Shared request-body reader: enforce the body-size limit
             (SAS-API-NFR-001), receive the body, and parse it as a JSON
             object.
    \return  The parsed object (caller deletes it), or NULL after an error
             response has already been sent - the caller then returns
             without invoking any owning module (SAS-API-FR-004).
*/
static cJSON *read_json_body(httpd_req_t *req)
{
    if (req->content_len > s_max_body_len) {
        send_error(req, 413, "payload_too_large", "Request body exceeds api.max_body_len");
        return NULL;
    }

    size_t received_total = 0U;
    while (received_total < req->content_len) {
        int received = httpd_req_recv(req, &s_body[received_total], req->content_len - received_total);
        if (received <= 0) {
            send_error(req, 400, "invalid_request", "Request body was not received");
            return NULL;
        }
        received_total += (size_t)received;
    }
    s_body[received_total] = '\0';

    cJSON *body = cJSON_Parse(s_body);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        send_error(req, 400, "invalid_request", "Request body is not a JSON object");
        return NULL;
    }
    return body;
}

/*! \brief Schema check for one required string field of at most max_len characters. Returns its
 * value, or NULL after sending a 400 that names the field. */
static const char *require_string(httpd_req_t *req, const cJSON *body, const char *field,
                                  size_t max_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(body, field);
    if (!cJSON_IsString(item) || (item->valuestring == NULL) ||
        (strlen(item->valuestring) > max_len)) {
        char message[64];
        (void)snprintf(message, sizeof(message), "Missing or invalid field: %s", field);
        send_error(req, 400, "invalid_request", message);
        return NULL;
    }
    return item->valuestring;
}

/*! \brief Receive up to one s_chunk of a streamed body, retrying socket timeouts. Returns the
 * byte count, or <= 0 on failure. */
static int recv_chunk(httpd_req_t *req, size_t remaining)
{
    size_t want = (remaining < sizeof(s_chunk)) ? remaining : sizeof(s_chunk);
    int received = HTTPD_SOCK_ERR_TIMEOUT;
    for (int attempt = 0; (attempt < SAS_API_RECV_RETRIES) && (received == HTTPD_SOCK_ERR_TIMEOUT);
         attempt++) {
        received = httpd_req_recv(req, (char *)s_chunk, want);
    }
    return received;
}

/*! \brief Resolve the request's ?path= query parameter to a File Manager path. Returns false
 * after sending a 400 if it is missing or not allowed. */
static bool get_query_path(httpd_req_t *req, char *out, size_t out_len)
{
    char query[160];
    char user_path[SAS_API_PATH_MAX_LEN] = "/";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "path", user_path, sizeof(user_path)) != ESP_OK) {
            (void)strcpy(user_path, "/");
        }
    }
    url_decode(user_path);

    if (!resolve_fs_path(user_path, out, out_len)) {
        send_error(req, 400, "invalid_request", "Invalid path");
        return false;
    }
    return true;
}

/*! \brief Map a client-supplied path ("/logs/a.txt", "logs/a.txt" or "/") onto the File Manager
 * mount point. Rejects anything that could leave the root or reach the config store: "..",
 * dotfiles, and SAS_API_FS_PRIVATE_DIR. */
static bool resolve_fs_path(const char *user_path, char *out, size_t out_len)
{
    while (*user_path == '/') {
        user_path++;
    }

    size_t len = strlen(user_path);
    while ((len > 0U) && (user_path[len - 1U] == '/')) {
        len--; /* a trailing slash names the same directory */
    }
    if ((sizeof(SAS_API_FS_ROOT) + len + 1U) > out_len) {
        return false;
    }

    /* Every component must be non-empty and must not start with '.'. */
    bool at_component_start = true;
    for (size_t i = 0U; i < len; i++) {
        char c = user_path[i];
        if (at_component_start && ((c == '.') || (c == '/'))) {
            return false;
        }
        if ((c == '\\') || ((unsigned char)c < 0x20U)) {
            return false;
        }
        at_component_start = (c == '/');
    }

    if (len == 0U) {
        (void)snprintf(out, out_len, "%s", SAS_API_FS_ROOT);
    } else {
        (void)snprintf(out, out_len, "%s/%.*s", SAS_API_FS_ROOT, (int)len, user_path);
    }

    const char *relative = &out[sizeof(SAS_API_FS_ROOT) - 1U];
    size_t private_len = sizeof(SAS_API_FS_PRIVATE_DIR) - 1U;
    if ((strncmp(relative, SAS_API_FS_PRIVATE_DIR, private_len) == 0) &&
        ((relative[private_len] == '\0') || (relative[private_len] == '/'))) {
        return false;
    }
    return true;
}

/*! \brief Decode %XX escapes and '+' in place. */
static void url_decode(char *text)
{
    char *out = text;
    for (const char *in = text; *in != '\0'; in++) {
        if ((*in == '%') && (in[1] != '\0') && (in[2] != '\0')) {
            char hex[3] = {in[1], in[2], '\0'};
            unsigned int value = 0U;
            if (sscanf(hex, "%2x", &value) == 1) {
                *out = (char)value;
                out++;
                in += 2;
                continue;
            }
        }
        *out = (*in == '+') ? ' ' : *in;
        out++;
    }
    *out = '\0';
}

/*! \brief True for keys whose values must never be returned by GET /config/{key}. */
static bool config_key_is_secret(const char *key)
{
    static const char *const secret_keys[] = {
        "auth.password_hash",
        "auth.password_salt",
        "auth.sw_key",
        "wifi.sta.password",
    };
    for (size_t i = 0U; i < (sizeof(secret_keys) / sizeof(secret_keys[0])); i++) {
        if (strcmp(key, secret_keys[i]) == 0) {
            return true;
        }
    }
    return false;
}

/*! \brief Identity of the request's remote peer for failed-login throttling: its IPv4 address
 * (also when presented as an IPv4-mapped IPv6 address). */
static uint32_t client_id_of(httpd_req_t *req)
{
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    uint32_t id = 0U;

    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &addr_len) == 0) {
        if (addr.ss_family == AF_INET) {
            id = ((const struct sockaddr_in *)&addr)->sin_addr.s_addr;
        } else if (addr.ss_family == AF_INET6) {
            const struct sockaddr_in6 *addr6 = (const struct sockaddr_in6 *)&addr;
            (void)memcpy(&id, &addr6->sin6_addr.s6_addr[12], sizeof(id));
        } else {
            /* unknown family: all such peers share one throttle bucket */
        }
    }
    return id;
}

/*! \brief Arm the one-shot timer that restarts the device. */
static void schedule_reboot(void)
{
    if (s_reboot_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = reboot_timer_callback,
            .name = "sas_api_reboot",
        };
        if (esp_timer_create(&timer_args, &s_reboot_timer) != ESP_OK) {
            s_reboot_timer = NULL;
            return;
        }
    }
    (void)esp_timer_start_once(s_reboot_timer, SAS_API_REBOOT_DELAY_US);
}

/*! \brief One-shot timer armed by schedule_reboot(). */
static void reboot_timer_callback(void *arg)
{
    (void)arg;
    esp_restart();
}

/*! \brief Worker for POST /ota/url: sas_ota_begin_from_url() blocks for the whole download, so it
 * cannot run on the HTTP Server's task. */
static void ota_url_task(void *arg)
{
    (void)arg;

    sas_status_t status = sas_ota_begin_from_url(s_ota_url);
    if (status != SAS_STATUS_OK) {
        sas_log_error("OTA from URL failed (0x%x)", (unsigned int)status);
    }

    s_ota_url_busy = false;
    vTaskDelete(NULL);
}

/*! \brief Worker for POST /wifi/connect: sas_wifi_connect() blocks until the link is up or
 * wifi.connect.timeout_ms elapses. */
static void wifi_connect_task(void *arg)
{
    (void)arg;

    (void)sas_wifi_connect(&s_wifi_creds);
    (void)memset(&s_wifi_creds, 0, sizeof(s_wifi_creds)); /* held the passphrase */

    s_wifi_connect_busy = false;
    vTaskDelete(NULL);
}

/*! \brief Reason phrase for the HTTP status codes this module sends. */
static const char *http_reason(int http_status)
{
    switch (http_status) {
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    default:
        return "Error";
    }
}

/*! \brief Map sas_log_level_t to its JSON field string. */
static const char *log_level_name(sas_log_level_t level)
{
    switch (level) {
    case SAS_LOG_LEVEL_ERROR:
        return "ERROR";
    case SAS_LOG_LEVEL_WARN:
        return "WARN";
    case SAS_LOG_LEVEL_INFO:
        return "INFO";
    case SAS_LOG_LEVEL_DEBUG:
        return "DEBUG";
    case SAS_LOG_LEVEL_VERBOSE:
    default:
        return "VERBOSE";
    }
}

/*! \brief Map sas_wifi_state_t to its JSON field string. */
static const char *wifi_state_name(sas_wifi_state_t state)
{
    switch (state) {
    case SAS_WIFI_STATE_IDLE:
        return "idle";
    case SAS_WIFI_STATE_CONNECTING:
        return "connecting";
    case SAS_WIFI_STATE_CONNECTED:
        return "connected";
    case SAS_WIFI_STATE_RECONNECTING:
        return "reconnecting";
    case SAS_WIFI_STATE_AP_FALLBACK:
        return "ap_fallback";
    default:
        return "unknown";
    }
}

/*! \brief Map sas_ota_state_t to its JSON field string. */
static const char *ota_state_name(sas_ota_state_t state)
{
    switch (state) {
    case SAS_OTA_STATE_IDLE:
        return "idle";
    case SAS_OTA_STATE_RECEIVING:
        return "receiving";
    case SAS_OTA_STATE_VERIFYING:
        return "verifying";
    case SAS_OTA_STATE_READY_TO_REBOOT:
        return "ready_to_reboot";
    case SAS_OTA_STATE_FAILED:
    default:
        return "failed";
    }
}

/*! \brief Map esp_reset_reason_t to its JSON field string. */
static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_EXT:
        return "external_pin";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep_sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    default:
        return "unknown";
    }
}

/*! \brief Map an eTaskState value to its JSON field string. */
static const char *task_state_name(uint8_t state)
{
    switch ((eTaskState)state) {
    case eRunning:
        return "running";
    case eReady:
        return "ready";
    case eBlocked:
        return "blocked";
    case eSuspended:
        return "suspended";
    case eDeleted:
        return "deleted";
    default:
        return "unknown";
    }
}

/*! \brief Map sas_boot_status_t to its JSON field string. */
static const char *boot_status_name(sas_boot_status_t status)
{
    switch (status) {
    case SAS_BOOT_STATUS_OK:
        return "ok";
    case SAS_BOOT_STATUS_SKIPPED_DEPENDENCY:
        return "skipped_dependency";
    case SAS_BOOT_STATUS_FAILED:
    default:
        return "failed";
    }
}

/*! \brief Format a 6-byte MAC as "AA:BB:CC:DD:EE:FF" into out (out_len >= 18). */
static void format_mac(const uint8_t mac[6], char *out, size_t out_len)
{
    (void)snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                   mac[4], mac[5]);
}
