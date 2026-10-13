/*!
    \file    sas_wifi_manager.c
    \brief   Implementation of the Wi-Fi Manager module.
*/

/* 1. Includes */
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

/* 2. Private macros */
#define SAS_WIFI_LOCK_WAIT_TICKS                                                                   \
    pdMS_TO_TICKS(                                                                                 \
        50) ///< Bounded wait for s_state_lock; critical sections are short field reads/writes.
#define SAS_WIFI_SCAN_LOCK_WAIT_TICKS                                                              \
    pdMS_TO_TICKS(100) ///< Bounded wait to serialize concurrent sas_wifi_scan() callers.

#define SAS_WIFI_SCAN_MAX_RECORDS                                                                  \
    (20U) ///< Internal esp_wifi scan buffer capacity; placeholder, to be tuned during
          ///< implementation.

#define SAS_WIFI_MAX_BACKOFF_MS                                                                    \
    (60000U) ///< Ceiling on the exponential backoff delay, regardless of retry count.
#define SAS_WIFI_MAX_BACKOFF_SHIFT                                                                 \
    (10U) ///< Caps the retry-count left-shift so it can never itself overflow uint32_t.

#define SAS_WIFI_AP_FALLBACK_CHANNEL (1U) ///< Placeholder fixed channel for the fallback AP.
#define SAS_WIFI_AP_FALLBACK_MAX_CONN                                                              \
    (4U) ///< Placeholder max simultaneous clients on the fallback AP.

#define SAS_WIFI_CONNECTED_BIT                                                                     \
    (1U << 0) ///< s_connect_event_group bit set by wifi_event_handler() on IP_EVENT_STA_GOT_IP.

/* 3. Private typedefs */
/* (none) */

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;

static sas_wifi_state_t s_state = SAS_WIFI_STATE_IDLE;
static uint32_t s_retry_count = 0U;
static sas_wifi_credentials_t s_creds;
static char s_hostname[SAS_WIFI_HOSTNAME_MAX_LEN]; ///< mDNS host name; set by load_config().

static uint32_t s_max_retry_count = 5U;
static uint32_t s_initial_backoff_ms = 1000U;
static uint32_t s_connect_timeout_ms = 10000U;

/* Guards s_state/s_retry_count/s_creds, shared between the caller's task
 * (sas_wifi_connect()/stop()) and the ESP-IDF event loop task
 * (wifi_event_handler(), retry_timer_callback()). */
static SemaphoreHandle_t s_state_lock = NULL;
static StaticSemaphore_t s_state_lock_storage;

static SemaphoreHandle_t s_scan_lock = NULL;
static StaticSemaphore_t s_scan_lock_storage;
static wifi_ap_record_t s_scan_records[SAS_WIFI_SCAN_MAX_RECORDS];

static StaticEventGroup_t s_connect_event_group_storage;
static EventGroupHandle_t s_connect_event_group = NULL;

static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;

static esp_event_handler_instance_t s_wifi_event_handler_instance = NULL;
static esp_event_handler_instance_t s_ip_event_handler_instance = NULL;

static esp_timer_handle_t s_retry_timer = NULL;

/* 5. Forward declarations */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data);
static void retry_timer_callback(void *arg);
static void set_state_and_publish(sas_wifi_state_t new_state);
static void handle_sta_disconnected(void);
static void build_fallback_ssid(char *out, size_t out_len);
static void build_default_hostname(char *out, size_t out_len);
static bool normalize_hostname(const char *in, char *out, size_t out_len);
static void start_mdns(void);
static void switch_to_ap_fallback(void);
static void begin_sta_connect(void);
static void issue_connect_or_handle_failure(void);
static void load_config(void);
static uint32_t compute_backoff_ms(uint32_t retry_count);

/* 6. Public functions */

