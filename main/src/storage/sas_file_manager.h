//=============================================================================
/*!
    \file    sas_file_manager.h
    \brief   Public interface for the File Manager module.
*/
//=============================================================================

// File storage on LittleFS: directory listing, read, write, delete, rename and
// free-space reporting. Writes replace the file atomically and are refused
// when they would leave too little free space. sas_fs_write_begin()/append()/
// commit() do the same for data that arrives in pieces.

#ifndef SAS_FILE_MANAGER_H
#define SAS_FILE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/sas_status.h"

/*! \brief One directory entry as returned by sas_fs_list(). */
typedef struct
{
    char name[64];     ///< Entry name (see SAS_FS_MAX_NAME_LEN).
    size_t size_bytes; ///< File size in bytes; undefined for directories.
    bool is_dir;       ///< true if this entry is a directory.
} sas_file_entry_t;

/*! \brief Filesystem usage snapshot as returned by sas_fs_get_usage(). */
typedef struct
{
    uint32_t total_bytes; ///< Total partition capacity in bytes.
    uint32_t used_bytes;  ///< Bytes currently in use.
    uint32_t free_bytes;  ///< Bytes currently free.
} sas_fs_usage_t;

#define SAS_FS_ERR_BASE (0x8500U) ///< Custom status base for this module.
/*! \brief sas_fs_write() would breach the free-space margin; target file left unmodified. */
#define SAS_FS_ERR_NO_SPACE (SAS_FS_ERR_BASE + 1U)
/*! \brief Path does not exist (sas_fs_read(), sas_fs_rename()). */
#define SAS_FS_ERR_NOT_FOUND (SAS_FS_ERR_BASE + 2U)
/*! \brief Destination path already exists (sas_fs_rename()). */
#define SAS_FS_ERR_ALREADY_EXISTS (SAS_FS_ERR_BASE + 3U)

/*! \brief Maximum length, including the NUL terminator, of a sas_file_entry_t name. */
#define SAS_FS_MAX_NAME_LEN (64U)
/*! \brief Free space, in bytes, that a write must leave on the filesystem. */
#define SAS_FS_MIN_FREE_MARGIN_BYTES (16384U)

sas_status_t sas_fs_init(void);

sas_status_t sas_fs_start(void);

sas_status_t sas_fs_stop(void);

sas_status_t sas_fs_deinit(void);

sas_status_t sas_fs_list(const char *dir_path, sas_file_entry_t *out_array, uint16_t max_entries,
                         uint16_t *out_count);

sas_status_t sas_fs_read(const char *path, size_t offset, uint8_t *out_buf, size_t buf_len,
                         size_t *out_len);

sas_status_t sas_fs_write(const char *path, const uint8_t *data, size_t len);

sas_status_t sas_fs_write_begin(const char *path, size_t expected_len);

sas_status_t sas_fs_write_append(const uint8_t *data, size_t len);

sas_status_t sas_fs_write_commit(void);

sas_status_t sas_fs_write_abort(void);

sas_status_t sas_fs_delete(const char *path);

sas_status_t sas_fs_rename(const char *old_path, const char *new_path);

sas_status_t sas_fs_get_usage(sas_fs_usage_t *out);

#endif /* SAS_FILE_MANAGER_H */
