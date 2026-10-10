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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 2. Private macros */
#define SAS_BOOT_MODULE_COUNT                                                                      \
    (13U) ///< Modules with a lifecycle in the boot sequence (Dashboard has none).

/* s_boot_status[] indices, in Development Order; used by the dependency
 * tables below. A dependency can only ever reference a lower index (an
 * earlier-booting module) - by construction, s_boot_status[] has no
 * meaningful entry yet for anything at or after the module currently
 * booting. */
#define SAS_BOOT_IDX_LOGGER (0U)
#define SAS_BOOT_IDX_CONFIG (1U)
#define SAS_BOOT_IDX_SYSINFO (2U)
#define SAS_BOOT_IDX_EVENT_BUS (3U)
#define SAS_BOOT_IDX_FILE_MANAGER (4U)
#define SAS_BOOT_IDX_WIFI_MANAGER (5U)
#define SAS_BOOT_IDX_HTTP_SERVER (6U)
#define SAS_BOOT_IDX_REST_API (7U)
#define SAS_BOOT_IDX_WEBSOCKET (8U)
#define SAS_BOOT_IDX_BLE_SCANNER (9U)
#define SAS_BOOT_IDX_OTA_MANAGER (10U)
#define SAS_BOOT_IDX_TERMINAL (11U)
#define SAS_BOOT_IDX_AUTH (12U)

/* 3. Private typedefs */

/*! Per-module boot outcome; extends System Information. */
typedef enum {
    SAS_BOOT_STATUS_OK = 0,
    SAS_BOOT_STATUS_FAILED,
    SAS_BOOT_STATUS_SKIPPED_DEPENDENCY,
} sas_boot_status_t;

/*! One boot-sequence record. */
typedef struct {
    const char *module_name;  ///< Module name, e.g. "logger".
    sas_boot_status_t status; ///< Outcome recorded for this module.
} sas_boot_record_t;

/* 4. Private variables */
static sas_boot_record_t s_boot_status[SAS_BOOT_MODULE_COUNT];

/* 5. Forward declarations */
static void record_boot_status(uint8_t index, const char *name, sas_status_t status);
static void record_boot_skipped(uint8_t index, const char *name);
static bool dependencies_met(const uint8_t *dep_indices, size_t dep_count);

/* 6. Public functions */

