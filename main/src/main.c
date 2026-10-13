//=============================================================================
/*!
    \file    main.c
    \brief   Application entry point and boot sequencing.
*/
//=============================================================================

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

#define SAS_BOOT_MODULE_COUNT (13U) ///< Number of modules in the boot sequence.

// Position of each module in s_boot_status[], in boot order. A module may
// only depend on modules with a lower index, because those have already booted.
#define SAS_BOOT_IDX_LOGGER       (0U)
#define SAS_BOOT_IDX_CONFIG       (1U)
#define SAS_BOOT_IDX_SYSINFO      (2U)
#define SAS_BOOT_IDX_EVENT_BUS    (3U)
#define SAS_BOOT_IDX_FILE_MANAGER (4U)
#define SAS_BOOT_IDX_WIFI_MANAGER (5U)
#define SAS_BOOT_IDX_HTTP_SERVER  (6U)
#define SAS_BOOT_IDX_REST_API     (7U)
#define SAS_BOOT_IDX_WEBSOCKET    (8U)
#define SAS_BOOT_IDX_BLE_SCANNER  (9U)
#define SAS_BOOT_IDX_OTA_MANAGER  (10U)
#define SAS_BOOT_IDX_TERMINAL     (11U)
#define SAS_BOOT_IDX_AUTH         (12U)

static const char *TAG = "sas_boot"; ///< Log tag for this file.

static sas_boot_record_t s_boot_status[SAS_BOOT_MODULE_COUNT]; ///< Boot outcome of each module.

static void record_boot_status(uint8_t index, const char *name, sas_status_t status);
static void record_boot_skipped(uint8_t index, const char *name);
static bool dependencies_met(const uint8_t *dep_indices, size_t dep_count);

/*!
    \brief   Application entry point: boot every module in dependency order.
*/
void app_main(void)
{
    uint8_t idx = 0U;
    sas_status_t status;

    // Each module is initialised and then started. A module is skipped when one
    // it depends on failed to boot. Every outcome is recorded for the boot-status
    // report.

    // Logger
    {
        static const sas_logger_config_t log_cfg = {
            .min_level       = SAS_LOG_LEVEL_INFO,
            .persist_enabled = false,
            .log_file_path   = "/logs/sas.log",
        };
        status = sas_logger_init(&log_cfg);
        if (status == SAS_STATUS_OK)
        {
            status = sas_logger_start();
        }
        record_boot_status(idx++, "logger", status);
    }

    // Configuration Manager
    status = sas_config_init();
    if (status == SAS_STATUS_OK)
    {
        status = sas_config_start();
    }
    record_boot_status(idx++, "config", status);

    // System Information
    status = sas_sysinfo_init();
    if (status == SAS_STATUS_OK)
    {
        status = sas_sysinfo_start();
    }
    record_boot_status(idx++, "sysinfo", status);

    // Event Bus
    status = sas_event_bus_init();
    if (status == SAS_STATUS_OK)
    {
        status = sas_event_bus_start();
    }
    record_boot_status(idx++, "event_bus", status);

    // File Manager
    status = sas_fs_init();
    if (status == SAS_STATUS_OK)
    {
        status = sas_fs_start();
    }
    record_boot_status(idx++, "file_manager", status);

    // The stored configuration could not be read before the filesystem was
    // mounted, so load it now, before the modules that use it. On first boot
    // there is no file and the defaults stay in effect.
    if (status == SAS_STATUS_OK)
    {
        if (sas_config_reload() == SAS_STATUS_OK)
        {
            sas_log_info("configuration loaded from the filesystem");
        }
        else
        {
            sas_log_info("no stored configuration; using defaults");
        }
    }

    // Wi-Fi Manager
    status = sas_wifi_init();
    if (status == SAS_STATUS_OK)
    {
        status = sas_wifi_start();
    }
    record_boot_status(idx++, "wifi_manager", status);

    // HTTP Server
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_FILE_MANAGER, SAS_BOOT_IDX_EVENT_BUS};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0])))
        {
            status = sas_http_init();
            if (status == SAS_STATUS_OK)
            {
                status = sas_http_start();
            }
            record_boot_status(idx, "http_server", status);
        }
        else
        {
            record_boot_skipped(idx, "http_server");
        }
        idx++;
    }

    // REST API. BLE Scanner, OTA Manager and Authentication boot later and are
    // not checked here: they are only called from request handlers, which run
    // after the boot sequence.
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_HTTP_SERVER, SAS_BOOT_IDX_WIFI_MANAGER,
                                       SAS_BOOT_IDX_LOGGER,      SAS_BOOT_IDX_CONFIG,
                                       SAS_BOOT_IDX_SYSINFO,     SAS_BOOT_IDX_FILE_MANAGER};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0])))
        {
            status = sas_rest_api_init();
            if (status == SAS_STATUS_OK)
            {
                status = sas_rest_api_start();
            }
            record_boot_status(idx, "rest_api", status);
        }
        else
        {
            record_boot_skipped(idx, "rest_api");
        }
        idx++;
    }

    // WebSocket
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_HTTP_SERVER, SAS_BOOT_IDX_EVENT_BUS,
                                       SAS_BOOT_IDX_SYSINFO};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0])))
        {
            status = sas_ws_init();
            if (status == SAS_STATUS_OK)
            {
                status = sas_ws_start();
            }
            record_boot_status(idx, "websocket", status);
        }
        else
        {
            record_boot_skipped(idx, "websocket");
        }
        idx++;
    }

    // BLE Scanner
    status = sas_ble_init();
    if (status == SAS_STATUS_OK)
    {
        status = sas_ble_start();
    }
    record_boot_status(idx++, "ble_scanner", status);

    // OTA Manager. File Manager is optional for it, so it is not a dependency.
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_SYSINFO, SAS_BOOT_IDX_EVENT_BUS};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0])))
        {
            status = sas_ota_init();
            if (status == SAS_STATUS_OK)
            {
                status = sas_ota_start();
            }
            record_boot_status(idx, "ota_manager", status);
        }
        else
        {
            record_boot_skipped(idx, "ota_manager");
        }
        idx++;
    }

    // Terminal
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_WEBSOCKET, SAS_BOOT_IDX_SYSINFO,
                                       SAS_BOOT_IDX_WIFI_MANAGER};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0])))
        {
            status = sas_terminal_init();
            if (status == SAS_STATUS_OK)
            {
                status = sas_terminal_start();
            }
            record_boot_status(idx, "terminal", status);
        }
        else
        {
            record_boot_skipped(idx, "terminal");
        }
        idx++;
    }

    // Authentication
    {
        static const uint8_t deps[] = {SAS_BOOT_IDX_CONFIG, SAS_BOOT_IDX_HTTP_SERVER,
                                       SAS_BOOT_IDX_EVENT_BUS};
        if (dependencies_met(deps, sizeof(deps) / sizeof(deps[0])))
        {
            status = sas_auth_init();
            if (status == SAS_STATUS_OK)
            {
                status = sas_auth_start();
            }
            record_boot_status(idx, "auth", status);
        }
        else
        {
            record_boot_skipped(idx, "auth");
        }
        idx++;
    }

    sas_log_info("boot sequence complete");
}