sas_status_t sas_wifi_init(void)
{
    if (s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    /* NVS/netif/the event loop are process-wide, call-once singletons
     * esp_wifi_init() needs; no other module owns this bring-up, and
     * Wi-Fi Manager is the first network module built, so it happens
     * here. sas_wifi_deinit() deliberately leaves it running, since other
     * modules (HTTP Server, OTA Manager) need it too. */
    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    configASSERT(err == ESP_OK);

    err = esp_netif_init();
    configASSERT((err == ESP_OK) || (err == ESP_ERR_INVALID_STATE));

    err = esp_event_loop_create_default();
    configASSERT((err == ESP_OK) || (err == ESP_ERR_INVALID_STATE));

    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();
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
        .arg = NULL,
        .name = "sas_wifi_retry",
    };
    configASSERT(esp_timer_create(&retry_timer_args, &s_retry_timer) == ESP_OK);

    load_config();
    start_mdns();

    s_state = SAS_WIFI_STATE_IDLE;
    s_retry_count = 0U;
    s_started = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_wifi_start(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    s_started = true;

    if (s_creds.ssid[0] == '\0') {
        /* SAS-WIFI-FR-002: no stored credentials, so go straight to the fallback AP. */
        set_state_and_publish(SAS_WIFI_STATE_AP_FALLBACK);
        switch_to_ap_fallback();
    } else {
        /* sas_wifi_connect() -> begin_sta_connect() performs the full
         * stop-then-configure-then-start sequence itself; esp_wifi_stop()
         * on a never-yet-started driver is a harmless no-op. */
        (void)sas_wifi_connect(&s_creds);
    }

    return SAS_STATUS_OK;
}

sas_status_t sas_wifi_stop(void)
{
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    (void)esp_timer_stop(
        s_retry_timer); /* SAS_ERR_INVALID_STATE-equivalent if not running; ignored */
    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();

    set_state_and_publish(SAS_WIFI_STATE_IDLE);
    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE) {
        s_retry_count = 0U;
        xSemaphoreGive(s_state_lock);
    }

    s_started = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_wifi_deinit(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_retry_timer != NULL) {
        (void)esp_timer_delete(s_retry_timer);
        s_retry_timer = NULL;
    }
    if (s_wifi_event_handler_instance != NULL) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                    s_wifi_event_handler_instance);
        s_wifi_event_handler_instance = NULL;
    }
    if (s_ip_event_handler_instance != NULL) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    s_ip_event_handler_instance);
        s_ip_event_handler_instance = NULL;
    }
    (void)esp_wifi_deinit();
    if (s_netif_sta != NULL) {
        esp_netif_destroy(s_netif_sta);
        s_netif_sta = NULL;
    }
    if (s_netif_ap != NULL) {
        esp_netif_destroy(s_netif_ap);
        s_netif_ap = NULL;
    }
    if (s_connect_event_group != NULL) {
        vEventGroupDelete(s_connect_event_group);
        s_connect_event_group = NULL;
    }
    if (s_scan_lock != NULL) {
        vSemaphoreDelete(s_scan_lock);
        s_scan_lock = NULL;
    }
    if (s_state_lock != NULL) {
        vSemaphoreDelete(s_state_lock);
        s_state_lock = NULL;
    }

    (void)memset(&s_creds, 0, sizeof(s_creds));
    s_state = SAS_WIFI_STATE_IDLE;
    s_retry_count = 0U;
    s_initialized = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_wifi_connect(const sas_wifi_credentials_t *creds)
{
    if (creds == NULL) {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    sas_config_value_t val;
    val.type = SAS_CFG_TYPE_STRING;
    (void)strncpy(val.value.str_val, creds->ssid, SAS_CFG_MAX_STRING_LEN - 1U);
    val.value.str_val[SAS_CFG_MAX_STRING_LEN - 1U] = '\0';
    sas_status_t persist_status = sas_config_set("wifi.sta.ssid", &val);
    if (persist_status != SAS_STATUS_OK) {
        sas_log_warn("failed to persist wifi.sta.ssid (0x%x)", (unsigned int)persist_status);
    }
    (void)strncpy(val.value.str_val, creds->password, SAS_CFG_MAX_STRING_LEN - 1U);
    val.value.str_val[SAS_CFG_MAX_STRING_LEN - 1U] = '\0';
    persist_status = sas_config_set("wifi.sta.password", &val);
    if (persist_status != SAS_STATUS_OK) {
        sas_log_warn("failed to persist wifi.sta.password (0x%x)", (unsigned int)persist_status);
    }

    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE) {
        s_creds = *creds;
        s_retry_count = 0U;
        xSemaphoreGive(s_state_lock);
    }

    xEventGroupClearBits(s_connect_event_group, SAS_WIFI_CONNECTED_BIT);
    set_state_and_publish(SAS_WIFI_STATE_CONNECTING);
    begin_sta_connect();

    EventBits_t bits = xEventGroupWaitBits(s_connect_event_group, SAS_WIFI_CONNECTED_BIT, pdFALSE,
                                           pdFALSE, pdMS_TO_TICKS(s_connect_timeout_ms));
    if ((bits & SAS_WIFI_CONNECTED_BIT) != 0U) {
        return SAS_STATUS_OK;
    }
    return SAS_WIFI_ERR_TIMEOUT; /* state machine continues asynchronously in the background */
}

