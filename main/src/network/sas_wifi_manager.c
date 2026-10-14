//=============================================================================
/*!
    \file    sas_wifi_manager.c
    \brief   Implementation of the Wi-Fi Manager module.
*/
//=============================================================================

#include "sas_wifi_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "../core/sas_config.h"
#include "../core/sas_event_bus.h"
#include "../core/sas_logger.h"

static const char *TAG = "sas_wifi";

/*! \brief Bounded wait for s_state_lock; critical sections are short field reads/writes. */
#define SAS_WIFI_LOCK_WAIT_TICKS pdMS_TO_TICKS(50)
/*! \brief Bounded wait to serialize concurrent sas_wifi_scan() callers. */
#define SAS_WIFI_SCAN_LOCK_WAIT_TICKS pdMS_TO_TICKS(100)

/*!
    \brief   Maximum number of networks kept from one scan.
*/
#define SAS_WIFI_SCAN_MAX_RECORDS (20U)

/*! \brief Ceiling on the exponential backoff delay, regardless of retry count. */
#define SAS_WIFI_MAX_BACKOFF_MS (60000U)
/*! \brief Caps the retry-count left-shift so it can never itself overflow uint32_t. */
#define SAS_WIFI_MAX_BACKOFF_SHIFT (10U)

#define SAS_WIFI_AP_FALLBACK_CHANNEL (1U) ///< Channel of the fallback access point.
/*! \brief Maximum simultaneous clients on the fallback access point. */
#define SAS_WIFI_AP_FALLBACK_MAX_CONN (4U)

/*! \brief s_connect_event_group bit set by wifi_event_handler() on IP_EVENT_STA_GOT_IP. */
#define SAS_WIFI_CONNECTED_BIT (1U << 0)

static bool s_initialized = false;
static bool s_started     = false;

static sas_wifi_state_t s_state = SAS_WIFI_STATE_IDLE;
static uint32_t s_retry_count   = 0U;
static sas_wifi_credentials_t s_creds;
static char s_hostname[SAS_WIFI_HOSTNAME_MAX_LEN]; ///< mDNS host name; set by load_config().

static bool s_ap_always_on = false; ///< wifi.ap.always_on: keep the AP up beside a joined network.
static char s_ap_password[SAS_WIFI_AP_PASSWORD_MAX_LEN]; ///< wifi.ap.password; empty: open AP.

static uint32_t s_max_retry_count    = 5U;
static uint32_t s_initial_backoff_ms = 1000U;
static uint32_t s_connect_timeout_ms = 10000U;

// Guards s_state, s_retry_count and s_creds, which are shared between the
// caller's task and the ESP-IDF event loop task.
static SemaphoreHandle_t s_state_lock = NULL;
static StaticSemaphore_t s_state_lock_storage;

static SemaphoreHandle_t s_scan_lock = NULL;
static StaticSemaphore_t s_scan_lock_storage;
static wifi_ap_record_t s_scan_records[SAS_WIFI_SCAN_MAX_RECORDS];

static StaticEventGroup_t s_connect_event_group_storage;
static EventGroupHandle_t s_connect_event_group = NULL;

static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap  = NULL;

static esp_event_handler_instance_t s_wifi_event_handler_instance = NULL;
static esp_event_handler_instance_t s_ip_event_handler_instance   = NULL;

static esp_timer_handle_t s_retry_timer = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data);
static void retry_timer_callback(void *arg);
static void set_state_and_publish(sas_wifi_state_t new_state);
static void handle_sta_disconnected(void);
static void build_fallback_ssid(char *out, size_t out_len);
static void build_default_hostname(char *out, size_t out_len);
static bool normalize_hostname(const char *in, char *out, size_t out_len);
static void start_mdns(void);
static void build_ap_config(wifi_config_t *out);
static bool ap_runs_beside_station(void);
static void switch_to_ap_fallback(void);
static void begin_sta_connect(void);
static void issue_connect_or_handle_failure(void);
static void load_config(void);
static uint32_t compute_backoff_ms(uint32_t retry_count);

