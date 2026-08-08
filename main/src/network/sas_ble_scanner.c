/*!
    \file    sas_ble_scanner.c
    \brief   Implementation skeleton for the BLE Scanner module.

    \description
    See sas_ble_scanner.h for the public contract. This file currently
    contains only the lifecycle and scanning API stubs; behaviour is filled
    in per module during implementation, in Development Order.

    Configuration keys consumed once the Configuration Manager integration is
    implemented (not yet read by this skeleton):
    - ble.scan.interval_ms
    - ble.scan.window_ms
    - ble.max_devices

    \dependencies
    - Event Bus (main/src/core/sas_event_bus.h): publishes
      SAS_EVENT_BLE_DEVICE_DISCOVERED.
*/

/* 1. Includes */
#include "sas_ble_scanner.h"

#include <stdbool.h>
#include <stddef.h>

/* 2. Private macros */
/* (none yet: NimBLE host configuration constants to be added during
 * implementation) */

/* 3. Private typedefs */
/* (none yet: internal de-duplication table entry type to be added during
 * implementation) */

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started     = false;

/* 5. Forward declarations */
/* (none yet: NimBLE GAP advertisement callback to be added during
 * implementation) */

/* 6. Public functions */

sas_status_t sas_ble_init(void)
{
    /* TODO:
     * 1. Initialize the NimBLE host bundled with ESP-IDF, configured for
     *    observer-only role (no GAP central/connect procedures).
     * 2. Allocate the static SAS_BLE_MAX_DEVICES-entry de-duplication table
     *    (static allocation only, no dynamic allocation after init). */
    s_initialized = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_ble_start(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Bring up the NimBLE host task.
     * 2. Register the GAP advertisement-report callback.
     * 3. Do not begin scanning here; see sas_ble_scan_start(). */
    s_started = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_ble_stop(void)
{
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Stop any in-progress scan.
     * 2. Stop the NimBLE host task started in sas_ble_start().
     * 3. Leave the de-duplication table acquired in sas_ble_init()
     *    untouched (stop() must not release resources). */
    s_started = false;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_ble_deinit(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Release the NimBLE host and the de-duplication table acquired in
     *    sas_ble_init().
     * 2. Reset module state to uninitialized. */
    s_initialized = false;
    return SAS_ERR_NOT_SUPPORTED;
}

/*
 * Hard constraint: this module is observer-only. No GATT connection is
 * ever initiated here or by any function it calls.
 */
sas_status_t sas_ble_scan_start(void)
{
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. If a scan is already in progress, return
     *    SAS_BLE_ERR_ALREADY_SCANNING instead of restarting it.
     * 2. Otherwise begin a passive-only NimBLE GAP discovery procedure (no
     *    active scan requests, no connect procedures).
     * 3. Rely on the ESP-IDF built-in Wi-Fi/BLE coexistence scheduler for
     *    time-division sharing with Wi-Fi; no SAS-level arbitration is
     *    implemented. */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_ble_scan_stop(void)
{
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. If no scan is currently in progress, return
     *    SAS_BLE_ERR_NOT_SCANNING instead.
     * 2. Otherwise stop the NimBLE GAP discovery procedure started by
     *    sas_ble_scan_start(). */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_ble_get_devices(sas_ble_device_t *out_array,
                                  uint16_t max_entries,
                                  uint16_t *out_count)
{
    if ((out_array == NULL) || (out_count == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    (void)max_entries;

    /* TODO:
     * 1. Copy up to max_entries entries from the static de-duplication
     *    table into out_array.
     * 2. Write the copied count to *out_count. */
    return SAS_ERR_NOT_SUPPORTED;
}

/* 7. Private functions */

/* TODO: GAP advertisement-report callback to be added here during
 * implementation. On each report:
 * 1. Look up the MAC in the static SAS_BLE_MAX_DEVICES-entry table.
 * 2. If found, update rssi and last_seen_us in place without publishing an
 *    event.
 * 3. If not found and the table has a free slot, insert a new entry and
 *    publish SAS_EVENT_BLE_DEVICE_DISCOVERED.
 * 4. If not found and the table is full, evict the least-recently-seen
 *    entry before inserting. */
