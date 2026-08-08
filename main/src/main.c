/*!
    \file    main.c
    \brief   Application entry point and boot sequencing.

    \description
    app_main() initializes and starts every SAS module strictly in the
    project's fixed module development order, which is also a valid
    topological order of the module dependency graph. Dashboard has no
    device-side lifecycle and is not part of this sequence.

    \dependencies
    Every SAS module under main/src/.
*/

/* 1. Includes */
#include "core/sas_status.h"
#include "core/sas_logger.h"
#include "core/sas_config.h"
#include "core/sas_sysinfo.h"
#include "core/sas_event_bus.h"
#include "storage/sas_file_manager.h"
#include "network/sas_wifi_manager.h"
#include "web/sas_http_server.h"
#include "web/sas_rest_api.h"
#include "web/sas_websocket.h"
#include "network/sas_ble_scanner.h"
#include "network/sas_ota_manager.h"
#include "terminal/sas_terminal.h"
#include "auth/sas_auth.h"

#include <stdint.h>

/* 2. Private macros */
#define SAS_BOOT_MODULE_COUNT (13U) ///< Modules with a lifecycle in the boot sequence (Dashboard has none).

/* 3. Private typedefs */

/*! Per-module boot outcome; extends System Information. */
typedef enum {
    SAS_BOOT_STATUS_OK = 0,
    SAS_BOOT_STATUS_FAILED,
    SAS_BOOT_STATUS_SKIPPED_DEPENDENCY,
} sas_boot_status_t;

/*! One boot-sequence record. */
typedef struct {
    const char       *module_name; ///< Module name, e.g. "logger".
    sas_boot_status_t status;      ///< Outcome recorded for this module.
} sas_boot_record_t;

/* 4. Private variables */
static sas_boot_record_t s_boot_status[SAS_BOOT_MODULE_COUNT];

/* 5. Forward declarations */
static void record_boot_status(uint8_t index, const char *name, sas_status_t status);

/* 6. Public functions */

void app_main(void)
{
    uint8_t idx = 0U;
    sas_status_t status;

    /* TODO:
     * 1. This block already calls each module's real init()/start() pair
     *    in Development Order and records the outcome via
     *    record_boot_status(); every call currently returns
     *    SAS_ERR_NOT_SUPPORTED because each module is still a stub.
     * 2. Add the skip-on-dependency-failure rule: before calling a
     *    module's init(), check that every module it depends on recorded
     *    SAS_BOOT_STATUS_OK; if not, record
     *    SAS_BOOT_STATUS_SKIPPED_DEPENDENCY and skip both init() and
     *    start() for this module.
     * 3. Expose s_boot_status through an extension of System Information
     *    (sas_sysinfo_get_boot_status(), see the TODO in
     *    main/src/core/sas_sysinfo.h) once that out-parameter shape is
     *    finalized. */

    /* 1. Logger */
    {
        static const sas_logger_config_t log_cfg = {
            .min_level = SAS_LOG_LEVEL_INFO,
            .persist_enabled = false,
            .log_file_path = "/logs/sas.log",
        };
        status = sas_logger_init(&log_cfg);
        if (status == SAS_STATUS_OK) {
            status = sas_logger_start();
        }
        record_boot_status(idx++, "logger", status);
    }

    /* 2. Configuration Manager */
    status = sas_config_init();
    if (status == SAS_STATUS_OK) {
        status = sas_config_start();
    }
    record_boot_status(idx++, "config", status);

    /* 3. System Information */
    status = sas_sysinfo_init();
    if (status == SAS_STATUS_OK) {
        status = sas_sysinfo_start();
    }
    record_boot_status(idx++, "sysinfo", status);

    /* 4. Event Bus */
    status = sas_event_bus_init();
    if (status == SAS_STATUS_OK) {
        status = sas_event_bus_start();
    }
    record_boot_status(idx++, "event_bus", status);

    /* 5. File Manager */
    status = sas_fs_init();
    if (status == SAS_STATUS_OK) {
        status = sas_fs_start();
    }
    record_boot_status(idx++, "file_manager", status);

    /* 6. Wi-Fi Manager */
    status = sas_wifi_init();
    if (status == SAS_STATUS_OK) {
        status = sas_wifi_start();
    }
    record_boot_status(idx++, "wifi_manager", status);

    /* 7. HTTP Server */
    status = sas_http_init();
    if (status == SAS_STATUS_OK) {
        status = sas_http_start();
    }
    record_boot_status(idx++, "http_server", status);

    /* 8. REST API */
    status = sas_rest_api_init();
    if (status == SAS_STATUS_OK) {
        status = sas_rest_api_start();
    }
    record_boot_status(idx++, "rest_api", status);

    /* 9. WebSocket */
    status = sas_ws_init();
    if (status == SAS_STATUS_OK) {
        status = sas_ws_start();
    }
    record_boot_status(idx++, "websocket", status);

    /* 10. Dashboard: client-side only, no device lifecycle. */

    /* 11. BLE Scanner */
    status = sas_ble_init();
    if (status == SAS_STATUS_OK) {
        status = sas_ble_start();
    }
    record_boot_status(idx++, "ble_scanner", status);

    /* 12. OTA Manager */
    status = sas_ota_init();
    if (status == SAS_STATUS_OK) {
        status = sas_ota_start();
    }
    record_boot_status(idx++, "ota_manager", status);

    /* 13. Terminal */
    status = sas_terminal_init();
    if (status == SAS_STATUS_OK) {
        status = sas_terminal_start();
    }
    record_boot_status(idx++, "terminal", status);

    /* 14. Authentication */
    status = sas_auth_init();
    if (status == SAS_STATUS_OK) {
        status = sas_auth_start();
    }
    record_boot_status(idx++, "auth", status);
}

/* 7. Private functions */

/*!
    \brief Record one module's boot outcome in s_boot_status.
*/
static void record_boot_status(uint8_t index, const char *name, sas_status_t status)
{
    if (index >= SAS_BOOT_MODULE_COUNT) {
        return;
    }

    s_boot_status[index].module_name = name;
    s_boot_status[index].status = (status == SAS_STATUS_OK) ? SAS_BOOT_STATUS_OK : SAS_BOOT_STATUS_FAILED;
}