/*!
    \brief   Initialize the Wi-Fi Manager module.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_STATE if already initialized.
*/
sas_status_t sas_wifi_init(void)
{
    // The fallback access point is deliberately open, so the device can always
    // be reached to set up real credentials.

    if (s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    // NVS, netif and the default event loop are process-wide and must exist
    // before esp_wifi_init(). They are created here and left running by
    // sas_wifi_deinit(), because other modules need them too.
    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    configASSERT(err == ESP_OK);

    err = esp_netif_init();
    configASSERT((err == ESP_OK) || (err == ESP_ERR_INVALID_STATE));

    err = esp_event_loop_create_default();
    configASSERT((err == ESP_OK) || (err == ESP_ERR_INVALID_STATE));

    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap  = esp_netif_create_default_wifi_ap();
    configASSERT((s_netif_sta != NULL) && (s_netif_ap != NULL));

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    configASSERT(esp_wifi_init(&wifi_init_cfg) == ESP_OK);

    configASSERT(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                     &wifi_event_handler, NULL,
                                                     &s_wifi_event_handler_instance) == ESP_OK);
    configASSERT(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                     &wifi_event_handler, NULL,
                                                     &s_ip_event_handler_instance) == ESP_OK);

    s_state_lock = xSemaphoreCreateMutexStatic(&s_state_lock_storage);
    configASSERT(s_state_lock != NULL);

    s_scan_lock = xSemaphoreCreateMutexStatic(&s_scan_lock_storage);
    configASSERT(s_scan_lock != NULL);

    s_connect_event_group = xEventGroupCreateStatic(&s_connect_event_group_storage);
    configASSERT(s_connect_event_group != NULL);

    const esp_timer_create_args_t retry_timer_args = {
        .callback = &retry_timer_callback,
        .arg      = NULL,
        .name     = "sas_wifi_retry",
    };
    configASSERT(esp_timer_create(&retry_timer_args, &s_retry_timer) == ESP_OK);

    load_config();
    start_mdns();

    s_state       = SAS_WIFI_STATE_IDLE;
    s_retry_count = 0U;
    s_started     = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Start active Wi-Fi operation (station driver + state machine).
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_STATE if not initialized or already
             started.
*/
sas_status_t sas_wifi_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    s_started = true;

    if (s_creds.ssid[0] == '\0')
    {
        // No stored credentials: go straight to the fallback access point
        // (SAS-WIFI-FR-002).
        set_state_and_publish(SAS_WIFI_STATE_AP_FALLBACK);
        switch_to_ap_fallback();
    }
    else
    {
        // sas_wifi_connect() stops, configures and restarts the driver itself.
        (void)sas_wifi_connect(&s_creds);
    }

    return SAS_STATUS_OK;
}

/*!
    \brief   Stop active Wi-Fi operation without releasing resources acquired
             by sas_wifi_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_wifi_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    (void)esp_timer_stop(s_retry_timer); // fails harmlessly when the timer is not running
    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();

    set_state_and_publish(SAS_WIFI_STATE_IDLE);
    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)
    {
        s_retry_count = 0U;
        xSemaphoreGive(s_state_lock);
    }

    s_started = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Release all resources acquired by sas_wifi_init().
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_STATE if not initialized or still
             started (call sas_wifi_stop() first).
*/
sas_status_t sas_wifi_deinit(void)
{
    // NVS, netif and the event loop are left running for other modules.

    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_retry_timer != NULL)
    {
        (void)esp_timer_delete(s_retry_timer);
        s_retry_timer = NULL;
    }
    if (s_wifi_event_handler_instance != NULL)
    {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                    s_wifi_event_handler_instance);
        s_wifi_event_handler_instance = NULL;
    }
    if (s_ip_event_handler_instance != NULL)
    {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    s_ip_event_handler_instance);
        s_ip_event_handler_instance = NULL;
    }
    (void)esp_wifi_deinit();
    if (s_netif_sta != NULL)
    {
        esp_netif_destroy(s_netif_sta);
        s_netif_sta = NULL;
    }
    if (s_netif_ap != NULL)
    {
        esp_netif_destroy(s_netif_ap);
        s_netif_ap = NULL;
    }
    if (s_connect_event_group != NULL)
    {
        vEventGroupDelete(s_connect_event_group);
        s_connect_event_group = NULL;
    }
    if (s_scan_lock != NULL)
    {
        vSemaphoreDelete(s_scan_lock);
        s_scan_lock = NULL;
    }
    if (s_state_lock != NULL)
    {
        vSemaphoreDelete(s_state_lock);
        s_state_lock = NULL;
    }

    (void)memset(&s_creds, 0, sizeof(s_creds));
    s_state       = SAS_WIFI_STATE_IDLE;
    s_retry_count = 0U;
    s_initialized = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Connect using creds, driving IDLE/AP_FALLBACK into CONNECTING.
    \param[in] creds  Non-NULL credentials to connect with.
    \return  SAS_STATUS_OK if accepted; SAS_ERR_INVALID_ARG (creds NULL);
             SAS_ERR_INVALID_STATE (not started); SAS_WIFI_ERR_TIMEOUT if the
             connection didn't complete in time (it keeps retrying in the
             background regardless).
