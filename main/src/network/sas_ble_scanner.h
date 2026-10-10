/*!
    \file    sas_ble_scanner.h
    \brief   Public interface for the BLE Scanner module.

    \details
    Passively listens for BLE advertisements and keeps a de-duplicated
    device list, publishing a discovery event the first time each MAC
    shows up. It never initiates a GAP central or GATT connection to
    anything it hears.
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
    char name[32];        ///< Advertised device name, if present; empty otherwise.
    int8_t rssi;          ///< RSSI at last observation, in dBm.
    int64_t last_seen_us; ///< Timestamp of last observation, microseconds since boot.
} sas_ble_device_t;

/* 5. Public constants */

#define SAS_BLE_ERR_BASE (0x8A00U) ///< Custom status base for this module.
#define SAS_BLE_ERR_ALREADY_SCANNING                                                               \
    (SAS_BLE_ERR_BASE + 1U) ///< scan_start() failed: a scan is already in progress.
#define SAS_BLE_ERR_NOT_SCANNING                                                                   \
    (SAS_BLE_ERR_BASE + 2U) ///< scan_stop() failed: no scan is currently in progress.
#define SAS_BLE_ERR_SCAN_FAILED                                                                    \
    (SAS_BLE_ERR_BASE + 3U) ///< scan_start() failed at the NimBLE driver level (address type
                            ///< resolution or ble_gap_disc()).

/*! Maximum number of de-duplicated advertiser entries tracked at once.
    Placeholder sizing; to be tuned during implementation against actual
    observed-device counts. */
#define SAS_BLE_MAX_DEVICES (32U)

/* 6. Public API declarations */

/*!
    \brief   Initialize the BLE Scanner module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already
             initialized.
*/
sas_status_t sas_ble_init(void);

/*!
    \brief   Start active BLE Scanner operation.
    \note    Brings up the NimBLE host task on first call and reuses it
             after a stop/start cycle (see sas_ble_scanner.c). Does not
             itself begin scanning - see sas_ble_scan_start().
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_STATE if not initialized or
             already started.
*/
sas_status_t sas_ble_start(void);

/*!
    \brief   Stop active BLE Scanner operation: cancels any scan in
             progress.
    \note    The NimBLE host task keeps running; it isn't designed to be
             cleanly restarted and is only torn down by sas_ble_deinit().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_ble_stop(void);

/*!
    \brief   Release all resources acquired by sas_ble_init(), including
             the NimBLE host task brought up by sas_ble_start().
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_STATE if not initialized or
             still started (call sas_ble_stop() first).
*/
sas_status_t sas_ble_deinit(void);

/*!
    \brief   Begin passive (observer-only) BLE advertisement scanning.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_STATE (not started);
             SAS_BLE_ERR_ALREADY_SCANNING (scan already running);
             SAS_BLE_ERR_SCAN_FAILED (NimBLE driver rejected the request).
*/
sas_status_t sas_ble_scan_start(void);

/*!
    \brief   Stop BLE advertisement scanning.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_STATE (not started);
             SAS_BLE_ERR_NOT_SCANNING (no scan in progress).
*/
sas_status_t sas_ble_scan_stop(void);

/*!
    \brief   Copy out the current de-duplicated advertiser table.
    \param   out_array   Non-NULL destination array of at least max_entries
                         elements.
    \param   max_entries Capacity of out_array, in entries.
    \param   out_count   Non-NULL; receives the number of entries written.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_ARG (bad args);
             SAS_ERR_INVALID_STATE (not initialized); SAS_ERR_TIMEOUT (lock
             busy - *out_count left unset).
*/
sas_status_t sas_ble_get_devices(sas_ble_device_t *out_array, uint16_t max_entries,
                                 uint16_t *out_count);
