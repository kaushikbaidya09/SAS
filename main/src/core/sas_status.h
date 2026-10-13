//=============================================================================
/*!
    \file    sas_status.h
    \brief   Project-wide status/error code type.
*/
//=============================================================================

// Public functions return sas_status_t rather than the ESP-IDF esp_err_t.
// The exception is a signature that ESP-IDF dictates, such as an HTTP handler.
//
// Value layout:
// - 0-15: SAS_STATUS_OK and the generic SAS_ERR_* codes below.
// - 0x8100 upwards: one 0x100-wide block per module for its own error codes:
//   Logger 0x8100, Configuration Manager 0x8200, System Information 0x8300,
//   Event Bus 0x8400, File Manager 0x8500, Wi-Fi Manager 0x8600,
//   HTTP Server 0x8700, REST API 0x8800, WebSocket 0x8900, BLE Scanner 0x8A00,
//   OTA Manager 0x8B00, Terminal 0x8C00, Authentication 0x8D00.

#ifndef SAS_STATUS_H
#define SAS_STATUS_H

#include <stdint.h>

/*! \brief Status code returned by public functions. */
typedef uint32_t sas_status_t;

#define SAS_STATUS_OK         (0U) ///< Success.
#define SAS_ERR_INVALID_ARG   (1U) ///< An argument is invalid.
#define SAS_ERR_INVALID_STATE (2U) ///< Not allowed in the module's current state.
#define SAS_ERR_NOT_SUPPORTED (3U) ///< Not implemented or not available in this build.
#define SAS_ERR_NO_MEM        (4U) ///< A fixed-size pool or table is full.
#define SAS_ERR_TIMEOUT       (5U) ///< Operation did not complete in time.

#endif /* SAS_STATUS_H */
