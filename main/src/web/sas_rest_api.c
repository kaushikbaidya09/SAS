/*!
    \file    sas_rest_api.c
    \brief   Implementation skeleton for the REST API module.

    \description
    See sas_rest_api.h for the public contract. This file currently contains
    only the lifecycle and response-helper API stubs; behaviour is filled
    in per module during implementation, in Development Order. No endpoint
    handler functions exist yet, so sas_http_server.h is not included here;
    it will be needed once sas_rest_api_start() actually calls
    sas_http_register_route() for each REST endpoint.

    Configuration Keys (not yet consumed by this skeleton): api.max_body_len.

    \dependencies
    - HTTP Server (main/src/web/sas_http_server.h): route registration.
    - Configuration Manager, System Information, File Manager,
      Wi-Fi Manager, BLE Scanner, OTA Manager, Authentication: each owns
      the handler backing its endpoints, see sas_rest_api.h.
*/

/* 1. Includes */
#include "sas_rest_api.h"

#include <stddef.h>

/* 2. Private macros */
/* (none yet) */

/* 3. Private typedefs */
/* (none yet: internal route table / schema-check state to be added during
 * implementation) */

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started     = false;

/* 5. Forward declarations */
/* (none yet: shared request-body reader and per-endpoint schema-check
 * helpers to be added during implementation) */

/* 6. Public functions */

sas_status_t sas_rest_api_init(void)
{
    if (s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Allocate any static module state needed by the shared
     *    request-body reader.
     * 2. Do not register routes here; see sas_rest_api_start(). */
    s_initialized = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_rest_api_start(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Register every REST endpoint with the HTTP Server via
     *    sas_http_register_route(), each under base path /api/v1.
     * 2. Each handler must run the shared request-body reader (enforcing
     *    SAS_API_MAX_BODY_LEN before cJSON_Parse()) and its per-endpoint
     *    schema check before invoking the owning module's handler.
     * 3. On malformed JSON, respond HTTP 400 via
     *    sas_rest_api_send_error() and never invoke the owning module
     *    handler. */
    s_started = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_rest_api_stop(void)
{
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Unregister the routes added in sas_rest_api_start().
     * 2. Leave any module state acquired in sas_rest_api_init() untouched
     *    (stop() must not release resources). */
    s_started = false;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_rest_api_deinit(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Release any resources acquired in sas_rest_api_init().
     * 2. Reset module state to uninitialized. */
    s_initialized = false;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_rest_api_send_error(httpd_req_t *req,
                                     const sas_api_error_t *err)
{
    if ((req == NULL) || (err == NULL) || (err->error_code == NULL) ||
        (err->message == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Build the standard error envelope
     *    { "error_code": ..., "message": ... } via cJSON.
     * 2. Set the HTTP status from err->http_status.
     * 3. Send it, so every endpoint returns the same error envelope shape
     *    on failure. */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_rest_api_send_json(httpd_req_t *req, cJSON *body)
{
    if ((req == NULL) || (body == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Serialize body via cJSON_PrintUnformatted() (or equivalent).
     * 2. Set Content-Type: application/json.
     * 3. Send the response. */
    return SAS_ERR_NOT_SUPPORTED;
}

/* 7. Private functions */
/* (none yet: shared request-body reader and schema-check helpers to be
 * added during implementation) */