/*!
    \brief   Record and report whether a module booted successfully.
*/
static void record_boot_status(uint8_t index, const char *name, sas_status_t status)
{
    if (index >= SAS_BOOT_MODULE_COUNT)
    {
        return;
    }

    s_boot_status[index].module_name = name;
    s_boot_status[index].status =
        (status == SAS_STATUS_OK) ? SAS_BOOT_STATUS_OK : SAS_BOOT_STATUS_FAILED;
    (void)sas_sysinfo_record_boot_status(name, s_boot_status[index].status);

    // A failed module is logged and the boot sequence carries on (SAS-INT-FR-002).
    if (status != SAS_STATUS_OK)
    {
        sas_log_error("module '%s' failed to boot (0x%x)", name, (unsigned int)status);
    }
}

/*!
    \brief   Record that a module was skipped because a dependency failed to
             boot.
*/
static void record_boot_skipped(uint8_t index, const char *name)
{
    if (index >= SAS_BOOT_MODULE_COUNT)
    {
        return;
    }

    s_boot_status[index].module_name = name;
    s_boot_status[index].status      = SAS_BOOT_STATUS_SKIPPED_DEPENDENCY;
    (void)sas_sysinfo_record_boot_status(name, SAS_BOOT_STATUS_SKIPPED_DEPENDENCY);
    sas_log_warn("module '%s' skipped: a dependency did not boot", name);
}

/*!
    \brief   Check whether every listed dependency booted successfully.
*/
static bool dependencies_met(const uint8_t *dep_indices, size_t dep_count)
{
    for (size_t i = 0U; i < dep_count; i++)
    {
        if (s_boot_status[dep_indices[i]].status != SAS_BOOT_STATUS_OK)
        {
            return false;
        }
    }
    return true;
}
