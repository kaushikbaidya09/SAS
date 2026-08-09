/*!
    \file    sas_file_manager.c
    \brief   Implementation skeleton for the File Manager module.

    \description
    See sas_file_manager.h for the public contract. This file currently
    contains only the lifecycle and API stubs; behaviour is filled in per
    module during implementation, in Development Order.

    \dependencies
    (none: File Manager has no dependency on any other SAS module.)
*/

/* 1. Includes */
#include "sas_file_manager.h"

#include <stddef.h>

/* 2. Private macros */
#define SAS_FS_READY_MARKER_PATH \
    "/.sas_fs_ready" ///< Reserved marker file used to detect first-boot / decide
                     ///< format_if_mount_failed.
#define SAS_FS_TMP_SUFFIX \
    ".tmp" ///< Temp path suffix used by sas_fs_write() for the write-then-rename replace.

/* 3. Private typedefs */
/* (none yet: internal path-mutex state to be added during implementation) */

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;

/* 5. Forward declarations */
static sas_status_t check_free_space_margin(size_t additional_bytes);

/* 6. Public functions */

sas_status_t sas_fs_init(void)
{
    /* TODO:
     * 1. Mount LittleFS via esp_vfs_littlefs_register().
     * 2. Check for SAS_FS_READY_MARKER_PATH directly on the partition (not
     *    a Configuration Manager key, per the no-dependency note in this
     *    module's header) to detect first boot.
     * 3. Set format_if_mount_failed only for that first-boot case, then
     *    create the marker file.
     * 4. Create the path-serializing mutex used by sas_fs_write()/rename()/
     *    delete(). */
    s_initialized = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_fs_start(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Mark the module ready to service list/read/write/delete/rename
     *    calls. */
    s_started = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_fs_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Reject new list/read/write/delete/rename calls.
     * 2. Do not unmount the partition or release the mutex acquired in
     *    sas_fs_init() (stop() must not release resources). */
    s_started = false;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_fs_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Release the path-serializing mutex acquired in sas_fs_init().
     * 2. Unmount LittleFS via the matching esp_vfs_littlefs_unregister()
     *    call.
     * 3. Reset module state to uninitialized. */
    s_initialized = false;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_fs_list(const char *dir_path, sas_file_entry_t *out_array, uint16_t max_entries,
                         uint16_t *out_count)
{
    if ((dir_path == NULL) || (out_array == NULL) || (out_count == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. opendir(dir_path)/readdir() up to max_entries, populating
     *    out_array[i].name/size_bytes/is_dir.
     * 2. Return SAS_FS_ERR_NOT_FOUND if dir_path does not exist.
     * Note: concurrent reads of different files are not blocked against
     * each other. */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_fs_read(const char *path, uint8_t *out_buf, size_t buf_len, size_t *out_len)
{
    if ((path == NULL) || (out_buf == NULL) || (out_len == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. fopen(path, "rb")/fread() up to buf_len bytes.
     * 2. Return SAS_FS_ERR_NOT_FOUND if path does not exist.
     * Note: not serialized against other reads. */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_fs_write(const char *path, const uint8_t *data, size_t len)
{
    if ((path == NULL) || (data == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Acquire the path-serializing mutex.
     * 2. Call check_free_space_margin(len) and return SAS_FS_ERR_NO_SPACE
     *    without touching path if the margin would be breached, leaving
     *    any existing file at path unmodified.
     * 3. Otherwise write to a temp path (path + SAS_FS_TMP_SUFFIX) and
     *    rename() it over path for an atomic replace.
     * 4. Release the mutex on every exit path.
     * 5. The mutex must ensure two tasks writing to the same path
     *    serialize without interleaved bytes. */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_fs_delete(const char *path)
{
    if (path == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Acquire the path-serializing mutex.
     * 2. unlink(path).
     * 3. Return SAS_FS_ERR_NOT_FOUND if path does not exist. */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_fs_rename(const char *old_path, const char *new_path)
{
    if ((old_path == NULL) || (new_path == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Acquire the path-serializing mutex.
     * 2. Return SAS_FS_ERR_NOT_FOUND if old_path does not exist, or
     *    SAS_FS_ERR_ALREADY_EXISTS if new_path already exists.
     * 3. Otherwise rename(old_path, new_path). */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_fs_get_usage(sas_fs_usage_t *out)
{
    if (out == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Populate out->total_bytes/used_bytes/free_bytes via
     *    esp_littlefs_info() for the mounted partition. */
    return SAS_ERR_NOT_SUPPORTED;
}

/* 7. Private functions */

/*!
    \brief   Check whether writing additional_bytes would breach
             SAS_FS_MIN_FREE_MARGIN_BYTES.
*/
static sas_status_t check_free_space_margin(size_t additional_bytes)
{
    (void)additional_bytes;

    /* TODO:
     * 1. Call sas_fs_get_usage() internally.
     * 2. Compare (free_bytes - additional_bytes) against
     *    SAS_FS_MIN_FREE_MARGIN_BYTES.
     * Note: used by sas_fs_write() before any bytes are written so the
     * target file is left unmodified on rejection. */
    return SAS_ERR_NOT_SUPPORTED;
}