sas_status_t sas_wifi_scan(sas_wifi_scan_result_t *out_array, uint8_t max_results,
                           uint8_t *out_count)
{
    if ((out_array == NULL) || (out_count == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_scan_lock, SAS_WIFI_SCAN_LOCK_WAIT_TICKS) != pdTRUE) {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        *out_count = 0U;
        status = SAS_WIFI_ERR_SCAN_FAILED;
    } else {
        uint16_t ap_count = SAS_WIFI_SCAN_MAX_RECORDS;
        (void)esp_wifi_scan_get_ap_records(&ap_count, s_scan_records);
        if (ap_count > max_results) {
            ap_count = max_results;
        }

        for (uint16_t i = 0U; i < ap_count; i++) {
            (void)strncpy(out_array[i].ssid, (const char *)s_scan_records[i].ssid,
                          sizeof(out_array[i].ssid) - 1U);
            out_array[i].ssid[sizeof(out_array[i].ssid) - 1U] = '\0';
            out_array[i].rssi = s_scan_records[i].rssi;
            out_array[i].auth_mode = (uint8_t)s_scan_records[i].authmode;
        }
        *out_count = (uint8_t)ap_count;
    }

    xSemaphoreGive(s_scan_lock);
    return status;
}

sas_wifi_state_t sas_wifi_get_state(void)
{
    sas_wifi_state_t state = SAS_WIFI_STATE_IDLE;

    if (s_initialized && (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)) {
        state = s_state;
        xSemaphoreGive(s_state_lock);
    }
    return state;
}

sas_status_t sas_wifi_get_status(sas_wifi_status_t *out)
{
    if (out == NULL) {
        return SAS_ERR_INVALID_ARG;
    }

    (void)memset(out, 0, sizeof(*out));
    if (s_initialized && (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE)) {
        out->state = s_state;
        (void)strncpy(out->ssid, s_creds.ssid, sizeof(out->ssid) - 1U);
        (void)strncpy(out->hostname, s_hostname, sizeof(out->hostname) - 1U);
        xSemaphoreGive(s_state_lock);
    }

    if (out->state == SAS_WIFI_STATE_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            out->rssi = ap_info.rssi;
        }

        esp_netif_ip_info_t ip_info;
        if ((s_netif_sta != NULL) && (esp_netif_get_ip_info(s_netif_sta, &ip_info) == ESP_OK)) {
            (void)snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip_info.ip));
        }
    }
    return SAS_STATUS_OK;
}

sas_status_t sas_wifi_set_hostname(const char *hostname)
{
    char normalized[SAS_WIFI_HOSTNAME_MAX_LEN];
    if ((hostname == NULL) || !normalize_hostname(hostname, normalized, sizeof(normalized))) {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    /* Applied first: the responder re-announces under the new name right away. */
    if (mdns_hostname_set(normalized) != ESP_OK) {
        return SAS_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE) {
        (void)strcpy(s_hostname, normalized);
        xSemaphoreGive(s_state_lock);
    }

    sas_config_value_t val;
    val.type = SAS_CFG_TYPE_STRING;
    (void)strcpy(val.value.str_val, normalized);
    sas_status_t persist_status = sas_config_set("wifi.hostname", &val);
    if (persist_status != SAS_STATUS_OK) {
        sas_log_warn("failed to persist wifi.hostname (0x%x)", (unsigned int)persist_status);
    }

    sas_log_info("mDNS name: %s.local", normalized);
    return SAS_STATUS_OK;
}

/* 7. Private functions */

/*! \brief Load the five wifi.* keys from Configuration Manager into module state. Caller:
 * sas_wifi_init(), before s_initialized is set. */
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

    /* wifi.hostname: a stored name wins; unset or unusable falls back to the MAC-derived one. */
    (void)sas_config_get("wifi.hostname", &val, &def);
    if (!normalize_hostname(val.value.str_val, s_hostname, sizeof(s_hostname))) {
        build_default_hostname(s_hostname, sizeof(s_hostname));
    }

    def.type = SAS_CFG_TYPE_INT;
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

/*! \brief Update s_state under lock, then publish outside the lock (never call into Event Bus while
 * holding s_state_lock). */
static void set_state_and_publish(sas_wifi_state_t new_state)
{
    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE) {
        s_state = new_state;
        xSemaphoreGive(s_state_lock);
    }

    sas_status_t publish_status =
        sas_event_bus_publish(SAS_EVENT_WIFI_STATE_CHANGED, &new_state, sizeof(new_state));
    if (publish_status != SAS_STATUS_OK) {
        sas_log_warn("SAS_EVENT_WIFI_STATE_CHANGED publish failed (0x%x)",
                     (unsigned int)publish_status);
    }
}

