//=============================================================================
/*!
    \file    sas_rest_api.h
    \brief   Public interface for the REST API module.
*/
//=============================================================================

// Registers the REST endpoints under /api/v1 with the HTTP Server, validates
// each request and reports failures in one common error format.

#ifndef SAS_REST_API_H
#define SAS_REST_API_H

#include "esp_http_server.h" // httpd_req_t

#include "../core/sas_status.h"
#include "cJSON.h"

/*! \brief Standard error envelope reported to the HTTP client on any non-2xx
    response. */
typedef struct
{
    int http_status;        ///< HTTP status code, e.g. 400, 404, 500.
    const char *error_code; ///< Short machine-readable code, e.g. "invalid_request".
    const char *message;    ///< Human-readable detail for logs/UI.
} sas_api_error_t;

/*!
    \brief   Custom sas_status_t base for this module. Note: this module
             reports errors to HTTP clients as sas_api_error_t values, not as
             sas_status_t codes returned from every internal path; the base is
             defined here for numbering consistency with sibling modules.
*/
#define SAS_API_ERR_BASE (0x8800U)

/*!
    \brief   Maximum accepted JSON request body length in bytes, enforced by
             the shared request-body reader before cJSON_Parse() is invoked.
             The api.max_body_len configuration key can lower this limit at
             runtime but not raise it (the receive buffer is sized by this
             macro). Raw file/firmware uploads are streamed and limited
             separately (api.max_upload_len, the OTA partition size).
*/
#define SAS_API_MAX_BODY_LEN (4096U)

sas_status_t sas_rest_api_init(void);

sas_status_t sas_rest_api_start(void);

sas_status_t sas_rest_api_stop(void);

sas_status_t sas_rest_api_deinit(void);

sas_status_t sas_rest_api_send_error(httpd_req_t *req, const sas_api_error_t *err);

sas_status_t sas_rest_api_send_json(httpd_req_t *req, cJSON *body);

#endif /* SAS_REST_API_H */
