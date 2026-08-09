/*!
    \file    sas_ble_scanner.c
    \brief   Implementation of the BLE Scanner module.
*/

/* 1. Includes */
#include "sas_ble_scanner.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* NimBLE calls below follow known usage patterns but aren't verified
 * against a pinned header version - check symbol names/struct fields
 * before treating this file as finished. */
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "../core/sas_config.h"
#include "../core/sas_event_bus.h"
#include "../core/sas_logger.h"

static const char *TAG = "sas_ble";

/* 2. Private macros */
#define SAS_BLE_LOCK_WAIT_TICKS \
    pdMS_TO_TICKS(              \
        50) ///< Bounded wait for s_table_lock; critical sections are short table scans/copies.

#define SAS_BLE_MAC_LEN (6U) ///< Bytes in a BLE MAC address; matches sas_ble_device_t.mac.

/* 3. Private typedefs */

/*! One de-duplication table slot. */
typedef struct
{
    bool in_use;             ///< false if this slot is free.
    sas_ble_device_t device; ///< mac/name/rssi/last_seen_us.
} sas_ble_slot_t;

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;
/* Latches that nimble_port_freertos_init() has run: the NimBLE host task
 * is not designed to be cleanly stopped and restarted, so a later
 * sas_ble_stop()/sas_ble_start() cycle must not recreate it; only
 * sas_ble_deinit() tears it down (nimble_port_stop()). */
static bool s_host_started = false;
static volatile bool s_host_synced =
    false; ///< Set by on_sync() once the NimBLE host/controller are ready.
static bool s_scanning = false;

static sas_ble_slot_t s_table[SAS_BLE_MAX_DEVICES];
static uint32_t s_max_devices =
    SAS_BLE_MAX_DEVICES; ///< Effective cap <= SAS_BLE_MAX_DEVICES, from ble.max_devices.

static SemaphoreHandle_t s_table_lock = NULL;
static StaticSemaphore_t s_table_lock_storage;

static uint32_t s_scan_interval_ms = 100U;
static uint32_t s_scan_window_ms = 50U;

/* 5. Forward declarations */
static void nimble_host_task(void *param);
static void on_sync(void);
static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void record_advertisement(const uint8_t mac[SAS_BLE_MAC_LEN], const char *name, int8_t rssi);
static sas_ble_slot_t *find_by_mac(const uint8_t mac[SAS_BLE_MAC_LEN]);
static sas_ble_slot_t *find_free_or_oldest_slot(void);
static void load_config(void);

/* 6. Public functions */