void app_main(void)
{
    uint8_t idx = 0U;
    sas_status_t status;

    /* This block calls each module's real init()/start() pair in
     * Development Order, gated by dependencies_met() for every module that
     * documents a dependency on an earlier one, and records the outcome
     * via record_boot_status()/record_boot_skipped(). Auth, OTA Manager,
     * Terminal, and System Information remain stubs (SAS_ERR_NOT_SUPPORTED)
     * as of this change - out of scope for the Wi-Fi/BLE/log dashboard
     * feature - so they still record SAS_BOOT_STATUS_FAILED whenever their
     * own dependencies are met.
     *
     * TODO: expose s_boot_status through an extension of System Information
     * (sas_sysinfo_get_boot_status(), see the TODO in
     * main/src/core/sas_sysinfo.h) once that out-parameter shape is
     * finalized. */

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

    /* 7. HTTP Server (depends on: File Manager, Event Bus - sas_http_server.h) */
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_FILE_MANAGER, SAS_BOOT_IDX_EVENT_BUS};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0]))) {
            status = sas_http_init();
            if (status == SAS_STATUS_OK) {
                status = sas_http_start();
            }
            record_boot_status(idx, "http_server", status);
        } else {
            record_boot_skipped(idx, "http_server");
        }
        idx++;
    }

    /* 8. REST API (depends on: HTTP Server, Wi-Fi Manager, Logger - the endpoints this build
     * actually registers; Sysinfo/Config/Files/OTA/Auth from sas_rest_api.h's full dependency
     * list back endpoint groups this build does not register, so they are deliberately not
     * gated here - see sas_rest_api.c's file header. BLE Scanner backs /api/v1/ble/devices but
     * boots after REST API in this sequence, so it cannot be checked here either; that is fine,
     * since sas_rest_api_start() itself never calls into BLE Scanner - only a later HTTP
     * request's handler does, by which point BLE Scanner has already booted.) */
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_HTTP_SERVER, SAS_BOOT_IDX_WIFI_MANAGER,
                                       SAS_BOOT_IDX_LOGGER};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0]))) {
            status = sas_rest_api_init();
            if (status == SAS_STATUS_OK) {
                status = sas_rest_api_start();
            }
            record_boot_status(idx, "rest_api", status);
        } else {
            record_boot_skipped(idx, "rest_api");
        }
        idx++;
    }

    /* 9. WebSocket (depends on: HTTP Server, Event Bus - sas_websocket.h) */
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_HTTP_SERVER, SAS_BOOT_IDX_EVENT_BUS};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0]))) {
            status = sas_ws_init();
            if (status == SAS_STATUS_OK) {
                status = sas_ws_start();
            }
            record_boot_status(idx, "websocket", status);
        } else {
            record_boot_skipped(idx, "websocket");
        }
        idx++;
    }

    /* 10. Dashboard: client-side only, no device lifecycle. */

    /* 11. BLE Scanner (no documented dependency on another SAS module) */
    status = sas_ble_init();
    if (status == SAS_STATUS_OK) {
        status = sas_ble_start();
    }
    record_boot_status(idx++, "ble_scanner", status);

    /* 12. OTA Manager (depends on: Sysinfo, Event Bus - sas_ota_manager.h; File Manager is
     * documented there as optional, so not gated here) */
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_SYSINFO, SAS_BOOT_IDX_EVENT_BUS};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0]))) {
            status = sas_ota_init();
            if (status == SAS_STATUS_OK) {
                status = sas_ota_start();
            }
            record_boot_status(idx, "ota_manager", status);
        } else {
            record_boot_skipped(idx, "ota_manager");
        }
        idx++;
    }

    /* 13. Terminal (depends on: WebSocket - sas_terminal.h; its Event Bus dependency is
     * documented there as "not yet included", so not gated here) */
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_WEBSOCKET};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0]))) {
            status = sas_terminal_init();
            if (status == SAS_STATUS_OK) {
                status = sas_terminal_start();
            }
            record_boot_status(idx, "terminal", status);
        } else {
            record_boot_skipped(idx, "terminal");
        }
        idx++;
    }

    /* 14. Authentication (depends on: Configuration Manager, HTTP Server, Event Bus -
     * sas_auth.h) */
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_CONFIG, SAS_BOOT_IDX_HTTP_SERVER,
                                       SAS_BOOT_IDX_EVENT_BUS};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0]))) {
            status = sas_auth_init();
            if (status == SAS_STATUS_OK) {
                status = sas_auth_start();
            }
            record_boot_status(idx, "auth", status);
        } else {
            record_boot_skipped(idx, "auth");
        }
        idx++;
    }
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
    s_boot_status[index].status =
        (status == SAS_STATUS_OK) ? SAS_BOOT_STATUS_OK : SAS_BOOT_STATUS_FAILED;
}

/*!
    \brief Record that a module's init()/start() were skipped because a
           dependency did not record SAS_BOOT_STATUS_OK.
*/
static void record_boot_skipped(uint8_t index, const char *name)
{
    if (index >= SAS_BOOT_MODULE_COUNT) {
        return;
    }

    s_boot_status[index].module_name = name;
    s_boot_status[index].status = SAS_BOOT_STATUS_SKIPPED_DEPENDENCY;
}

/*!
    \brief   Check whether every module named in dep_indices already
             recorded SAS_BOOT_STATUS_OK.
    \note    Every dep_indices entry must be a lower index than the module
             currently booting (an earlier entry in Development Order);
             s_boot_status[] has no meaningful value yet for anything at
             or after it.
*/
static bool dependencies_met(const uint8_t *dep_indices, size_t dep_count)
{
    for (size_t i = 0U; i < dep_count; i++) {
        if (s_boot_status[dep_indices[i]].status != SAS_BOOT_STATUS_OK) {
            return false;
        }
    }
    return true;
}
