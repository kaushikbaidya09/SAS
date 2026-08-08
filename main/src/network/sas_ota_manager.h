/*!
    \file    sas_ota_manager.h
    \brief   Public interface for the OTA Manager module.

    \description
    Firmware update over the ESP-IDF dual-OTA-partition scheme: accepts an
    image either as a chunked upload (e.g. from the REST API) or fetched
    directly from a URL, verifies it, and hands off to the bootloader for
    the next boot. Owns the sas_ota_state_t update state machine (see
    below) and publishes SAS_EVENT_OTA_PROGRESS on every transition.

    \dependencies
    - File Manager (main/src/storage/sas_file_manager.h): optional, only
      if a firmware image is staged to the local filesystem ahead of
      installation; the chunked-upload path
      (sas_ota_begin_from_upload()/write_chunk()/finish()) does not
      require it.
    - HTTP client (ESP-IDF esp_http_client component): used by
      sas_ota_begin_from_url() to stream the image directly into the
      esp_ota_* write path.
    - System Information (main/src/core/sas_sysinfo.h): used to size-check
      the inactive partition ahead of esp_ota_begin().
    - Event Bus (main/src/core/sas_event_bus.h): publishes
      SAS_EVENT_OTA_PROGRESS on every state transition below.
*/

#pragma once

/* 1. Standard includes */
#include <stddef.h>
#include <stdint.h>

/* 2. ESP-IDF includes */
/* (none) */

/* 3. Project includes */
#include "../core/sas_status.h"

/* 4. Public types */

/*!
    Update state machine:

    IDLE --begin_from_upload()/begin_from_url()--> RECEIVING
    RECEIVING --transfer complete--> VERIFYING
    VERIFYING --checksum/signature OK--> READY_TO_REBOOT
    VERIFYING --checksum/signature FAIL--> FAILED (partition discarded, current
                                            boot partition untouched)
    READY_TO_REBOOT --reboot--> [new image runs] --self-confirm within
        SAS_OTA_CONFIRM_TIMEOUT_MS--> previous partition marked valid
    [new image fails to self-confirm in time] --watchdog reboot--> old
        partition (ESP-IDF bootloader anti-rollback)

    Each state transition above publishes SAS_EVENT_OTA_PROGRESS with
    percent complete and state.
*/
typedef enum {
    SAS_OTA_STATE_IDLE = 0,
    SAS_OTA_STATE_RECEIVING,
    SAS_OTA_STATE_VERIFYING,
    SAS_OTA_STATE_READY_TO_REBOOT,
    SAS_OTA_STATE_FAILED,
} sas_ota_state_t;

/*! Update progress snapshot published with SAS_EVENT_OTA_PROGRESS. */
typedef struct {
    uint8_t         percent_complete; ///< 0-100 completion of the current state.
    sas_ota_state_t state;            ///< State this snapshot was published from.
} sas_ota_progress_t;

/* 5. Public constants */

#define SAS_OTA_ERR_BASE (0x8B00U) ///< Custom status base for this module.
#define SAS_OTA_ERR_IMAGE_TOO_LARGE (SAS_OTA_ERR_BASE + 1U) ///< begin_from_upload()/begin_from_url() rejected: image exceeds the inactive partition size; no flash write to the inactive partition was started.
#define SAS_OTA_ERR_VERIFY_FAILED (SAS_OTA_ERR_BASE + 2U) ///< finish() failed: checksum/signature verification failed; state moved to FAILED and the current boot partition is untouched.

/* 6. Public API declarations */

/*!
    \brief   Initialize the OTA Manager module.
    \return  SAS_STATUS_OK on success.
*/
sas_status_t sas_ota_init(void);

/*!
    \brief   Start active OTA Manager operation.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_ota_start(void);

/*!
    \brief   Stop active OTA Manager operation without releasing resources
             acquired by sas_ota_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_ota_stop(void);

/*!
    \brief   Release all resources acquired by sas_ota_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_ota_deinit(void);

/*!
    \brief   Begin an OTA update from a chunked upload (e.g. the REST API),
             transitioning IDLE -> RECEIVING.
    \param   total_size_hint Expected total image size in bytes (e.g. from
             a Content-Length header), used only for the pre-write size
             check below.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started
             or not in the IDLE state; SAS_OTA_ERR_IMAGE_TOO_LARGE if
             total_size_hint exceeds the inactive partition size, checked
             before esp_ota_begin() is called.
*/
sas_status_t sas_ota_begin_from_upload(size_t total_size_hint);

/*!
    \brief   Write one chunk of image data while in the RECEIVING state.
    \param   data Non-NULL chunk buffer.
    \param   len  Length of data in bytes.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if data is
             NULL; SAS_ERR_INVALID_STATE if not in the RECEIVING state.
*/
sas_status_t sas_ota_write_chunk(const uint8_t *data, size_t len);

/*!
    \brief   Finalize the current OTA update: ends the write, verifies the
             image, and on success selects it as the next boot partition,
             transitioning RECEIVING -> VERIFYING -> READY_TO_REBOOT (or
             -> FAILED on verification failure).
    \return  SAS_STATUS_OK on success (state is READY_TO_REBOOT);
             SAS_ERR_INVALID_STATE if not in the RECEIVING state;
             SAS_OTA_ERR_VERIFY_FAILED if checksum/signature verification
             fails (state -> FAILED, current boot partition left
             untouched).
*/
sas_status_t sas_ota_finish(void);

/*!
    \brief   Begin an OTA update by fetching the image directly from url
             via the HTTP client, transitioning IDLE -> RECEIVING and
             driving the update to completion internally.
    \param   url Non-NULL image URL.
    \return  SAS_STATUS_OK on success (state is READY_TO_REBOOT);
             SAS_ERR_INVALID_ARG if url is NULL; SAS_ERR_INVALID_STATE if
             not started or not in the IDLE state;
             SAS_OTA_ERR_IMAGE_TOO_LARGE if the reported image size
             exceeds the inactive partition size, checked before any
             flash write begins; SAS_OTA_ERR_VERIFY_FAILED if
             checksum/signature verification fails.
*/
sas_status_t sas_ota_begin_from_url(const char *url);

/*!
    \brief   Get the current OTA update state.
    \return  Current sas_ota_state_t.
*/
sas_ota_state_t sas_ota_get_state(void);
