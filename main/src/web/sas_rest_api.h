/*!
    \file    sas_rest_api.h
    \brief   Public interface for the REST API module.

    \description
    Registers every REST endpoint with the HTTP Server
    (main/src/web/sas_http_server.h) under the common base path /api/v1,
    validates each request body against its per-endpoint schema before
    invoking the owning module's handler, and reports every failure to
    the HTTP client through a single uniform error envelope.

    \dependencies
    - HTTP Server (main/src/web/sas_http_server.h): route registration via
      sas_http_register_route(); this module owns no server instance of
      its own.
    - Configuration Manager (main/src/core/sas_config.h): backs the
      GET/PUT /api/v1/config/{key} and POST /api/v1/config/factory-reset
      endpoints.
    - System Information (main/src/core/sas_sysinfo.h): backs the
      /api/v1/system/* endpoints.
    - File Manager (main/src/storage/sas_file_manager.h): backs the
      /api/v1/files/* endpoints.
    - Wi-Fi Manager (main/src/network/sas_wifi_manager.h): backs the
      /api/v1/wifi/* endpoints.
    - BLE Scanner (main/src/network/sas_ble_scanner.h): backs the
      /api/v1/ble/* endpoints.
    - OTA Manager (main/src/network/sas_ota_manager.h): backs the
      /api/v1/ota/* endpoints.
    - Authentication (main/src/auth/sas_auth.h): backs the
      /api/v1/auth/* endpoints and token validation for every
      requires_auth route.
*/

#pragma once

/* 1. Standard includes */
/* (none: only what ESP-IDF/cJSON pull in below) */

/* 2. ESP-IDF includes */
#include "esp_http_server.h" /* httpd_req_t */

/* 3. Project includes */
#include "../core/sas_status.h"
#include "cJSON.h"

/* 4. Public types */

/*! Standard error envelope reported to the HTTP client on any non-2xx
    response. */
typedef struct {
    int         http_status; ///< HTTP status code, e.g. 400, 404, 500.
    const char *error_code;  ///< Short machine-readable code, e.g. "invalid_request".
    const char *message;     ///< Human-readable detail for logs/UI.
} sas_api_error_t;

/* 5. Public constants */

#define SAS_API_ERR_BASE (0x8800U) ///< Custom sas_status_t base for this module. Note: this module reports errors to HTTP clients as sas_api_error_t values, not as sas_status_t codes returned from every internal path; the base is defined here for numbering consistency with sibling modules.

#define SAS_API_MAX_BODY_LEN (4096U) ///< Placeholder maximum accepted request body length in bytes, enforced by the shared request-body reader before cJSON_Parse() is invoked. The authoritative value at runtime is the api.max_body_len configuration key (Configuration Manager); this macro is a compile-time fallback.

/* 6. Public API declarations */

/*!
    \brief   Initialize the REST API module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already
             initialized.
*/
sas_status_t sas_rest_api_init(void);

/*!
    \brief   Register every REST endpoint with the HTTP Server.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_rest_api_start(void);

/*!
    \brief   Stop serving REST API requests without releasing resources
             acquired by sas_rest_api_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             started.
*/
sas_status_t sas_rest_api_stop(void);

/*!
    \brief   Release all resources acquired by sas_rest_api_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_rest_api_deinit(void);

/*!
    \brief   Write the standard error envelope to the HTTP response and
             finalize it.
    \param   req Non-NULL request being responded to.
    \param   err Non-NULL error descriptor; err->error_code and
             err->message must be non-NULL.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if req or err
             (or its members) is NULL.
*/
sas_status_t sas_rest_api_send_error(httpd_req_t *req,
                                     const sas_api_error_t *err);

/*!
    \brief   Serialize body and write it as the JSON HTTP response,
             finalizing the response.
    \param   req  Non-NULL request being responded to.
    \param   body Non-NULL cJSON object owned by the caller; not freed by
             this function.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if req or body
             is NULL.
*/
sas_status_t sas_rest_api_send_json(httpd_req_t *req, cJSON *body);
