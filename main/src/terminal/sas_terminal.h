/*!
    \file    sas_terminal.h
    \brief   Public interface for the Terminal module.

    \description
    Interactive command terminal reachable over the WebSocket transport.
    Other modules register named commands via
    sas_terminal_register_command() during their own init/start; remote
    clients invoke them by sending a command line that sas_terminal_execute()
    tokenizes, looks up in the static command table, dispatches, and whose
    output it streams back to the originating client, isolated per client.

    \dependencies
    - WebSocket (main/src/web/sas_websocket.h): owns sas_ws_client_t and
      sas_ws_send_to_client(); every command handler and every terminal
      session in this module is addressed by, and streams output through,
      that client type.
    - Event Bus (main/src/core/sas_event_bus.h): not yet included; reserved
      for a future terminal-session lifecycle event.
*/

#pragma once

/* 1. Standard includes */
#include <stddef.h>

/* 2. ESP-IDF includes */
/* (none) */

/* 3. Project includes */
#include "../core/sas_status.h"   /* sas_status_t */
#include "../web/sas_websocket.h" /* sas_ws_client_t */

/* 4. Public constants */
/* NOTE: ordered ahead of Public types (deviating from the usual
 * types-then-constants section order) because sas_terminal_command_t and
 * sas_terminal_session_t below size their members with these macros; C
 * requires the macros to be defined first. */

#define SAS_TERM_ERR_BASE (0x8C00U) ///< Custom status base for this module.
#define SAS_TERM_ERR_CMD_NOT_FOUND                                                                 \
    (SAS_TERM_ERR_BASE + 1U) ///< sas_terminal_execute() failed: argv[0] did not match any
                             ///< registered command; nothing was executed.
#define SAS_TERM_ERR_UNAUTHORIZED                                                                  \
    (SAS_TERM_ERR_BASE + 2U) ///< sas_terminal_execute() failed: the client's session token did not
                             ///< pass Authentication module validation.
#define SAS_TERM_ERR_OUTPUT_TRUNCATED                                                              \
    (SAS_TERM_ERR_BASE + 3U) ///< Command output reached SAS_TERM_MAX_OUTPUT_LEN before completing;
                             ///< remaining output was discarded and the truncation was reported to
                             ///< the client instead of being silently dropped.

/*!
    Placeholder maximum command name length in bytes, including the NUL
    terminator; to be tuned during implementation against actual registered
    command names.
*/
#define SAS_TERM_MAX_CMD_NAME_LEN (16U)

/*!
    Placeholder maximum number of entries in the static command table; to
    be tuned during implementation against the actual number of
    registering modules.
*/
#define SAS_TERM_MAX_COMMANDS (32U)

/*!
    Placeholder maximum per-client output buffer size in bytes, enforced by
    sas_terminal_execute(); to be tuned during implementation against
    actual command output sizes.
*/
#define SAS_TERM_MAX_OUTPUT_LEN (2048U)

/* 5. Public types */

/*!
    \brief   Command handler registered with sas_terminal_register_command().
    \param   argc   Argument count, including argv[0] (the command name).
    \param   argv   Tokenized argument vector; valid only for the duration
                    of the call.
    \param   client Non-NULL WebSocket client the command was invoked from;
                    handlers write their response to this client.
    \return  SAS_STATUS_OK on success; any other sas_status_t is reported
             to client.
*/
typedef sas_status_t (*sas_terminal_cmd_fn)(int argc, char **argv, sas_ws_client_t *client);

/*! A single command registered in the static command table. */
typedef struct {
    char name[SAS_TERM_MAX_CMD_NAME_LEN]; ///< Name matched against argv[0].
    sas_terminal_cmd_fn handler;          ///< Handler invoked on a match.
} sas_terminal_command_t;

/*!
    Per-client terminal session, isolating output buffering per client.
*/
typedef struct {
    sas_ws_client_t client;                   ///< Owning WebSocket client.
    char output_buf[SAS_TERM_MAX_OUTPUT_LEN]; ///< Output pending flush to client.
    size_t output_len;                        ///< Bytes currently held in output_buf.
} sas_terminal_session_t;

/* 6. Public API declarations */

/*!
    \brief   Initialize the Terminal module.
    \return  SAS_STATUS_OK on success.
*/
sas_status_t sas_terminal_init(void);

/*!
    \brief   Start active terminal operation (command dispatch).
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_terminal_start(void);

/*!
    \brief   Stop active terminal operation without releasing resources.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not started.
*/
sas_status_t sas_terminal_stop(void);

/*!
    \brief   Release all resources acquired by sas_terminal_init().
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_STATE if not initialized.
*/
sas_status_t sas_terminal_deinit(void);

/*!
    \brief   Register a command in the static command table. Other modules
             call this from their own init/start.
    \param   cmd Non-NULL command descriptor; copied internally.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if cmd (or
             cmd->handler) is NULL; SAS_ERR_NO_MEM if SAS_TERM_MAX_COMMANDS
             entries are already registered.
*/
sas_status_t sas_terminal_register_command(const sas_terminal_command_t *cmd);

/*!
    \brief   Tokenize and dispatch a command line to its registered handler,
             streaming output back to client.
    \param   client       Non-NULL WebSocket client the command line was
                          received from.
    \param   command_line Non-NULL, NUL-terminated command line to tokenize
                          and dispatch.
    \return  SAS_STATUS_OK on success; SAS_ERR_INVALID_ARG if client or
             command_line is NULL; SAS_TERM_ERR_CMD_NOT_FOUND if argv[0]
             does not match a registered command (nothing is executed);
             SAS_TERM_ERR_UNAUTHORIZED if the client's session token fails
             Authentication validation; SAS_TERM_ERR_OUTPUT_TRUNCATED if
             command output reached SAS_TERM_MAX_OUTPUT_LEN before
             completing (the truncation is also reported to client).
*/
sas_status_t sas_terminal_execute(sas_ws_client_t *client, const char *command_line);
