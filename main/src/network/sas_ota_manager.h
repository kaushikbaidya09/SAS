//=============================================================================
/*!
    \file    sas_ota_manager.h
    \brief   Public interface for the OTA Manager module.
*/
//=============================================================================

// Firmware update using the two OTA partitions. An image is received as a
// chunked upload or downloaded from a URL, verified, and selected for the next
// boot. Progress is published as SAS_EVENT_OTA_PROGRESS.

#ifndef SAS_OTA_MANAGER_H
#define SAS_OTA_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#include "../core/sas_status.h"

/*!
    \brief Update state machine:

    IDLE --begin_from_upload()/begin_from_url()--> RECEIVING
    FAILED --begin_from_upload()/begin_from_url()--> RECEIVING (retry)
    RECEIVING --write error/abort()--> FAILED
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
typedef enum
{
    SAS_OTA_STATE_IDLE = 0,
    SAS_OTA_STATE_RECEIVING,
    SAS_OTA_STATE_VERIFYING,
    SAS_OTA_STATE_READY_TO_REBOOT,
    SAS_OTA_STATE_FAILED,
} sas_ota_state_t;

/*! \brief Update progress snapshot published with SAS_EVENT_OTA_PROGRESS. */
typedef struct
{
    uint8_t percent_complete; ///< 0-100 completion of the current state.
    sas_ota_state_t state;    ///< State this snapshot was published from.
} sas_ota_progress_t;

#define SAS_OTA_ERR_BASE (0x8B00U) ///< Custom status base for this module.
/*!
    \brief   begin_from_upload()/begin_from_url() rejected: image exceeds the
             inactive partition size; no flash write to the inactive partition
             was started.
*/
#define SAS_OTA_ERR_IMAGE_TOO_LARGE (SAS_OTA_ERR_BASE + 1U)
/*!
    \brief   finish() failed: checksum/signature verification failed; state
             moved to FAILED and the current boot partition is untouched.
*/
#define SAS_OTA_ERR_VERIFY_FAILED (SAS_OTA_ERR_BASE + 2U)

sas_status_t sas_ota_init(void);

sas_status_t sas_ota_start(void);

sas_status_t sas_ota_stop(void);

sas_status_t sas_ota_deinit(void);

sas_status_t sas_ota_begin_from_upload(size_t total_size_hint);

sas_status_t sas_ota_write_chunk(const uint8_t *data, size_t len);

sas_status_t sas_ota_finish(void);

sas_status_t sas_ota_begin_from_url(const char *url);

sas_ota_state_t sas_ota_get_state(void);

sas_status_t sas_ota_abort(void);

sas_status_t sas_ota_get_progress(sas_ota_progress_t *out);

#endif /* SAS_OTA_MANAGER_H */
