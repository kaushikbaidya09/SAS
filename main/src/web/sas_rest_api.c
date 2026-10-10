/*!
    \file    sas_rest_api.c
    \brief   Implementation of the REST API module.

    \description
    See sas_rest_api.h for the public contract. This implementation covers
    only the read-only endpoints needed by the current dashboard feature
    (Wi-Fi scan results, BLE scan results, recent log entries) - the
    config/system/files/ota/auth endpoint groups named in sas_rest_api.h's
    \dependencies list are not registered here; that scope is deferred, not
    an oversight. Every endpoint below sets requires_auth = false:
    Authentication is not wired into this build (see sas_http_server.c).

    \dependencies
    - HTTP Server (main/src/web/sas_http_server.h): route registration.
    - Wi-Fi Manager (main/src/network/sas_wifi_manager.h): GET /api/v1/wifi/scan.
    - BLE Scanner (main/src/network/sas_ble_scanner.h): GET /api/v1/ble/devices.
    - Logger (main/src/core/sas_logger.h): GET /api/v1/logs.
*/

/* 1. Includes */
#include "sas_rest_api.h"

#include <stdio.h>

#include "sas_http_server.h"

#include "../core/sas_logger.h"
#include "../network/sas_ble_scanner.h"
#include "../network/sas_wifi_manager.h"

static const char *TAG = "sas_rest";

/* 2. Private macros */
#define SAS_API_BASE_PATH "/api/v1" ///< Common base path every endpoint is registered under.
#define SAS_API_WIFI_SCAN_MAX_RESULTS (20U) ///< Cap on one GET /wifi/scan response.

/* 3. Private typedefs */
/* (none) */

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;

/* 5. Forward declarations */
/* Stay esp_err_t: same ESP-IDF-mandated exception as sas_http_handler_fn, see sas_http_server.h. */
static esp_err_t handle_wifi_scan(httpd_req_t *req);
static esp_err_t handle_ble_devices(httpd_req_t *req);
static esp_err_t handle_logs(httpd_req_t *req);
static const char *log_level_name(sas_log_level_t level);
static void format_mac(const uint8_t mac[6], char *out, size_t out_len);

/* 6. Public functions */

sas_status_t sas_rest_api_init(void)
{
    if (s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

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

    const sas_http_route_t routes[] = {
        {
            .uri = SAS_API_BASE_PATH "/wifi/scan",
            .method = HTTP_GET,
            .handler = handle_wifi_scan,
            .requires_auth = false,
        },
        {
            .uri = SAS_API_BASE_PATH "/ble/devices",
            .method = HTTP_GET,
            .handler = handle_ble_devices,
            .requires_auth = false,
        },
        {
            .uri = SAS_API_BASE_PATH "/logs",
            .method = HTTP_GET,
            .handler = handle_logs,
            .requires_auth = false,
        },
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

    char status_line[32];
    (void)snprintf(status_line, sizeof(status_line), "%d Error", err->http_status);
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

/*! \brief GET /api/v1/wifi/scan: blocking Wi-Fi scan, JSON array of {ssid, rssi, auth_mode}. */
static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    /* static, not stack-local: keeps this handler's stack footprint small
     * under the httpd task, safe because esp_http_server dispatches one
     * handler to completion at a time on that task (same assumption
     * sas_wifi_manager.c's own s_scan_records buffer already relies on). */
    static sas_wifi_scan_result_t results[SAS_API_WIFI_SCAN_MAX_RESULTS];
    uint8_t count = 0U;
    sas_status_t status = sas_wifi_scan(results, SAS_API_WIFI_SCAN_MAX_RESULTS, &count);
    if (status != SAS_STATUS_OK) {
        const sas_api_error_t err = {
            .http_status = 500,
            .error_code = "wifi_scan_failed",
            .message = "Wi-Fi scan failed or timed out",
        };
        (void)sas_rest_api_send_error(req, &err);
        return ESP_OK;
    }

    cJSON *arr = cJSON_CreateArray();
    for (uint8_t i = 0U; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", results[i].rssi);
        cJSON_AddNumberToObject(item, "auth_mode", results[i].auth_mode);
        cJSON_AddItemToArray(arr, item);
    }

    (void)sas_rest_api_send_json(req, arr);
    cJSON_Delete(arr);
    return ESP_OK;
}

/*! \brief GET /api/v1/ble/devices: current de-duplicated advertiser table, JSON array of {mac,
 * name, rssi, last_seen_us}. */
static esp_err_t handle_ble_devices(httpd_req_t *req)
{
    static sas_ble_device_t devices[SAS_BLE_MAX_DEVICES];
    uint16_t count = 0U;
    sas_status_t status = sas_ble_get_devices(devices, SAS_BLE_MAX_DEVICES, &count);
    if (status != SAS_STATUS_OK) {
        const sas_api_error_t err = {
            .http_status = 500,
            .error_code = "ble_devices_failed",
            .message = "Failed to read the BLE device table",
        };
        (void)sas_rest_api_send_error(req, &err);
        return ESP_OK;
    }

    cJSON *arr = cJSON_CreateArray();
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

    (void)sas_rest_api_send_json(req, arr);
    cJSON_Delete(arr);
    return ESP_OK;
}

/*! \brief GET /api/v1/logs: recent Logger history, JSON array of {timestamp_us, level, tag,
 * message}. */
static esp_err_t handle_logs(httpd_req_t *req)
{
    static sas_log_entry_t entries[SAS_LOGGER_MAX_HISTORY_ENTRIES];
    uint16_t count = 0U;
    sas_status_t status = sas_log_get_entries(entries, SAS_LOGGER_MAX_HISTORY_ENTRIES, &count);
    if (status != SAS_STATUS_OK) {
        const sas_api_error_t err = {
            .http_status = 500,
            .error_code = "logs_failed",
            .message = "Failed to read log history",
        };
        (void)sas_rest_api_send_error(req, &err);
        return ESP_OK;
    }

    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0U; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "timestamp_us", (double)entries[i].timestamp_us);
        cJSON_AddStringToObject(item, "level", log_level_name(entries[i].level));
        cJSON_AddStringToObject(item, "tag", entries[i].tag);
        cJSON_AddStringToObject(item, "message", entries[i].message);
        cJSON_AddItemToArray(arr, item);
    }

    (void)sas_rest_api_send_json(req, arr);
    cJSON_Delete(arr);
    return ESP_OK;
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

/*! \brief Format a 6-byte MAC as "AA:BB:CC:DD:EE:FF" into out (out_len >= 18). */
static void format_mac(const uint8_t mac[6], char *out, size_t out_len)
{
    (void)snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                   mac[4], mac[5]);
}
