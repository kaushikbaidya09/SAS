//=============================================================================
/*!
    \file    sas_ota_manager.c
    \brief   Implementation of the OTA Manager module.
*/
//=============================================================================

// Built on the ESP-IDF esp_ota_* API. Image verification is the check done by
// esp_ota_end(): image header, size, SHA-256, and the signature when Secure
// Boot is enabled.

#include "sas_ota_manager.h"

#include <stdbool.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "../core/sas_event_bus.h"
#include "../core/sas_logger.h"

static const char *TAG = "sas_ota";

/*! \brief Bounded wait for s_lock; a holder may be inside a flash erase/write. */
#define SAS_OTA_LOCK_WAIT_TICKS pdMS_TO_TICKS(5000)
#define SAS_OTA_URL_READ_CHUNK  (1024U) ///< Chunk size streaming an image from the HTTP client.
#define SAS_OTA_URL_TIMEOUT_MS  (10000) ///< HTTP client network timeout for begin_from_url().

static bool s_initialized = false;
static bool s_started     = false;

static SemaphoreHandle_t s_lock = NULL; ///< Guards every variable below.
static StaticSemaphore_t s_lock_storage;

static sas_ota_state_t s_state         = SAS_OTA_STATE_IDLE;
static uint8_t s_percent               = 0U;
static esp_ota_handle_t s_handle       = 0;
static const esp_partition_t *s_target = NULL; ///< Inactive partition being written.
static size_t s_total_size             = 0U;   ///< Expected image size; 0 if unknown.
static size_t s_written                = 0U;   ///< Bytes written so far.

static uint8_t s_url_chunk[SAS_OTA_URL_READ_CHUNK]; ///< begin_from_url() read buffer.

static void publish_progress(sas_ota_state_t state, uint8_t percent_complete);
static void set_state_locked(sas_ota_state_t state, uint8_t percent_complete);
static sas_status_t begin_locked(size_t total_size_hint);
static void fail_locked(void);
static void confirm_running_image(void);