*/
sas_status_t sas_wifi_connect(const sas_wifi_credentials_t *creds)
{
    // The credentials are always saved first (SAS-WIFI-FR-006).

    if (creds == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    sas_config_value_t val;
    val.type = SAS_CFG_TYPE_STRING;
    (void)strncpy(val.value.str_val, creds->ssid, SAS_CFG_MAX_STRING_LEN - 1U);
    val.value.str_val[SAS_CFG_MAX_STRING_LEN - 1U] = '\0';
    sas_status_t persist_status                    = sas_config_set("wifi.sta.ssid", &val);
    if (persist_status != SAS_STATUS_OK)
    {
        sas_log_warn("failed to persist wifi.sta.ssid (0x%x)", (unsigned int)persist_status);
    }
    (void)strncpy(val.value.str_val, creds->password, SAS_CFG_MAX_STRING_LEN - 1U);
    val.value.str_val[SAS_CFG_MAX_STRING_LEN - 1U] = '\0';
    persist_status                                 = sas_config_set("wifi.sta.password", &val);
    if (persist_status != SAS_STATUS_OK)
    {
        sas_log_warn("failed to persist wifi.sta.password (0x%x)", (unsigned int)persist_status);
    }

    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)
    {
        s_creds       = *creds;
        s_retry_count = 0U;
        xSemaphoreGive(s_state_lock);
    }

    xEventGroupClearBits(s_connect_event_group, SAS_WIFI_CONNECTED_BIT);
    set_state_and_publish(SAS_WIFI_STATE_CONNECTING);
    begin_sta_connect();

    EventBits_t bits = xEventGroupWaitBits(s_connect_event_group, SAS_WIFI_CONNECTED_BIT, pdFALSE,
                                           pdFALSE, pdMS_TO_TICKS(s_connect_timeout_ms));
    if ((bits & SAS_WIFI_CONNECTED_BIT) != 0U)
    {
        return SAS_STATUS_OK;
    }
    return SAS_WIFI_ERR_TIMEOUT; // the connection attempt continues in the background
}

