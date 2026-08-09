/*!
    \file    sas_status.h
    \brief   Project-wide status/error code type.

    \description
    Every sas_<module>_* public API function returns sas_status_t instead
    of the raw ESP-IDF esp_err_t, so the project's error surface is not
    coupled to ESP-IDF's error numbering. The only exception is a function
    signature ESP-IDF itself dictates (for example sas_http_handler_fn,
    which must return esp_err_t to satisfy esp_https_server's httpd_uri_t
    contract); such boundary signatures stay esp_err_t and are documented
    as exceptions where they occur.

    Value layout:
    - SAS_STATUS_OK and the generic SAS_ERR_* codes below occupy the low
      range (0-15).
    - Each module reserves a 0x100-wide block starting at 0x8100 for its
      own SAS_<MOD>_ERR_* codes, assigned in the project's module
      development order: Logger 0x8100, Configuration Manager 0x8200,
      System Information 0x8300, Event Bus 0x8400, File Manager 0x8500,
      Wi-Fi Manager 0x8600, HTTP Server 0x8700, REST API 0x8800,
      WebSocket 0x8900, BLE Scanner 0x8A00, OTA Manager 0x8B00,
      Terminal 0x8C00, Authentication 0x8D00.

    \dependencies
    None. This is a Layer-1 common definitions header, not a module with
    its own .c file: it declares only a type and constants, so the
    project's one-.c/.h-file-per-module rule does not apply to it.
*/

#pragma once

/* 1. Standard includes */
#include <stdint.h>

/* 2. ESP-IDF includes */
/* (none) */

/* 3. Project includes */
/* (none) */

/* 4. Public types */

/*! Project-wide status/error code returned by sas_<module>_* public API functions. */
typedef uint32_t sas_status_t;

/* 5. Public constants */

#define SAS_STATUS_OK (0U)         ///< Success.
#define SAS_ERR_INVALID_ARG (1U)   ///< A public-API argument failed validation at the boundary.
#define SAS_ERR_INVALID_STATE (2U) ///< Call is not valid in the module's current lifecycle state.
#define SAS_ERR_NOT_SUPPORTED (3U) ///< Not yet implemented (pre-implementation skeleton stub).
#define SAS_ERR_NO_MEM (4U)        ///< A fixed-size static pool/table is exhausted.
#define SAS_ERR_TIMEOUT (5U)       ///< Operation did not complete within its bounded time budget.

/* 6. Public API declarations */
/* (none: this header declares only the common type and constants above) */