/*!
    \brief Apply s_creds to the STA driver config and (re)start the driver.
    \note  Does not itself call esp_wifi_connect(): esp_wifi_start() is
           asynchronous, so the actual connect attempt is issued from the
           WIFI_EVENT_STA_START handler in wifi_event_handler() once the
           driver confirms it is up, per standard ESP-IDF sequencing.
*/
static void begin_sta_connect(void)
{
    wifi_config_t wifi_config;
    (void)memset(&wifi_config, 0, sizeof(wifi_config));
    (void)strncpy((char *)wifi_config.sta.ssid, s_creds.ssid, sizeof(wifi_config.sta.ssid) - 1U);
    (void)strncpy((char *)wifi_config.sta.password, s_creds.password,
                  sizeof(wifi_config.sta.password) - 1U);

    (void)esp_wifi_stop(); /* harmless no-op if already stopped; guarantees a clean mode switch out
                              of AP_FALLBACK */
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    (void)esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    (void)esp_wifi_start();
}

/*! \brief Issue esp_wifi_connect(); if the driver rejects it immediately, treat that like a lost
 * link. */
static void issue_connect_or_handle_failure(void)
{
    if (esp_wifi_connect() != ESP_OK) {
        handle_sta_disconnected();
    }
}

/*! \brief WIFI_EVENT_STA_DISCONNECTED handling: retry with backoff, or fall back to AP once
 * wifi.retry.max_count is exhausted. */
static void handle_sta_disconnected(void)
{
    sas_wifi_state_t current;
    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE) {
        current = s_state;
        xSemaphoreGive(s_state_lock);
    } else {
        current = s_state;
    }

    if ((current == SAS_WIFI_STATE_IDLE) || (current == SAS_WIFI_STATE_AP_FALLBACK)) {
        return; /* no active STA attempt in progress; ignore spurious disconnect event */
    }

    uint32_t retry_count = 0U;
    bool exhausted = false;
    if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE) {
        if (s_retry_count < s_max_retry_count) {
            s_retry_count++;
            retry_count = s_retry_count;
        } else {
            exhausted = true;
        }
        xSemaphoreGive(s_state_lock);
    }

    if (exhausted) {
        set_state_and_publish(
            SAS_WIFI_STATE_AP_FALLBACK); /* exactly one transition event, per AC-WIFI-2 */
        switch_to_ap_fallback();
        return;
    }

    set_state_and_publish(SAS_WIFI_STATE_RECONNECTING);
    uint32_t backoff_ms = compute_backoff_ms(retry_count);
    (void)esp_timer_stop(s_retry_timer);
    (void)esp_timer_start_once(s_retry_timer, (uint64_t)backoff_ms * 1000ULL);
}

/*!
    \brief esp_timer one-shot callback: re-issue the STA connect attempt
           while remaining in RECONNECTING.
    \note  Calls esp_wifi_connect() directly, not begin_sta_connect(): the
           driver is already started (we are recovering from a disconnect,
           not doing a first-time start), so WIFI_EVENT_STA_START will not
           fire again to trigger it.
*/
static void retry_timer_callback(void *arg)
{
    (void)arg;
    issue_connect_or_handle_failure();
}

/*! \brief Exponential backoff, in ms, clamped to SAS_WIFI_MAX_BACKOFF_MS. */
static uint32_t compute_backoff_ms(uint32_t retry_count)
{
    uint32_t shift = (retry_count > 0U) ? (retry_count - 1U) : 0U;
    if (shift > SAS_WIFI_MAX_BACKOFF_SHIFT) {
        shift = SAS_WIFI_MAX_BACKOFF_SHIFT;
    }

    uint64_t backoff = (uint64_t)s_initial_backoff_ms << shift;
    if (backoff > SAS_WIFI_MAX_BACKOFF_MS) {
        backoff = SAS_WIFI_MAX_BACKOFF_MS;
    }
    return (uint32_t)backoff;
}