/*!
    \brief   Scan for nearby access points; blocks the caller for the scan's
             duration.
    \param[out] out_array    Non-NULL destination array with capacity for at
                             least max_results entries.
    \param[in]  max_results  Capacity of out_array, in entries.
    \param[out] out_count    Non-NULL; receives the number of entries written
                             to out_array.
    \return  SAS_STATUS_OK; SAS_ERR_INVALID_ARG (bad args);
             SAS_ERR_INVALID_STATE (not started); SAS_WIFI_ERR_SCAN_FAILED
             (driver failure); SAS_ERR_TIMEOUT (another scan already in
             progress).
*/
sas_status_t sas_wifi_scan(sas_wifi_scan_result_t *out_array, uint8_t max_results,
                           uint8_t *out_count)
{
    if ((out_array == NULL) || (out_count == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_scan_lock, SAS_WIFI_SCAN_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK)
    {
        *out_count = 0U;
        status     = SAS_WIFI_ERR_SCAN_FAILED;
    }
    else
    {
        uint16_t ap_count = SAS_WIFI_SCAN_MAX_RECORDS;
        (void)esp_wifi_scan_get_ap_records(&ap_count, s_scan_records);
        if (ap_count > max_results)
        {
            ap_count = max_results;
        }

        for (uint16_t i = 0U; i < ap_count; i++)
        {
            (void)strncpy(out_array[i].ssid, (const char *)s_scan_records[i].ssid,
                          sizeof(out_array[i].ssid) - 1U);
            out_array[i].ssid[sizeof(out_array[i].ssid) - 1U] = '\0';
            out_array[i].rssi                                 = s_scan_records[i].rssi;
            out_array[i].auth_mode                            = (uint8_t)s_scan_records[i].authmode;
        }
        *out_count = (uint8_t)ap_count;
    }

    xSemaphoreGive(s_scan_lock);
    return status;
}

/*!
    \brief   Get the current connection state.
    \return  Current sas_wifi_state_t.
*/
sas_wifi_state_t sas_wifi_get_state(void)
{
    sas_wifi_state_t state = SAS_WIFI_STATE_IDLE;

    if (s_initialized && (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE))
    {
        state = s_state;
        xSemaphoreGive(s_state_lock);
    }
    return state;
}

/*!
    \brief   Get the connection state together with the SSID and RSSI it
             applies to.
    \param[out] out  Non-NULL destination. ssid is the configured station SSID
                     (empty if none); rssi is the current link RSSI in dBm, or
                     0 when not connected; ip is the address the access point's
                     DHCP server assigned; hostname is the mDNS name the device
                     can be reached by regardless of that address.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out is NULL.
*/
sas_status_t sas_wifi_get_status(sas_wifi_status_t *out)
{
    if (out == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    (void)memset(out, 0, sizeof(*out));
    if (s_initialized && (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE))
    {
        out->state = s_state;
        (void)strncpy(out->ssid, s_creds.ssid, sizeof(out->ssid) - 1U);
        (void)strncpy(out->hostname, s_hostname, sizeof(out->hostname) - 1U);
        out->ap_always_on = s_ap_always_on;
        out->ap_secured   = (s_ap_password[0] != '\0');
        xSemaphoreGive(s_state_lock);
    }
    build_fallback_ssid(out->ap_ssid, sizeof(out->ap_ssid));

    wifi_mode_t mode = WIFI_MODE_NULL;
    if ((out->state != SAS_WIFI_STATE_IDLE) && (esp_wifi_get_mode(&mode) == ESP_OK))
    {
        out->ap_active = (mode == WIFI_MODE_AP) || (mode == WIFI_MODE_APSTA);
    }

    if (out->state == SAS_WIFI_STATE_CONNECTED)
    {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
        {
            out->rssi = ap_info.rssi;
        }

        esp_netif_ip_info_t ip_info;
        if ((s_netif_sta != NULL) && (esp_netif_get_ip_info(s_netif_sta, &ip_info) == ESP_OK))
        {
            (void)snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip_info.ip));
        }
    }
    return SAS_STATUS_OK;
}

/*!
    \brief   Disconnect from the current network and forget it: the stored
             SSID and passphrase are erased and the device goes back to its
             fallback access point, as on a first boot.
    \note    Blocks while the driver restarts, and drops every connection made
             over the forgotten network, including the caller's if it arrived
             that way.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_wifi_forget(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    (void)esp_timer_stop(s_retry_timer); // no pending reconnect to the forgotten network

    sas_config_value_t val;
    val.type             = SAS_CFG_TYPE_STRING;
    val.value.str_val[0] = '\0';
    if ((sas_config_set("wifi.sta.ssid", &val) != SAS_STATUS_OK) ||
        (sas_config_set("wifi.sta.password", &val) != SAS_STATUS_OK))
    {
        sas_log_warn("stored Wi-Fi credentials could not be erased from the configuration");
    }

    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)
    {
        (void)memset(&s_creds, 0, sizeof(s_creds));
        s_retry_count = 0U;
        xSemaphoreGive(s_state_lock);
    }

    // State first: the disconnect event raised by stopping the driver is then
    // ignored instead of starting a reconnect.
    set_state_and_publish(SAS_WIFI_STATE_AP_FALLBACK);
    switch_to_ap_fallback();

    sas_log_info("Wi-Fi network forgotten; fallback access point started");
    return SAS_STATUS_OK;
}

/*!
    \brief   Store the access point settings. Nothing changes on air until
             sas_wifi_apply_ap_config() is called, so a caller connected
             through the access point can still be answered first.
    \param[in] always_on  true to keep the access point up after a network is
                          joined; false to run it only as the fallback when
                          there is no network.
    \param[in] password   NULL to keep the stored password; "" to remove it
                          (open access point); otherwise the new WPA2 password
                          of 8 to 63 characters.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if the password's
             length is out of range, or always_on is requested without a
             password (an open access point is not left running beside a
             joined network); SAS_ERR_INVALID_STATE if not initialized;
             SAS_ERR_TIMEOUT if the lock is busy.
*/
sas_status_t sas_wifi_set_ap_config(bool always_on, const char *password)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (password != NULL)
    {
        size_t len = strlen(password);
        if ((len != 0U) && ((len < 8U) || (len >= SAS_WIFI_AP_PASSWORD_MAX_LEN)))
        {
            return SAS_ERR_INVALID_ARG;
        }
    }

    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status      = SAS_STATUS_OK;
    const char *new_password = (password != NULL) ? password : s_ap_password;
    if (always_on && (new_password[0] == '\0'))
    {
        status = SAS_ERR_INVALID_ARG;
    }
    else
    {
        if (password != NULL)
        {
            (void)strcpy(s_ap_password, password);
        }
        s_ap_always_on = always_on;
    }

    sas_config_value_t password_val;
    password_val.type = SAS_CFG_TYPE_STRING;
    (void)strcpy(password_val.value.str_val, s_ap_password);
    xSemaphoreGive(s_state_lock);

    if (status == SAS_STATUS_OK)
    {
        sas_config_value_t always_on_val;
        always_on_val.type           = SAS_CFG_TYPE_BOOL;
        always_on_val.value.bool_val = always_on;
        if ((sas_config_set("wifi.ap.password", &password_val) != SAS_STATUS_OK) ||
            (sas_config_set("wifi.ap.always_on", &always_on_val) != SAS_STATUS_OK))
        {
            sas_log_warn("access point settings applied but could not be persisted");
        }
    }

    (void)memset(&password_val, 0, sizeof(password_val)); // held the password
    return status;
}