/*!
    \brief   Initialize the OTA Manager module.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already
             initialized.
*/
sas_status_t sas_ota_init(void)
{
    if (s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    configASSERT(s_lock != NULL);

    s_state      = SAS_OTA_STATE_IDLE;
    s_percent    = 0U;
    s_handle     = 0;
    s_target     = NULL;
    s_total_size = 0U;
    s_written    = 0U;

    s_started     = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Start active OTA Manager operation. If the running image was just
             installed by OTA and is still pending verification, this is where
             it confirms itself (esp_ota_mark_app_valid_cancel_rollback()):
             reaching this call means the image booted through every earlier
             module. An image that resets before getting here is rolled back by
             the bootloader (requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_ota_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    confirm_running_image();

    s_started = true;
    return SAS_STATUS_OK;
}

/*!
    \brief   Stop active OTA Manager operation without releasing resources
             acquired by sas_ota_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_ota_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    // Only new begin_*() calls are rejected; an update already in progress is left to
    // complete or fail on its own.
    s_started = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Release all resources acquired by sas_ota_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_ota_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    (void)sas_ota_abort();
    if (s_lock != NULL)
    {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }

    s_started     = false;
    s_initialized = false;
    return SAS_STATUS_OK;
}

/*!
    \brief   Begin an OTA update from a chunked upload (e.g. the REST API),
             transitioning IDLE -> RECEIVING.
    \param[in] total_size_hint  Expected total image size in bytes (e.g. from a
                                Content-Length header), used only for the
                                pre-write size check below.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started or
             not in the IDLE or FAILED state; SAS_OTA_ERR_IMAGE_TOO_LARGE if
             total_size_hint exceeds the inactive partition size, checked
             before esp_ota_begin() is called.
*/
sas_status_t sas_ota_begin_from_upload(size_t total_size_hint)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, SAS_OTA_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = begin_locked(total_size_hint);

    xSemaphoreGive(s_lock);
    return status;
}

/*!
    \brief   Write one chunk of image data while in the RECEIVING state.
    \param[in] data  Non-NULL chunk buffer.
    \param[in] len   Length of data in bytes.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if data is NULL;
             SAS_ERR_INVALID_STATE if not in the RECEIVING state;
             SAS_OTA_ERR_IMAGE_TOO_LARGE if the data would overrun the inactive
             partition, or SAS_OTA_ERR_VERIFY_FAILED if the flash write is
             rejected (e.g. not a valid application image) - in both cases the
             update is aborted and the state moves to FAILED.
*/
sas_status_t sas_ota_write_chunk(const uint8_t *data, size_t len)
{
    if (data == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, SAS_OTA_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    if (s_state != SAS_OTA_STATE_RECEIVING)
    {
        status = SAS_ERR_INVALID_STATE;
    }
    else if ((s_written + len) > s_target->size)
    {
        // The size hint was absent or understated: stop before overrunning the partition.
        fail_locked();
        status = SAS_OTA_ERR_IMAGE_TOO_LARGE;
    }
    else if (esp_ota_write(s_handle, data, len) != ESP_OK)
    {
        fail_locked();
        status = SAS_OTA_ERR_VERIFY_FAILED;
    }
    else
    {
        s_written += len;
        if (s_total_size > 0U)
        {
            // Hold at 99 until finish() has verified the image.
            uint32_t percent = (uint32_t)(((uint64_t)s_written * 100U) / s_total_size);
            if (percent > 99U)
            {
                percent = 99U;
            }
            if ((uint8_t)percent != s_percent)
            {
                set_state_locked(SAS_OTA_STATE_RECEIVING, (uint8_t)percent);
            }
        }
    }

    xSemaphoreGive(s_lock);
    return status;
}

/*!
    \brief   Finalize the current OTA update: ends the write, verifies the
             image, and on success selects it as the next boot partition,
             transitioning RECEIVING -> VERIFYING -> READY_TO_REBOOT (or ->
             FAILED on verification failure).
    \return  SAS_STATUS_OK on success (state is READY_TO_REBOOT);
             SAS_ERR_INVALID_STATE if not in the RECEIVING state;
             SAS_OTA_ERR_VERIFY_FAILED if checksum/signature verification fails
             (state -> FAILED, current boot partition left untouched).
*/
sas_status_t sas_ota_finish(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, SAS_OTA_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    if (s_state != SAS_OTA_STATE_RECEIVING)
    {
        status = SAS_ERR_INVALID_STATE;
    }
    else
    {
        set_state_locked(SAS_OTA_STATE_VERIFYING, 99U);

        // esp_ota_end() releases the handle whether or not verification passes.
        esp_err_t err = esp_ota_end(s_handle);
        s_handle      = 0;
        if (err == ESP_OK)
        {
            err = esp_ota_set_boot_partition(s_target);
        }

        if (err != ESP_OK)
        {
            sas_log_error("image verification failed (0x%x)", (unsigned int)err);
            set_state_locked(SAS_OTA_STATE_FAILED, 0U);
            status = SAS_OTA_ERR_VERIFY_FAILED;
        }
        else
        {
            sas_log_info("image verified; next boot is partition '%s'", s_target->label);
            set_state_locked(SAS_OTA_STATE_READY_TO_REBOOT, 100U);
        }
    }

    xSemaphoreGive(s_lock);
    return status;
}

/*!
    \brief   Begin an OTA update by fetching the image directly from url via
             the HTTP client, transitioning IDLE -> RECEIVING and driving the
             update to completion internally.
    \param[in] url  Non-NULL image URL.
    \return  SAS_STATUS_OK on success (state is READY_TO_REBOOT);
             SAS_ERR_INVALID_ARG if url is NULL; SAS_ERR_INVALID_STATE if not
             started or not in the IDLE or FAILED state; SAS_ERR_TIMEOUT if the
             download fails or stalls; SAS_OTA_ERR_IMAGE_TOO_LARGE if the
             reported image size exceeds the inactive partition size, checked
             before any flash write begins; SAS_OTA_ERR_VERIFY_FAILED if
             checksum/signature verification fails.
*/
sas_status_t sas_ota_begin_from_url(const char *url)
{
    // Blocks the calling task for the whole download; call it from a task that
    // can afford that, not from the HTTP Server's task.

    if (url == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    // Reject early so that a busy module costs no network request. The state
    // is checked again under the lock once the size is known.
    sas_ota_state_t state = sas_ota_get_state();
    if ((state != SAS_OTA_STATE_IDLE) && (state != SAS_OTA_STATE_FAILED))
    {
        return SAS_ERR_INVALID_STATE;
    }

    esp_http_client_config_t config = {
        .url               = url,
        .timeout_ms        = SAS_OTA_URL_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    sas_status_t status = SAS_STATUS_OK;
    bool began          = false;

    if (esp_http_client_open(client, 0) != ESP_OK)
    {
        sas_log_error("could not connect to the image URL");
        status = SAS_ERR_TIMEOUT;
    }

    if (status == SAS_STATUS_OK)
    {
        int64_t content_length = esp_http_client_fetch_headers(client);
        int http_status        = esp_http_client_get_status_code(client);
        if ((content_length < 0) || (http_status != 200))
        {
            sas_log_error("image URL answered HTTP %d", http_status);
            status = SAS_ERR_TIMEOUT;
        }
        else
        {
            // content_length is 0 for a chunked response: size unknown, checked per chunk.
            status = sas_ota_begin_from_upload((size_t)content_length);
            began  = (status == SAS_STATUS_OK);
        }
    }

    while (status == SAS_STATUS_OK)
    {
        int read_len = esp_http_client_read(client, (char *)s_url_chunk, sizeof(s_url_chunk));
        if (read_len < 0)
        {
            status = SAS_ERR_TIMEOUT;
        }
        else if (read_len == 0)
        {
            if (!esp_http_client_is_complete_data_received(client))
            {
                status = SAS_ERR_TIMEOUT; // connection dropped mid-image
            }
            break;
        }
        else
        {
            status = sas_ota_write_chunk(s_url_chunk, (size_t)read_len);
        }
    }

    (void)esp_http_client_close(client);
    (void)esp_http_client_cleanup(client);

    if (status == SAS_STATUS_OK)
    {
        status = sas_ota_finish();
    }
    else if (began)
    {
        (void)sas_ota_abort(); // no-op if write_chunk() already moved the state to FAILED
    }
    else
    {
        // Never reached RECEIVING: report the failure without having touched flash.
        if (xSemaphoreTake(s_lock, SAS_OTA_LOCK_WAIT_TICKS) == pdTRUE)
        {
            if ((s_state == SAS_OTA_STATE_IDLE) || (s_state == SAS_OTA_STATE_FAILED))
            {
                set_state_locked(SAS_OTA_STATE_FAILED, 0U);
            }
            xSemaphoreGive(s_lock);
        }
    }
    return status;
}

/*!
    \brief   Abandon an update that is in the RECEIVING state (e.g. the
             uploading client disconnected), discarding what was written and
             moving the state to FAILED. The current boot partition is
             untouched.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not in the
             RECEIVING state.
*/
sas_status_t sas_ota_abort(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, SAS_OTA_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    if (s_state != SAS_OTA_STATE_RECEIVING)
    {
        status = SAS_ERR_INVALID_STATE;
    }
    else
    {
        fail_locked();
    }

    xSemaphoreGive(s_lock);
    return status;
}

/*!
    \brief   Get the current OTA update state.
    \return  Current sas_ota_state_t.
*/
sas_ota_state_t sas_ota_get_state(void)
{
    sas_ota_progress_t progress = {.percent_complete = 0U, .state = SAS_OTA_STATE_IDLE};
    (void)sas_ota_get_progress(&progress);
    return progress.state;
}

/*!
    \brief   Get the latest progress snapshot (state and percent complete).
    \param[out] out  Non-NULL destination.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out is NULL.
*/
sas_status_t sas_ota_get_progress(sas_ota_progress_t *out)
{
    if (out == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    out->state            = SAS_OTA_STATE_IDLE;
    out->percent_complete = 0U;
    if (s_initialized && (xSemaphoreTake(s_lock, SAS_OTA_LOCK_WAIT_TICKS) == pdTRUE))
    {
        out->state            = s_state;
        out->percent_complete = s_percent;
        xSemaphoreGive(s_lock);
    }
    return SAS_STATUS_OK;
}

/*!
    \brief   Publish a SAS_EVENT_OTA_PROGRESS event for one state transition or
             progress step.
*/
static void publish_progress(sas_ota_state_t state, uint8_t percent_complete)
{
    const sas_ota_progress_t progress = {
        .percent_complete = percent_complete,
        .state            = state,
    };
    (void)sas_event_bus_publish(SAS_EVENT_OTA_PROGRESS, &progress, sizeof(progress));
}

/*!
    \brief   Record state/percent and publish them. Caller holds s_lock;
             subscribers must not call back into this module's state-changing
             API from the callback.
*/
static void set_state_locked(sas_ota_state_t state, uint8_t percent_complete)
{
    s_state   = state;
    s_percent = percent_complete;
    publish_progress(state, percent_complete);
}

/*!
    \brief   IDLE/FAILED -> RECEIVING: size-check against the inactive
             partition, then esp_ota_begin(). Caller holds s_lock.
*/
static sas_status_t begin_locked(size_t total_size_hint)
{
    if ((s_state != SAS_OTA_STATE_IDLE) && (s_state != SAS_OTA_STATE_FAILED))
    {
        return SAS_ERR_INVALID_STATE;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL)
    {
        sas_log_error("no inactive OTA partition in the partition table");
        return SAS_ERR_NOT_SUPPORTED;
    }

    // Reject before anything is written to flash (SAS-OTA-NFR-002).
    if (total_size_hint > target->size)
    {
        return SAS_OTA_ERR_IMAGE_TOO_LARGE;
    }

    // Sequential-write mode erases flash as the image arrives. Erasing the
    // whole partition first would stall the caller for seconds.
    if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle) != ESP_OK)
    {
        sas_log_error("esp_ota_begin failed on partition '%s'", target->label);
        return SAS_ERR_INVALID_STATE;
    }

    s_target     = target;
    s_total_size = total_size_hint;
    s_written    = 0U;
    sas_log_info("receiving image into partition '%s'", target->label);
    set_state_locked(SAS_OTA_STATE_RECEIVING, 0U);
    return SAS_STATUS_OK;
}

/*!
    \brief   RECEIVING -> FAILED: discard the partial image. The current boot
             partition is never touched on this path (SAS-OTA-FR-004). Caller
             holds s_lock.
*/
static void fail_locked(void)
{
    (void)esp_ota_abort(s_handle);
    s_handle = 0;
    sas_log_warn("update aborted after %u bytes", (unsigned int)s_written);
    set_state_locked(SAS_OTA_STATE_FAILED, 0U);
}

/*!
    \brief   If the running image is a freshly installed OTA image awaiting
             confirmation, mark it valid so the bootloader stops treating the
             previous image as the rollback target.
*/
static void confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state;
    if ((running != NULL) && (esp_ota_get_state_partition(running, &image_state) == ESP_OK) &&
        (image_state == ESP_OTA_IMG_PENDING_VERIFY))
    {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        {
            sas_log_info("new firmware in '%s' confirmed", running->label);
        }
        else
        {
            sas_log_error("failed to confirm new firmware in '%s'", running->label);
        }
    }
}
