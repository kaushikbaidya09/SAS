/*!
    \file    sas_wifi_manager.h
    \brief   Public interface for the Wi-Fi Manager module.

    \details
    Manages the device's Wi-Fi connection: connects in station mode with
    stored credentials, reconnects automatically if the link drops, and
    falls back to running its own AP (named from the factory MAC) if it
    can't connect at all. Owns the sas_wifi_state_t state machine below.
*/

#pragma once

/* 1. Standard includes */
#include <stdint.h>

/* 2. ESP-IDF includes */
/* (none: sas_status_t replaces esp_err_t for this module's public API,
 * see @dependencies above) */

/* 3. Project includes */
#include "../core/sas_status.h"

/* 4. Public types */

/*!
    Connection state machine:

    IDLE --connect()--> CONNECTING --link up--> CONNECTED
    CONNECTING --failure--> RECONNECTING (exponential backoff)
    RECONNECTING --retry exhausted--> AP_FALLBACK
    CONNECTED --link lost--> RECONNECTING
    AP_FALLBACK --new valid credentials via sas_wifi_connect()--> CONNECTING

    Each transition above publishes SAS_EVENT_WIFI_STATE_CHANGED on the
    Event Bus.
*/
typedef enum
{
    SAS_WIFI_STATE_IDLE = 0,
    SAS_WIFI_STATE_CONNECTING,
    SAS_WIFI_STATE_CONNECTED,
    SAS_WIFI_STATE_RECONNECTING,
    SAS_WIFI_STATE_AP_FALLBACK,
} sas_wifi_state_t;

/*! One access point found by sas_wifi_scan(). */
typedef struct
{
    char ssid[33];     ///< Null-terminated SSID, up to 32 characters.
    int8_t rssi;       ///< Received signal strength, in dBm.
    uint8_t auth_mode; ///< Authentication mode, per wifi_auth_mode_t.
} sas_wifi_scan_result_t;

/*! Station credentials supplied to sas_wifi_connect(). */
typedef struct
{
    char ssid[33];     ///< Null-terminated SSID, up to 32 characters.
    char password[65]; ///< Null-terminated passphrase, up to 64 characters.
} sas_wifi_credentials_t;

/* 5. Public constants */

/*! Custom sas_status_t base for this module. */
#define SAS_WIFI_ERR_BASE (0x8600U)
/*! sas_wifi_connect() returned after wifi.connect.timeout_ms elapsed without
    a completed connection; the module continues retrying asynchronously
    per the state machine above. */
#define SAS_WIFI_ERR_TIMEOUT (SAS_WIFI_ERR_BASE + 1U)
/*! sas_wifi_scan() failed to start or complete a scan at the driver level. */
#define SAS_WIFI_ERR_SCAN_FAILED (SAS_WIFI_ERR_BASE + 2U)

/* 6. Public API declarations */

/*!
    \brief   Initialize the Wi-Fi Manager module.
    \note    The AP_FALLBACK access point is intentionally open, no
             password - so an operator can always reach this device's
             HTTP server to configure real credentials.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_STATE if already initialized.
*/
sas_status_t sas_wifi_init(void);

/*!
    \brief   Start active Wi-Fi operation (station driver + state machine).
    \note    Connects with stored credentials if present; otherwise goes
             straight to AP_FALLBACK (SAS-WIFI-FR-002).
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_STATE if not initialized or
             already started.
*/
sas_status_t sas_wifi_start(void);

/*!
    \brief   Stop active Wi-Fi operation without releasing resources
             acquired by sas_wifi_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_wifi_stop(void);

/*!
    \brief   Release all resources acquired by sas_wifi_init().
    \note    Does not tear down the shared NVS/netif/event-loop bring-up
             (see sas_wifi_manager.c) - other modules rely on that staying up.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_STATE if not initialized or
             still started (call sas_wifi_stop() first).
*/
sas_status_t sas_wifi_deinit(void);

/*!
    \brief   Connect using creds, driving IDLE/AP_FALLBACK into CONNECTING.
    \param   creds Non-NULL credentials to connect with.
    \note    Always saves creds via Configuration Manager first
             (SAS-WIFI-FR-006), regardless of who calls this.
    \return  SAS_STATUS_OK if accepted; SAS_ERR_INVALID_ARG (creds NULL);
             SAS_ERR_INVALID_STATE (not started); SAS_WIFI_ERR_TIMEOUT if
             the connection didn't complete in time (it keeps retrying in
             the background regardless).
*/
sas_status_t sas_wifi_connect(const sas_wifi_credentials_t *creds);

/*!
    \brief   Scan for nearby access points; blocks the caller for the
             scan's duration.
    \param   out_array   Non-NULL destination array with capacity for at
                          least max_results entries.
    \param   max_results Capacity of out_array, in entries.
    \param   out_count   Non-NULL; receives the number of entries written
                          to out_array.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_ARG (bad args);
             SAS_ERR_INVALID_STATE (not started); SAS_WIFI_ERR_SCAN_FAILED
             (driver failure); SAS_ERR_TIMEOUT (another scan already in
             progress).
*/
sas_status_t sas_wifi_scan(sas_wifi_scan_result_t *out_array, uint8_t max_results,
                           uint8_t *out_count);

/*!
    \brief   Get the current connection state.
    \return  Current sas_wifi_state_t.
*/
sas_wifi_state_t sas_wifi_get_state(void);