sas_status_t sas_ble_init(void)
{
    if (s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    (void)memset(s_table, 0, sizeof(s_table));

    s_table_lock = xSemaphoreCreateMutexStatic(&s_table_lock_storage);
    configASSERT(s_table_lock != NULL);

    load_config();

    /* NimBLE (like esp_wifi) needs NVS; see sas_wifi_manager.c for the
     * same erase-and-retry pattern. Harmless if Wi-Fi Manager already did
     * this: BLE Scanner declares no dependency on it and must not assume
     * a particular boot order relative to it. */
    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    configASSERT(err == ESP_OK);

    configASSERT(nimble_port_init() == ESP_OK);
    ble_hs_cfg.sync_cb = &on_sync;

    s_host_started = false;
    s_host_synced = false;
    s_scanning = false;
    s_started = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_ble_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (!s_host_started)
    {
        nimble_port_freertos_init(nimble_host_task);
        s_host_started = true;
    }

    s_started = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_ble_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_scanning)
    {
        (void)ble_gap_disc_cancel();
        s_scanning = false;
    }

    s_started = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_ble_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_host_started)
    {
        /* nimble_port_stop() causes nimble_port_run() to return inside
         * nimble_host_task(), which then calls
         * nimble_port_freertos_deinit() itself; calling that here instead
         * would run it from the wrong task context. */
        (void)nimble_port_stop();
        s_host_started = false;
    }
    if (s_table_lock != NULL)
    {
        vSemaphoreDelete(s_table_lock);
        s_table_lock = NULL;
    }
    (void)memset(s_table, 0, sizeof(s_table));

    s_host_synced = false;
    s_initialized = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_ble_scan_start(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_scanning)
    {
        return SAS_BLE_ERR_ALREADY_SCANNING;
    }
    if (!s_host_synced)
    {
        return SAS_BLE_ERR_SCAN_FAILED; /* host/controller not ready yet */
    }

    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0)
    {
        return SAS_BLE_ERR_SCAN_FAILED;
    }

    struct ble_gap_disc_params disc_params;
    (void)memset(&disc_params, 0, sizeof(disc_params));
    disc_params.passive = 1;                                            /* SAS-BLE-NFR-002 / observer-only: never send scan requests */
    disc_params.filter_duplicates = 0;                                  /* de-dup ourselves so rssi/last_seen_us keep updating */
    disc_params.itvl = (uint16_t)((s_scan_interval_ms * 1000U) / 625U); /* ms -> 0.625ms units */
    disc_params.window = (uint16_t)((s_scan_window_ms * 1000U) / 625U); /* ms -> 0.625ms units */

    if (ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, &gap_event_cb, NULL) != 0)
    {
        return SAS_BLE_ERR_SCAN_FAILED;
    }

    s_scanning = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_ble_scan_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (!s_scanning)
    {
        return SAS_BLE_ERR_NOT_SCANNING;
    }

    (void)ble_gap_disc_cancel();
    s_scanning = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_ble_get_devices(sas_ble_device_t *out_array, uint16_t max_entries,
                                 uint16_t *out_count)
{
    if ((out_array == NULL) || (out_count == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_table_lock, SAS_BLE_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    uint16_t count = 0U;
    for (size_t i = 0U; (i < s_max_devices) && (count < max_entries); i++)
    {
        if (s_table[i].in_use)
        {
            out_array[count] = s_table[i].device;
            count++;
        }
    }
    xSemaphoreGive(s_table_lock);

    *out_count = count;
    return SAS_STATUS_OK;
}

/* 7. Private functions */

/*! \brief Load ble.scan.interval_ms / ble.scan.window_ms / ble.max_devices. Caller: sas_ble_init(),
 * before s_initialized is set. */
static void load_config(void)
{
    sas_config_value_t val;
    sas_config_value_t def;

    def.type = SAS_CFG_TYPE_INT;
    def.value.int_val = (int32_t)s_scan_interval_ms;
    (void)sas_config_get("ble.scan.interval_ms", &val, &def);
    s_scan_interval_ms = (uint32_t)val.value.int_val;

    def.value.int_val = (int32_t)s_scan_window_ms;
    (void)sas_config_get("ble.scan.window_ms", &val, &def);
    s_scan_window_ms = (uint32_t)val.value.int_val;

    def.value.int_val = (int32_t)SAS_BLE_MAX_DEVICES;
    (void)sas_config_get("ble.max_devices", &val, &def);
    uint32_t requested = (uint32_t)val.value.int_val;
    s_max_devices =
        ((requested > 0U) && (requested <= SAS_BLE_MAX_DEVICES)) ? requested : SAS_BLE_MAX_DEVICES;
}

/*! \brief FreeRTOS entry point for nimble_port_freertos_init(): runs NimBLE's host loop until
 * nimble_port_stop() returns it, then tears the task down. */
static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/*! \brief NimBLE host/controller sync callback, registered via ble_hs_cfg.sync_cb in
 * sas_ble_init(). Runs on the NimBLE host task. */
static void on_sync(void)
{
    s_host_synced = true;
}

/*! \brief Find the in-use slot for mac among the first s_max_devices slots, or NULL. Caller holds
 * s_table_lock. */
static sas_ble_slot_t *find_by_mac(const uint8_t mac[SAS_BLE_MAC_LEN])
{
    for (size_t i = 0U; i < s_max_devices; i++)
    {
        if (s_table[i].in_use && (memcmp(s_table[i].device.mac, mac, SAS_BLE_MAC_LEN) == 0))
        {
            return &s_table[i];
        }
    }
    return NULL;
}

/*! \brief First free slot among the first s_max_devices, or the least-recently-seen in-use one
 * (SAS-BLE-NFR-001). Caller holds s_table_lock. */
static sas_ble_slot_t *find_free_or_oldest_slot(void)
{
    for (size_t i = 0U; i < s_max_devices; i++)
    {
        if (!s_table[i].in_use)
        {
            return &s_table[i];
        }
    }

    size_t oldest_index = 0U;
    int64_t oldest_time = s_table[0].device.last_seen_us;
    for (size_t i = 1U; i < s_max_devices; i++)
    {
        if (s_table[i].device.last_seen_us < oldest_time)
        {
            oldest_time = s_table[i].device.last_seen_us;
            oldest_index = i;
        }
    }
    return &s_table[oldest_index];
}

/*!
    \brief   De-dup one advertisement by MAC: update rssi/last_seen_us for
             a known MAC, or insert (evicting the oldest if full) and
             publish SAS_EVENT_BLE_DEVICE_DISCOVERED for a new one.
    \param   name Advertised name, or "" if none present; empty never
                  overwrites a previously recorded name.
*/
static void record_advertisement(const uint8_t mac[SAS_BLE_MAC_LEN], const char *name, int8_t rssi)
{
    int64_t now_us = esp_timer_get_time();
    bool is_new = false;
    sas_ble_device_t snapshot;
    (void)memset(&snapshot, 0, sizeof(snapshot));

    if (xSemaphoreTake(s_table_lock, SAS_BLE_LOCK_WAIT_TICKS) == pdTRUE)
    {
        sas_ble_slot_t *slot = find_by_mac(mac);
        if (slot == NULL)
        {
            slot = find_free_or_oldest_slot();
            (void)memset(slot, 0, sizeof(*slot));
            (void)memcpy(slot->device.mac, mac, SAS_BLE_MAC_LEN);
            slot->in_use = true;
            is_new = true;
        }

        if (name[0] != '\0')
        {
            (void)strncpy(slot->device.name, name, sizeof(slot->device.name) - 1U);
            slot->device.name[sizeof(slot->device.name) - 1U] = '\0';
        }
        slot->device.rssi = rssi;
        slot->device.last_seen_us = now_us;
        snapshot = slot->device;

        xSemaphoreGive(s_table_lock);
    }

    if (is_new)
    {
        sas_status_t publish_status =
            sas_event_bus_publish(SAS_EVENT_BLE_DEVICE_DISCOVERED, &snapshot, sizeof(snapshot));
        if (publish_status != SAS_STATUS_OK)
        {
            sas_log_warn("SAS_EVENT_BLE_DEVICE_DISCOVERED publish failed (0x%x)",
                         (unsigned int)publish_status);
        }
    }
}

/*! \brief NimBLE GAP callback for ble_gap_disc(); runs on the host task. Only handles
 * BLE_GAP_EVENT_DISC, since this module never initiates a connection (SAS-BLE-NFR-002). */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event->type != BLE_GAP_EVENT_DISC)
    {
        return 0;
    }

    const struct ble_gap_disc_desc *disc = &event->disc;
    char name[32] = {0};

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0)
    {
        if ((fields.name != NULL) && (fields.name_len > 0U))
        {
            size_t len =
                (fields.name_len < (sizeof(name) - 1U)) ? fields.name_len : (sizeof(name) - 1U);
            (void)memcpy(name, fields.name, len);
            name[len] = '\0';
        }
    }

    record_advertisement(disc->addr.val, name, disc->rssi);
    return 0;
}