/*! \brief Derive "SAS-XXXXXX" from the last 3 bytes of the factory Wi-Fi STA MAC (SAS-WIFI-FR-002).
 */
static void build_fallback_ssid(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    (void)esp_wifi_get_mac(WIFI_IF_STA, mac);
    (void)snprintf(out, out_len, "SAS-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

/*! \brief Derive the default mDNS host name "sas-xxxxxx" from the same MAC bytes as the fallback
 * SSID, so the name shown on the fallback AP is also the device's default .local name. */
static void build_default_hostname(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    (void)esp_wifi_get_mac(WIFI_IF_STA, mac);
    (void)snprintf(out, out_len, "sas-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

/*! \brief Validate in as a single DNS label and copy it, lower-cased, into out: 1 to
 * out_len - 1 characters from a-z, 0-9 and '-', not starting or ending with '-'. */
static bool normalize_hostname(const char *in, char *out, size_t out_len)
{
    size_t len = strlen(in);
    if ((len == 0U) || (len >= out_len) || (in[0] == '-') || (in[len - 1U] == '-')) {
        return false;
    }

    for (size_t i = 0U; i < len; i++) {
        char c = in[i];
        if ((c >= 'A') && (c <= 'Z')) {
            c = (char)(c - 'A' + 'a');
        }
        if (!(((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '-'))) {
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
             server (and therefore the device's address) the operator does
             not control.
    \note    Not fatal if it fails: the device is still reachable by IP.
*/
static void start_mdns(void)
{
    const char *hostname = s_hostname; /* set by load_config(), which runs first */

    if ((mdns_init() != ESP_OK) || (mdns_hostname_set(hostname) != ESP_OK)) {
        sas_log_warn("mDNS unavailable; reach the device by IP address");
        return;
    }
    (void)mdns_instance_name_set("SAS Dashboard");
    (void)mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    sas_log_info("mDNS name: %s.local", hostname);
}

/*!
    \brief Stop the STA driver, reconfigure as an open fallback AP, and
           start it. See sas_wifi_init()'s doc comment for why the fallback
           AP is open rather than password-protected.
*/
static void switch_to_ap_fallback(void)
{
    char ap_ssid[33]; /* matches sas_wifi_credentials_t.ssid / sas_wifi_scan_result_t.ssid sizing */
    build_fallback_ssid(ap_ssid, sizeof(ap_ssid));

    wifi_config_t ap_config;
    (void)memset(&ap_config, 0, sizeof(ap_config));
    (void)strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1U);
    ap_config.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    ap_config.ap.channel = SAS_WIFI_AP_FALLBACK_CHANNEL;
    ap_config.ap.max_connection = SAS_WIFI_AP_FALLBACK_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    /* AP+STA rather than AP-only: esp_wifi_scan_start() needs the station interface, and an
     * operator on the fallback AP has to be able to scan for the network to join. The station
     * side stays idle here - wifi_event_handler() only issues a connect while a connection
     * attempt is in progress. */
    (void)esp_wifi_stop();
    (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
    (void)esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    (void)esp_wifi_start();
}

/*! \brief WIFI_EVENT / IP_EVENT dispatcher registered in sas_wifi_init(); runs on the default
 * ESP-IDF event loop task. */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            /* Also fires when the fallback AP comes up in AP+STA mode, where there is nothing
             * to connect to. */
            sas_wifi_state_t state = sas_wifi_get_state();
            if ((state == SAS_WIFI_STATE_CONNECTING) || (state == SAS_WIFI_STATE_RECONNECTING)) {
                issue_connect_or_handle_failure();
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            handle_sta_disconnected();
        } else {
            /* other WIFI_EVENT ids are not handled by this module */
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            if (xSemaphoreTake(s_state_lock, SAS_WIFI_LOCK_WAIT_TICKS) == pdTRUE) {
                s_retry_count = 0U;
                xSemaphoreGive(s_state_lock);
            }
            set_state_and_publish(SAS_WIFI_STATE_CONNECTED);
            xEventGroupSetBits(s_connect_event_group, SAS_WIFI_CONNECTED_BIT);
        }
    } else {
        /* no other event bases are registered */
    }
}