/*!
    \brief   Bring the running access point in line with the stored settings:
             start or stop it beside a joined network, and apply a changed
             password.
    \note    Clients on the access point are disconnected when its password
             changes or it is switched off.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_wifi_apply_ap_config(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    wifi_config_t ap_config;
    build_ap_config(&ap_config);

    sas_wifi_state_t state = sas_wifi_get_state();
    if (state == SAS_WIFI_STATE_AP_FALLBACK)
    {
        // The fallback access point is always up; only its security can change.
        (void)esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    else if (state != SAS_WIFI_STATE_IDLE)
    {
        // On, or joining, a network: the mode can change without restarting
        // the driver, so the station link is kept.
        if (ap_runs_beside_station())
        {
            (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
            (void)esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        }
        else
        {
            (void)esp_wifi_set_mode(WIFI_MODE_STA);
        }
    }
    else
    {
        // idle: the settings take effect at the next start
    }

    (void)memset(&ap_config, 0, sizeof(ap_config)); // held the password
    sas_log_info("access point: %s, %s", ap_runs_beside_station() ? "always on" : "fallback only",
                 (s_ap_password[0] != '\0') ? "WPA2" : "open");
    return SAS_STATUS_OK;
}

/*!
    \brief   Change the mDNS host name the device answers to
             ("<hostname>.local"). Takes effect at once and is persisted as the
             wifi.hostname configuration key; with that key unset the name
             is SAS_WIFI_DEFAULT_HOSTNAME.
    \param[in] hostname  Non-NULL name of 1 to 31 characters from a-z, 0-9,
                         '-' and '_', not starting or ending with '-'.
                         Upper-case letters are folded to lower case.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if hostname is NULL
             or not a valid name; SAS_ERR_INVALID_STATE if not initialized or
             the mDNS responder rejected the name.
*/
sas_status_t sas_wifi_set_hostname(const char *hostname)
{
    char normalized[SAS_WIFI_HOSTNAME_MAX_LEN];
    if ((hostname == NULL) || !normalize_hostname(hostname, normalized, sizeof(normalized)))
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    // Applied first: the responder re-announces under the new name right away.
    if (mdns_hostname_set(normalized) != ESP_OK)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)
    {
        (void)strcpy(s_hostname, normalized);
        xSemaphoreGive(s_state_lock);
    }

    sas_config_value_t val;
    val.type = SAS_CFG_TYPE_STRING;
    (void)strcpy(val.value.str_val, normalized);
    sas_status_t persist_status = sas_config_set("wifi.hostname", &val);
    if (persist_status != SAS_STATUS_OK)
    {
        sas_log_warn("failed to persist wifi.hostname (0x%x)", (unsigned int)persist_status);
    }

    sas_log_info("mDNS name: %s.local", normalized);
    return SAS_STATUS_OK;
}

