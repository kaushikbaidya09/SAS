//=============================================================================
/*!
    \file    sas_wifi_manager.h
    \brief   Public interface for the Wi-Fi Manager module.
*/
//=============================================================================

// Manages the Wi-Fi connection: connects as a station with the stored
// credentials, reconnects when the link drops, and falls back to its own
// access point when it cannot connect.

#ifndef SAS_WIFI_MANAGER_H
#define SAS_WIFI_MANAGER_H

#include <stdint.h>

#include "../core/sas_status.h"

/*!
    \brief Connection state machine:

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

/*! \brief One access point found by sas_wifi_scan(). */
typedef struct
{
    char ssid[33];     ///< Null-terminated SSID, up to 32 characters.
    int8_t rssi;       ///< Received signal strength, in dBm.
    uint8_t auth_mode; ///< Authentication mode, per wifi_auth_mode_t.
} sas_wifi_scan_result_t;

/*! \brief Size of an mDNS host name buffer: up to 31 characters plus the NUL. Defined ahead of the
    Public constants section because sas_wifi_status_t below sizes a member with it. */
#define SAS_WIFI_HOSTNAME_MAX_LEN (32U)

/*! \brief Connection status snapshot returned by sas_wifi_get_status(). */
typedef struct
{
    sas_wifi_state_t state; ///< Current connection state.
    char ssid[33];          ///< Configured station SSID; empty if none.
    int8_t rssi;            ///< Link RSSI in dBm; 0 when not connected.
    char ip[16];            ///< Station IPv4 address, dotted; empty when not connected.
    char hostname[SAS_WIFI_HOSTNAME_MAX_LEN]; ///< mDNS host name; the device answers at
                                              ///< "<hostname>.local".
} sas_wifi_status_t;

/*! \brief Station credentials supplied to sas_wifi_connect(). */
typedef struct
{
    char ssid[33];     ///< Null-terminated SSID, up to 32 characters.
    char password[65]; ///< Null-terminated passphrase, up to 64 characters.
} sas_wifi_credentials_t;

/*! \brief Custom sas_status_t base for this module. */
#define SAS_WIFI_ERR_BASE (0x8600U)
/*! \brief sas_wifi_connect() returned after wifi.connect.timeout_ms elapsed without
    a completed connection; the module continues retrying asynchronously
    per the state machine above. */
#define SAS_WIFI_ERR_TIMEOUT (SAS_WIFI_ERR_BASE + 1U)
/*! \brief sas_wifi_scan() failed to start or complete a scan at the driver level. */
#define SAS_WIFI_ERR_SCAN_FAILED (SAS_WIFI_ERR_BASE + 2U)

sas_status_t sas_wifi_init(void);

sas_status_t sas_wifi_start(void);

sas_status_t sas_wifi_stop(void);

sas_status_t sas_wifi_deinit(void);

sas_status_t sas_wifi_connect(const sas_wifi_credentials_t *creds);

sas_status_t sas_wifi_scan(sas_wifi_scan_result_t *out_array, uint8_t max_results,
                           uint8_t *out_count);

sas_wifi_state_t sas_wifi_get_state(void);

sas_status_t sas_wifi_get_status(sas_wifi_status_t *out);

sas_status_t sas_wifi_set_hostname(const char *hostname);

#endif /* SAS_WIFI_MANAGER_H */
