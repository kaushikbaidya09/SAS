/*!
    \file    sas_file_manager.h
    \brief   Public interface for the File Manager module.

    \description
    LittleFS-backed file storage: directory listing, read/write/delete/rename,
    and free-space accounting. sas_fs_write() performs an atomic
    write-then-rename replace and pre-checks the free-space margin before any
    bytes are written; sas_fs_write_begin()/append()/commit() give the same
    guarantees to a caller that receives the data in pieces (e.g. an HTTP
    upload too large to buffer in RAM).

    \dependencies
    (none: File Manager is a Layer-1 module with no compile-time header deps.
    It deliberately does not depend on Configuration Manager, to avoid a
    bootstrap cycle; a first-boot marker-file mechanism is used in its
    place.)
*/

#pragma once

/* 1. Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 2. ESP-IDF includes */
/* (none) */

/* 3. Project includes */
#include "../core/sas_status.h"

/* 4. Public types */

/*! One directory entry as returned by sas_fs_list(). */
typedef struct {
    char name[64];     ///< Entry name (see SAS_FS_MAX_NAME_LEN).
    size_t size_bytes; ///< File size in bytes; undefined for directories.
    bool is_dir;       ///< true if this entry is a directory.
} sas_file_entry_t;

/*! Filesystem usage snapshot as returned by sas_fs_get_usage(). */
typedef struct {
    uint32_t total_bytes; ///< Total partition capacity in bytes.
    uint32_t used_bytes;  ///< Bytes currently in use.
    uint32_t free_bytes;  ///< Bytes currently free.
} sas_fs_usage_t;

/* 5. Public constants */

#define SAS_FS_ERR_BASE (0x8500U) ///< Custom status base for this module.
#define SAS_FS_ERR_NO_SPACE                                                                        \
    (SAS_FS_ERR_BASE +                                                                             \
     1U) ///< sas_fs_write() would breach the free-space margin; target file left unmodified.
#define SAS_FS_ERR_NOT_FOUND                                                                       \
    (SAS_FS_ERR_BASE + 2U) ///< Path does not exist (sas_fs_read(), sas_fs_rename()).
#define SAS_FS_ERR_ALREADY_EXISTS                                                                  \
    (SAS_FS_ERR_BASE + 3U) ///< Destination path already exists (sas_fs_rename()).

#define SAS_FS_MAX_NAME_LEN                                                                        \
    (64U) ///< Maximum length, including the NUL terminator, of a sas_file_entry_t name.
#define SAS_FS_MIN_FREE_MARGIN_BYTES                                                               \
    (16384U) ///< Placeholder minimum free-space margin enforced by sas_fs_write(); size TBD.

/* 6. Public API declarations */

/*!
    \brief   Initialize the File Manager module and mount the backing
             LittleFS partition.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if already
             initialized.
*/
sas_status_t sas_fs_init(void);

/*!
    \brief   Start active File Manager operation.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_fs_start(void);

/*!
    \brief   Stop active File Manager operation without releasing resources.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_fs_stop(void);

/*!
    \brief   Release all resources acquired by sas_fs_init(), including
             unmounting the backing LittleFS partition.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not
             initialized.
*/
sas_status_t sas_fs_deinit(void);

/*!
    \brief   List directory entries under dir_path.
    \param   dir_path    Non-NULL directory path.
    \param   out_array   Non-NULL destination array of at least max_entries.
    \param   max_entries Capacity of out_array.
    \param   out_count   Non-NULL; receives the number of entries written.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if any pointer
             argument is NULL; SAS_FS_ERR_NOT_FOUND if dir_path does not
             exist.
*/
sas_status_t sas_fs_list(const char *dir_path, sas_file_entry_t *out_array, uint16_t max_entries,
                         uint16_t *out_count);

/*!
    \brief   Read up to buf_len bytes of a file starting at offset; callers
             needing the whole file loop, advancing offset by *out_len each
             call until *out_len < buf_len (short read: end of file).
    \param   path     Non-NULL file path.
    \param   offset   Byte offset to start reading from.
    \param   out_buf  Non-NULL destination buffer.
    \param   buf_len  Capacity of out_buf in bytes.
    \param   out_len  Non-NULL; receives the number of bytes read (0 if
                       offset is at or past end of file).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if any pointer
             argument is NULL; SAS_FS_ERR_NOT_FOUND if path does not exist.
*/
sas_status_t sas_fs_read(const char *path, size_t offset, uint8_t *out_buf, size_t buf_len,
                         size_t *out_len);

/*!
    \brief   Atomically write data to path (write-temp-then-rename replace),
             pre-checked against SAS_FS_MIN_FREE_MARGIN_BYTES.
    \param   path Non-NULL destination path; missing parent directories are
                  created.
    \param   data Non-NULL source buffer.
    \param   len  Length of data in bytes.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if path or data is
             NULL; SAS_FS_ERR_NO_SPACE if the write would breach the
             free-space margin (target file left unmodified).
*/
sas_status_t sas_fs_write(const char *path, const uint8_t *data, size_t len);

/*!
    \brief   Begin a streamed atomic write to path: data goes to a temp file
             that only replaces path on sas_fs_write_commit(). One streamed
             write may be open at a time.
    \param   path         Non-NULL destination path; missing parent
                          directories are created.
    \param   expected_len Total size the caller intends to write, used for
                          the free-space pre-check (0 if unknown).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if path is NULL or
             too long; SAS_ERR_INVALID_STATE if a streamed write is already
             open; SAS_FS_ERR_NO_SPACE if expected_len would breach the
             free-space margin or the temp file cannot be created.
*/
sas_status_t sas_fs_write_begin(const char *path, size_t expected_len);

/*!
    \brief   Append data to the streamed write opened by sas_fs_write_begin().
    \param   data Non-NULL source buffer.
    \param   len  Length of data in bytes.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if data is NULL;
             SAS_ERR_INVALID_STATE if no streamed write is open;
             SAS_FS_ERR_NO_SPACE if the write fails or would breach the
             free-space margin (the streamed write is aborted).
*/
sas_status_t sas_fs_write_append(const uint8_t *data, size_t len);

/*!
    \brief   Finish the streamed write, atomically replacing the destination.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if no streamed
             write is open; SAS_FS_ERR_NO_SPACE if the final rename fails
             (target file left unmodified).
*/
sas_status_t sas_fs_write_commit(void);

/*!
    \brief   Discard the streamed write, leaving the destination unmodified.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if no streamed
             write is open.
*/
sas_status_t sas_fs_write_abort(void);

/*!
    \brief   Delete a file.
    \param   path Non-NULL file path.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if path is NULL;
             SAS_FS_ERR_NOT_FOUND if path does not exist.
*/
sas_status_t sas_fs_delete(const char *path);

/*!
    \brief   Rename (move) a file.
    \param   old_path Non-NULL existing path.
    \param   new_path Non-NULL destination path.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if old_path or
             new_path is NULL; SAS_FS_ERR_NOT_FOUND if old_path does not
             exist; SAS_FS_ERR_ALREADY_EXISTS if new_path already exists.
*/
sas_status_t sas_fs_rename(const char *old_path, const char *new_path);

/*!
    \brief   Get current filesystem usage.
    \param   out Non-NULL; receives the usage snapshot.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if out is NULL.
*/
sas_status_t sas_fs_get_usage(sas_fs_usage_t *out);