/*!
    \brief   Load the five wifi.* keys from Configuration Manager into module
             state. Caller: sas_wifi_init(), before s_initialized is set.
*/
static void load_config(void)
{
    sas_config_value_t val;
    sas_config_value_t def;

    def.type = SAS_CFG_TYPE_STRING;
    (void)memset(def.value.str_val, 0, sizeof(def.value.str_val));
    (void)sas_config_get("wifi.sta.ssid", &val, &def);
    (void)strncpy(s_creds.ssid, val.value.str_val, sizeof(s_creds.ssid) - 1U);
    s_creds.ssid[sizeof(s_creds.ssid) - 1U] = '\0';

    (void)sas_config_get("wifi.sta.password", &val, &def);
    (void)strncpy(s_creds.password, val.value.str_val, sizeof(s_creds.password) - 1U);
    s_creds.password[sizeof(s_creds.password) - 1U] = '\0';

    // Access point: the password protects it, always_on keeps it up beside a
    // joined network. A stored password that is not a valid WPA2 one is ignored.
    (void)sas_config_get("wifi.ap.password", &val, &def);
    size_t ap_password_len = strlen(val.value.str_val);
    if ((ap_password_len >= 8U) && (ap_password_len < sizeof(s_ap_password)))
    {
        (void)strcpy(s_ap_password, val.value.str_val);
    }

    sas_config_value_t bool_def;
    bool_def.type           = SAS_CFG_TYPE_BOOL;
    bool_def.value.bool_val = false;
    (void)sas_config_get("wifi.ap.always_on", &val, &bool_def);
    s_ap_always_on = (val.type == SAS_CFG_TYPE_BOOL) && val.value.bool_val;

    // wifi.hostname: a stored name wins; unset or unusable falls back to the default.
    (void)sas_config_get("wifi.hostname", &val, &def);
    if (!normalize_hostname(val.value.str_val, s_hostname, sizeof(s_hostname)))
    {
        build_default_hostname(s_hostname, sizeof(s_hostname));
    }

    def.type          = SAS_CFG_TYPE_INT;
    def.value.int_val = (int32_t)s_max_retry_count;
    (void)sas_config_get("wifi.retry.max_count", &val, &def);
    s_max_retry_count = (uint32_t)val.value.int_val;

    def.value.int_val = (int32_t)s_initial_backoff_ms;
    (void)sas_config_get("wifi.retry.initial_backoff_ms", &val, &def);
    s_initial_backoff_ms = (uint32_t)val.value.int_val;

    def.value.int_val = (int32_t)s_connect_timeout_ms;
    (void)sas_config_get("wifi.connect.timeout_ms", &val, &def);
    s_connect_timeout_ms = (uint32_t)val.value.int_val;
}

/*!
    \brief   Update s_state under lock, then publish outside the lock (never
             call into Event Bus while holding s_state_lock).
*/
static void set_state_and_publish(sas_wifi_state_t new_state)
{
    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)
    {
        s_state = new_state;
        xSemaphoreGive(s_state_lock);
    }

    sas_status_t publish_status =
        sas_event_bus_publish(SAS_EVENT_WIFI_STATE_CHANGED, &new_state, sizeof(new_state));
    if (publish_status != SAS_STATUS_OK)
    {
        sas_log_warn("SAS_EVENT_WIFI_STATE_CHANGED publish failed (0x%x)",
                     (unsigned int)publish_status);
    }
}

