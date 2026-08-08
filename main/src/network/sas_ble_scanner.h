/*!
    \file    sas_ble_scanner.h
    \brief   Public interface for the BLE Scanner module.

    \description
    Observer-only BLE advertisement scanner. This module owns passive BLE
    scanning and advertiser de-duplication, publishing a discovery event the
    first time each MAC address is observed; it never initiates a GAP central
    or GATT connection procedure.

    \dependencies
    - Event Bus (main/src/core/sas_event_bus.h): publishes
      SAS_EVENT_BLE_DEVICE_DISCOVERED; runtime dependency only, no
      compile-time header include.
*/

#pragma once

/* 1. Standard includes */
#include <stdint.h>

/* 2. ESP-IDF includes */
/* (none) */

/* 3. Project includes */
#include "../core/sas_status.h"

/* 4. Public types */

/*! One de-duplicated BLE advertiser entry. */
typedef struct {
    uint8_t mac[6];       ///< Advertiser MAC address.
    char    name[32];     ///< Advertised device name, if present; empty otherwise.
    int8_t  rssi;         ///< RSSI at last observation, in dBm.
    int64_t last_seen_us; ///< Timestamp of last observation, microseconds since boot.
} sas_ble_device_t;

/* 5. Public constants */

#define SAS_BLE_ERR_BASE (0x8A00U) ///< Custom status base for this module.
#define SAS_BLE_ERR_ALREADY_SCANNING (SAS_BLE_ERR_BASE + 1U) ///< scan_start() failed: a scan is already in progress.
#define SAS_BLE_ERR_NOT_SCANNING (SAS_BLE_ERR_BASE + 2U) ///< scan_stop() failed: no scan is currently in progress.

/*! Maximum number of de-duplicated advertiser entries tracked at once.
    Placeholder sizing; to be tuned during implementation against actual
    observed-device counts. */
#define SAS_BLE_MAX_DEVICES (32U)

/* 6. Public API declarations */

/*!
    \brief   Initialize the BLE Scanner module.
    \return  SAS_STATUS_OK on success.
*/
sas_status_t sas_ble_init(void);

/*!
    \brief   Start active BLE Scanner operation.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_ble_start(void);

/*!
    \brief   Stop active BLE Scanner operation without releasing resources.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_ble_stop(void);

/*!
    \brief   Release all resources acquired by sas_ble_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_ble_deinit(void);

/*!
    \brief   Begin passive (observer-only) BLE advertisement scanning.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started;
             SAS_BLE_ERR_ALREADY_SCANNING if a scan is already in progress.
*/
sas_status_t sas_ble_scan_start(void);

/*!
    \brief   Stop BLE advertisement scanning.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started;
             SAS_BLE_ERR_NOT_SCANNING if no scan is currently in progress.
*/
sas_status_t sas_ble_scan_stop(void);

/*!
    \brief   Copy out the current de-duplicated advertiser table.
    \param   out_array   Non-NULL destination array of at least max_entries
                         elements.
    \param   max_entries Capacity of out_array, in entries.
    \param   out_count   Non-NULL; receives the number of entries written.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out_array or
             out_count is NULL; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_ble_get_devices(sas_ble_device_t *out_array,
                                  uint16_t max_entries,
                                  uint16_t *out_count);
