/*!
    \file    sas_file_manager.c
    \brief   Implementation of the File Manager module.

    \description
    Mounts the sole LittleFS partition (label "littlefs", see
    partitions/partitions-esp32-s3.csv and the root CMakeLists.txt's
    littlefs_create_partition_image() call) directly at SAS_FS_MOUNT_POINT
    ("/www"). Every path/dir_path argument accepted by this module's public
    API is a literal, already-qualified VFS path under that mount point
    (e.g. "/www/index.html") - this module does no implicit prefixing;
    callers (e.g. HTTP Server's default_static_handler()) are responsible
    for qualifying the paths they pass in.
*/

/* 1. Includes */
#include "sas_file_manager.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sas_fs";

/* 2. Private macros */
#define SAS_FS_MOUNT_POINT "/www"       ///< Sole LittleFS mount point; see file header doc above.
#define SAS_FS_PARTITION_LABEL "littlefs" ///< Matches partitions/partitions-esp32-s3.csv.

#define SAS_FS_READY_MARKER_PATH \
    "/.sas_fs_ready" ///< Reserved marker file used to detect first-boot / decide
                     ///< format_if_mount_failed.
#define SAS_FS_TMP_SUFFIX \
    ".tmp"                         ///< Temp path suffix used by sas_fs_write() for the write-then-rename replace.
#define SAS_FS_TMP_SUFFIX_LEN (4U) ///< strlen(SAS_FS_TMP_SUFFIX); sizes the temp-path buffer.

#define SAS_FS_PATH_MAX_LEN (128U) ///< Longest path (including NUL) this module builds internally.

#define SAS_FS_LOCK_WAIT_TICKS \
    pdMS_TO_TICKS(1000) ///< Bounded wait for s_path_lock; write()/delete()/rename() touch flash
                        ///< and can legitimately take longer than the short in-RAM critical
                        ///< sections used elsewhere in this project.

/* 3. Private typedefs */
/* (none) */

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;

static SemaphoreHandle_t s_path_lock = NULL;
static StaticSemaphore_t s_path_lock_storage;

/* Streamed write state (sas_fs_write_begin()/append()/commit()/abort()); guarded by s_path_lock. */
static FILE *s_stream_file = NULL;                ///< Open temp file; NULL if no stream is open.
static char s_stream_path[SAS_FS_PATH_MAX_LEN];   ///< Final destination path.
static char s_stream_tmp_path[SAS_FS_PATH_MAX_LEN + SAS_FS_TMP_SUFFIX_LEN]; ///< Temp path.

/* 5. Forward declarations */
static sas_status_t check_free_space_margin(size_t additional_bytes);
static bool path_exists(const char *path, bool *out_is_dir);
static void ensure_parent_dirs(const char *path);

/* 6. Public functions */

sas_status_t sas_fs_init(void)
{
    if (s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = SAS_FS_MOUNT_POINT,
        .partition_label = SAS_FS_PARTITION_LABEL,
        .format_if_mount_failed = false,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK)
    {
        /* Mount failed on the probe attempt above: this module has no way
         * to check SAS_FS_READY_MARKER_PATH without first mounting
         * successfully (it deliberately has no Configuration Manager
         * dependency to fall back on, see this module's header), so a
         * failed probe mount is this module's operational definition of
         * "first boot" / not-yet-formatted. */
        conf.format_if_mount_failed = true;
        err = esp_vfs_littlefs_register(&conf);
    }
    configASSERT(err == ESP_OK);

    s_path_lock = xSemaphoreCreateMutexStatic(&s_path_lock_storage);
    configASSERT(s_path_lock != NULL);

    const char *marker_path = SAS_FS_MOUNT_POINT SAS_FS_READY_MARKER_PATH;
    bool marker_is_dir = false;
    if (!path_exists(marker_path, &marker_is_dir))
    {
        FILE *marker = fopen(marker_path, "w");
        if (marker != NULL)
        {
            (void)fclose(marker);
        }
        else
        {
            /* Not a SAS module dependency: ESP-IDF's own logging facility, used directly per
             * this module's documented "no dependency on any other SAS module" contract. */
            ESP_LOGW(TAG, "failed to create %s", marker_path);
        }
    }

    s_started = false;
    s_initialized = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_fs_start(void)
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
    return SAS_STATUS_OK;
}