/*!
    \brief   Apply s_creds to the STA driver config and (re)start the driver.
*/
static void begin_sta_connect(void)
{
    // esp_wifi_start() is asynchronous, so the connect itself is issued from
    // the WIFI_EVENT_STA_START handler once the driver is up.

    wifi_config_t wifi_config;
    (void)memset(&wifi_config, 0, sizeof(wifi_config));
    (void)strncpy((char *)wifi_config.sta.ssid, s_creds.ssid, sizeof(wifi_config.sta.ssid) - 1U);
    (void)strncpy((char *)wifi_config.sta.password, s_creds.password,
                  sizeof(wifi_config.sta.password) - 1U);

    (void)esp_wifi_stop(); // ensures a clean switch out of fallback mode
    if (ap_runs_beside_station())
    {
        // Always-on access point: it follows the joined network's channel.
        wifi_config_t ap_config;
        build_ap_config(&ap_config);
        (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
        (void)esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    else
    {
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
    }
    (void)esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    (void)esp_wifi_start();
}

/*!
    \brief   Issue esp_wifi_connect(); if the driver rejects it immediately,
             treat that like a lost link.
*/
static void issue_connect_or_handle_failure(void)
{
    if (esp_wifi_connect() != ESP_OK)
    {
        handle_sta_disconnected();
    }
}

/*!
    \brief   WIFI_EVENT_STA_DISCONNECTED handling: retry with backoff, or fall
             back to AP once wifi.retry.max_count is exhausted.
*/
static void handle_sta_disconnected(void)
{
    sas_wifi_state_t current;
    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)
    {
        current = s_state;
        xSemaphoreGive(s_state_lock);
    }
    else
    {
        current = s_state;
    }

    if ((current == SAS_WIFI_STATE_IDLE) || (current == SAS_WIFI_STATE_AP_FALLBACK))
    {
        return; // no connection attempt in progress
    }

    uint32_t retry_count = 0U;
    bool exhausted       = false;
    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)
    {
        if (s_retry_count < s_max_retry_count)
        {
            s_retry_count++;
            retry_count = s_retry_count;
        }
        else
        {
            exhausted = true;
        }
        xSemaphoreGive(s_state_lock);
    }

    if (exhausted)
    {
        set_state_and_publish(SAS_WIFI_STATE_AP_FALLBACK); // published once (AC-WIFI-2)
        switch_to_ap_fallback();
        return;
    }

    set_state_and_publish(SAS_WIFI_STATE_RECONNECTING);
    uint32_t backoff_ms = compute_backoff_ms(retry_count);
    (void)esp_timer_stop(s_retry_timer);
    (void)esp_timer_start_once(s_retry_timer, (uint64_t)backoff_ms * 1000ULL);
}

/*!
    \brief   esp_timer one-shot callback: re-issue the STA connect attempt
             while remaining in RECONNECTING.
*/
static void retry_timer_callback(void *arg)
{
    // The driver is already started here, so WIFI_EVENT_STA_START will not
    // fire again; connect directly.

    (void)arg;
    issue_connect_or_handle_failure();
}

/*!
    \brief   Exponential backoff, in ms, clamped to SAS_WIFI_MAX_BACKOFF_MS.
*/
static uint32_t compute_backoff_ms(uint32_t retry_count)
{
    uint32_t shift = (retry_count > 0U) ? (retry_count - 1U) : 0U;
    if (shift > SAS_WIFI_MAX_BACKOFF_SHIFT)
    {
        shift = SAS_WIFI_MAX_BACKOFF_SHIFT;
    }

    uint64_t backoff = (uint64_t)s_initial_backoff_ms << shift;
    if (backoff > SAS_WIFI_MAX_BACKOFF_MS)
    {
        backoff = SAS_WIFI_MAX_BACKOFF_MS;
    }
    return (uint32_t)backoff;
}

/*!
    \brief   Copy the fallback access point's SSID, SAS_WIFI_FALLBACK_SSID, into
             out.
*/
static void build_fallback_ssid(char *out, size_t out_len)
{
    (void)snprintf(out, out_len, "%s", SAS_WIFI_FALLBACK_SSID);
}

/*!
    \brief   Copy the default mDNS host name, SAS_WIFI_DEFAULT_HOSTNAME, into
             out. Used when wifi.hostname is unset or unusable.
*/
static void build_default_hostname(char *out, size_t out_len)
{
    (void)snprintf(out, out_len, "%s", SAS_WIFI_DEFAULT_HOSTNAME);
}

