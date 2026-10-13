//=============================================================================
/*!
    \file    sas_ble_scanner.h
    \brief   Public interface for the BLE Scanner module.
*/
//=============================================================================

// Passively listens for BLE advertisements and keeps a de-duplicated device
// list, publishing an event the first time each device is seen. It never
// connects to a device.

#ifndef SAS_BLE_SCANNER_H
#define SAS_BLE_SCANNER_H

#include <stdint.h>

#include "../core/sas_status.h"

/*! \brief One de-duplicated BLE advertiser entry. */
typedef struct
{
    uint8_t mac[6];       ///< Advertiser MAC address.
    char name[32];        ///< Advertised device name, if present; empty otherwise.
    int8_t rssi;          ///< RSSI at last observation, in dBm.
    int64_t last_seen_us; ///< Timestamp of last observation, microseconds since boot.
} sas_ble_device_t;

#define SAS_BLE_ERR_BASE (0x8A00U) ///< Custom status base for this module.
/*! \brief scan_start() failed: a scan is already in progress. */
#define SAS_BLE_ERR_ALREADY_SCANNING (SAS_BLE_ERR_BASE + 1U)
/*! \brief scan_stop() failed: no scan is currently in progress. */
#define SAS_BLE_ERR_NOT_SCANNING (SAS_BLE_ERR_BASE + 2U)
/*!
    \brief   scan_start() failed at the NimBLE driver level (address type
             resolution or ble_gap_disc()).
*/
#define SAS_BLE_ERR_SCAN_FAILED (SAS_BLE_ERR_BASE + 3U)

/*! \brief Maximum number of devices tracked at once. */
#define SAS_BLE_MAX_DEVICES (32U)

sas_status_t sas_ble_init(void);

sas_status_t sas_ble_start(void);

sas_status_t sas_ble_stop(void);

sas_status_t sas_ble_deinit(void);

sas_status_t sas_ble_scan_start(void);

sas_status_t sas_ble_scan_stop(void);

sas_status_t sas_ble_get_devices(sas_ble_device_t *out_array, uint16_t max_entries,
                                 uint16_t *out_count);

#endif /* SAS_BLE_SCANNER_H */