sas_status_t sas_fs_stop(void)
{
    if (!s_started)
    {
        return SAS_ERR_INVALID_STATE;
    }

    s_started = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_fs_deinit(void)
{
    if (!s_initialized)
    {
        return SAS_ERR_INVALID_STATE;
    }

    if (s_path_lock != NULL)
    {
        vSemaphoreDelete(s_path_lock);
        s_path_lock = NULL;
    }
    (void)esp_vfs_littlefs_unregister(SAS_FS_PARTITION_LABEL);

    s_initialized = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_fs_list(const char *dir_path, sas_file_entry_t *out_array, uint16_t max_entries,
                         uint16_t *out_count)
{
    if ((dir_path == NULL) || (out_array == NULL) || (out_count == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    DIR *dir = opendir(dir_path);
    if (dir == NULL)
    {
        return SAS_FS_ERR_NOT_FOUND;
    }

    uint16_t count = 0U;
    struct dirent *entry;
    while (((entry = readdir(dir)) != NULL) && (count < max_entries))
    {
        char full_path[128];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (ret < 0 || (size_t)ret >= sizeof(full_path))
        {
            // Path was too long or snprintf failed.
            // Handle appropriately.
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0)
        {
            continue; /* entry vanished between readdir() and stat(); skip it */
        }

        (void)strncpy(out_array[count].name, entry->d_name, SAS_FS_MAX_NAME_LEN - 1U);
        out_array[count].name[SAS_FS_MAX_NAME_LEN - 1U] = '\0';
        out_array[count].is_dir = S_ISDIR(st.st_mode);
        out_array[count].size_bytes = out_array[count].is_dir ? 0U : (size_t)st.st_size;
        count++;
    }
    (void)closedir(dir);

    *out_count = count;
    return SAS_STATUS_OK;
}

sas_status_t sas_fs_read(const char *path, size_t offset, uint8_t *out_buf, size_t buf_len,
                         size_t *out_len)
{
    if ((path == NULL) || (out_buf == NULL) || (out_len == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        return SAS_FS_ERR_NOT_FOUND;
    }

    if ((offset > 0U) && (fseek(f, (long)offset, SEEK_SET) != 0))
    {
        (void)fclose(f);
        return SAS_FS_ERR_NOT_FOUND;
    }

    *out_len = fread(out_buf, 1U, buf_len, f);
    (void)fclose(f);
    return SAS_STATUS_OK;
}

sas_status_t sas_fs_write(const char *path, const uint8_t *data, size_t len)
{
    if ((path == NULL) || (data == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_path_lock, SAS_FS_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = check_free_space_margin(len);
    if (status != SAS_STATUS_OK)
    {
        xSemaphoreGive(s_path_lock);
        return status;
    }

    char tmp_path[SAS_FS_PATH_MAX_LEN + SAS_FS_TMP_SUFFIX_LEN];
    (void)snprintf(tmp_path, sizeof(tmp_path), "%s%s", path, SAS_FS_TMP_SUFFIX);

    ensure_parent_dirs(path);
    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL)
    {
        xSemaphoreGive(s_path_lock);
        return SAS_FS_ERR_NO_SPACE;
    }

    size_t written = fwrite(data, 1U, len, f);
    (void)fclose(f);

    if (written != len)
    {
        (void)unlink(tmp_path);
        xSemaphoreGive(s_path_lock);
        return SAS_FS_ERR_NO_SPACE;
    }

    if (rename(tmp_path, path) != 0)
    {
        (void)unlink(tmp_path);
        xSemaphoreGive(s_path_lock);
        return SAS_FS_ERR_NO_SPACE;
    }

    xSemaphoreGive(s_path_lock);
    return SAS_STATUS_OK;
}

sas_status_t sas_fs_write_begin(const char *path, size_t expected_len)
{
    if ((path == NULL) || (strlen(path) >= SAS_FS_PATH_MAX_LEN))
    {
        return SAS_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_path_lock, SAS_FS_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    if (s_stream_file != NULL)
    {
        status = SAS_ERR_INVALID_STATE;
    }
    else
    {
        status = check_free_space_margin(expected_len);
    }

    if (status == SAS_STATUS_OK)
    {
        (void)snprintf(s_stream_path, sizeof(s_stream_path), "%s", path);
        (void)snprintf(s_stream_tmp_path, sizeof(s_stream_tmp_path), "%s%s", path,
                       SAS_FS_TMP_SUFFIX);

        ensure_parent_dirs(path);
        s_stream_file = fopen(s_stream_tmp_path, "wb");
        if (s_stream_file == NULL)
        {
            status = SAS_FS_ERR_NO_SPACE;
        }
    }

    xSemaphoreGive(s_path_lock);
    return status;
}

sas_status_t sas_fs_write_append(const uint8_t *data, size_t len)
{
    if (data == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_path_lock, SAS_FS_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    if (s_stream_file == NULL)
    {
        status = SAS_ERR_INVALID_STATE;
    }
    else
    {
        status = check_free_space_margin(len);
        if ((status == SAS_STATUS_OK) && (fwrite(data, 1U, len, s_stream_file) != len))
        {
            status = SAS_FS_ERR_NO_SPACE;
        }
        if (status != SAS_STATUS_OK)
        {
            /* Out of space mid-stream: drop the partial temp file so it cannot linger. */
            (void)fclose(s_stream_file);
            s_stream_file = NULL;
            (void)unlink(s_stream_tmp_path);
            status = SAS_FS_ERR_NO_SPACE;
        }
    }

    xSemaphoreGive(s_path_lock);
    return status;
}

sas_status_t sas_fs_write_commit(void)
{
    if (xSemaphoreTake(s_path_lock, SAS_FS_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    if (s_stream_file == NULL)
    {
        status = SAS_ERR_INVALID_STATE;
    }
    else
    {
        int close_result = fclose(s_stream_file);
        s_stream_file = NULL;
        if ((close_result != 0) || (rename(s_stream_tmp_path, s_stream_path) != 0))
        {
            (void)unlink(s_stream_tmp_path);
            status = SAS_FS_ERR_NO_SPACE;
        }
    }

    xSemaphoreGive(s_path_lock);
    return status;
}

sas_status_t sas_fs_write_abort(void)
{
    if (xSemaphoreTake(s_path_lock, SAS_FS_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    if (s_stream_file == NULL)
    {
        status = SAS_ERR_INVALID_STATE;
    }
    else
    {
        (void)fclose(s_stream_file);
        s_stream_file = NULL;
        (void)unlink(s_stream_tmp_path);
    }

    xSemaphoreGive(s_path_lock);
    return status;
}

sas_status_t sas_fs_delete(const char *path)
{
    if (path == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_path_lock, SAS_FS_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    if (unlink(path) != 0)
    {
        status = SAS_FS_ERR_NOT_FOUND;
    }

    xSemaphoreGive(s_path_lock);
    return status;
}

sas_status_t sas_fs_rename(const char *old_path, const char *new_path)
{
    if ((old_path == NULL) || (new_path == NULL))
    {
        return SAS_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_path_lock, SAS_FS_LOCK_WAIT_TICKS) != pdTRUE)
    {
        return SAS_ERR_TIMEOUT;
    }

    sas_status_t status = SAS_STATUS_OK;
    bool unused_is_dir = false;
    if (!path_exists(old_path, &unused_is_dir))
    {
        status = SAS_FS_ERR_NOT_FOUND;
    }
    else if (path_exists(new_path, &unused_is_dir))
    {
        status = SAS_FS_ERR_ALREADY_EXISTS;
    }
    else if (rename(old_path, new_path) != 0)
    {
        status = SAS_FS_ERR_NOT_FOUND;
    }
    else
    {
        /* renamed successfully */
    }

    xSemaphoreGive(s_path_lock);
    return status;
}

sas_status_t sas_fs_get_usage(sas_fs_usage_t *out)
{
    if (out == NULL)
    {
        return SAS_ERR_INVALID_ARG;
    }

    size_t total = 0U;
    size_t used = 0U;
    if (esp_littlefs_info(SAS_FS_PARTITION_LABEL, &total, &used) != ESP_OK)
    {
        return SAS_ERR_INVALID_STATE;
    }

    out->total_bytes = (uint32_t)total;
    out->used_bytes = (uint32_t)used;
    out->free_bytes = (total > used) ? (uint32_t)(total - used) : 0U;
    return SAS_STATUS_OK;
}

/* 7. Private functions */

/*!
    \brief   Check whether writing additional_bytes would breach
             SAS_FS_MIN_FREE_MARGIN_BYTES.
*/
static sas_status_t check_free_space_margin(size_t additional_bytes)
{
    sas_fs_usage_t usage;
    sas_status_t status = sas_fs_get_usage(&usage);
    if (status != SAS_STATUS_OK)
    {
        return status;
    }

    if ((usage.free_bytes < additional_bytes) ||
        ((usage.free_bytes - additional_bytes) < SAS_FS_MIN_FREE_MARGIN_BYTES))
    {
        return SAS_FS_ERR_NO_SPACE;
    }
    return SAS_STATUS_OK;
}

/*! \brief stat() wrapper reporting existence without treating a missing path as an error. */
static bool path_exists(const char *path, bool *out_is_dir)
{
    struct stat st;
    if (stat(path, &st) != 0)
    {
        return false;
    }
    *out_is_dir = S_ISDIR(st.st_mode);
    return true;
}

/*! \brief Create every missing directory between the mount point and path's final component
 * (mkdir -p on the parent). Failures are left for the caller's fopen() to surface. */
static void ensure_parent_dirs(const char *path)
{
    char dir[SAS_FS_PATH_MAX_LEN];
    size_t len = strlen(path);
    if (len >= sizeof(dir))
    {
        return;
    }
    (void)memcpy(dir, path, len + 1U);

    /* Skip the mount point itself, then create one component at a time. */
    for (size_t i = strlen(SAS_FS_MOUNT_POINT) + 1U; i < len; i++)
    {
        if (dir[i] == '/')
        {
            dir[i] = '\0';
            if ((mkdir(dir, 0775) != 0) && (errno != EEXIST))
            {
                return;
            }
            dir[i] = '/';
        }
    }
}