/*!
    \brief   Validate in as a single mDNS label and copy it, lower-cased, into
             out: 1 to out_len - 1 characters from a-z, 0-9, '-' and '_', not
             starting or ending with '-'.
    \note    '_' is not a legal host name character under RFC 952/1123, but
             mDNS responders and the common browsers accept it, and the
             default name uses it.
*/
static bool normalize_hostname(const char *in, char *out, size_t out_len)
{
    size_t len = strlen(in);
    if ((len == 0U) || (len >= out_len) || (in[0] == '-') || (in[len - 1U] == '-'))
    {
        return false;
    }

    for (size_t i = 0U; i < len; i++)
    {
        char c = in[i];
        if ((c >= 'A') && (c <= 'Z'))
        {
            c = (char)(c - 'A' + 'a');
        }
        if (!(((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '-') || (c == '_')))
        {
            return false;
        }
        out[i] = c;
    }
    out[len] = '\0';
    return true;
}

/*!
    \brief   Advertise the device over mDNS as "<hostname>.local" with an
             _http._tcp service, so it can be found on a network whose DHCP
             server (and therefore the device's address) the operator does not
             control.
*/
static void start_mdns(void)
{
    // Not fatal if it fails: the device is still reachable by IP.

    const char *hostname = s_hostname; // set by load_config(), which runs first

    if ((mdns_init() != ESP_OK) || (mdns_hostname_set(hostname) != ESP_OK))
    {
        sas_log_warn("mDNS unavailable; reach the device by IP address");
        return;
    }
    (void)mdns_instance_name_set("SAS Dashboard");
    (void)mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    sas_log_info("mDNS name: %s.local", hostname);
}

/*!
    \brief   Fill out with the access point's configuration: the fixed SSID, and
             WPA2 with wifi.ap.password when one is stored, otherwise open.
*/
static void build_ap_config(wifi_config_t *out)
{
    char ap_ssid[33]; // same size as sas_wifi_credentials_t.ssid
    build_fallback_ssid(ap_ssid, sizeof(ap_ssid));

    (void)memset(out, 0, sizeof(*out));
    (void)strncpy((char *)out->ap.ssid, ap_ssid, sizeof(out->ap.ssid) - 1U);
    out->ap.ssid_len       = (uint8_t)strlen(ap_ssid);
    out->ap.channel        = SAS_WIFI_AP_FALLBACK_CHANNEL;
    out->ap.max_connection = SAS_WIFI_AP_FALLBACK_MAX_CONN;

    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)
    {
        (void)strncpy((char *)out->ap.password, s_ap_password, sizeof(out->ap.password) - 1U);
        xSemaphoreGive(s_state_lock);
    }
    out->ap.authmode = (out->ap.password[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
}

/*!
    \brief   True if the access point should stay up while the device is on a
             network: always-on is set and a password protects it. An open
             access point is never kept up beside a joined network.
*/
static bool ap_runs_beside_station(void)
{
    bool keep = false;
    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)
    {
        keep = s_ap_always_on && (s_ap_password[0] != '\0');
        xSemaphoreGive(s_state_lock);
    }
    return keep;
}

/*!
    \brief   Stop the STA driver, reconfigure as an open fallback AP, and start
             it. See sas_wifi_init()'s doc comment for why the fallback AP is
             open rather than password-protected.
*/
static void switch_to_ap_fallback(void)
{
    wifi_config_t ap_config;
    build_ap_config(&ap_config);

    // AP+STA, not AP only: scanning needs the station interface, and a user on
    // the fallback access point must be able to scan for networks. The station
    // stays idle; a connect is only issued while a connection attempt is in
    // progress.
    (void)esp_wifi_stop();
    (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
    (void)esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    (void)esp_wifi_start();
}

/*!
    \brief   WIFI_EVENT / IP_EVENT dispatcher registered in sas_wifi_init();
             runs on the default ESP-IDF event loop task.
*/
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_START)
        {
            // Also fires when the fallback AP comes up in AP+STA mode, where there is nothing
            // to connect to.
            sas_wifi_state_t state = sas_wifi_get_state();
            if ((state == SAS_WIFI_STATE_CONNECTING) || (state == SAS_WIFI_STATE_RECONNECTING))
            {
                issue_connect_or_handle_failure();
            }
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            handle_sta_disconnected();
        }
        else
        {
            // other WIFI_EVENT ids are not handled by this module
        }
    }
    else if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)
            {
                s_retry_count = 0U;
                xSemaphoreGive(s_state_lock);
            }
            set_state_and_publish(SAS_WIFI_STATE_CONNECTED);
            xEventGroupSetBits(s_connect_event_group, SAS_WIFI_CONNECTED_BIT);
        }
    }
    else
    {
        // no other event bases are registered
    }
}
