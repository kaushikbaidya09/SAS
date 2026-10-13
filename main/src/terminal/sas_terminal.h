//=============================================================================
/*!
    \file    sas_terminal.h
    \brief   Public interface for the Terminal module.
*/
//=============================================================================

// Command terminal reached over the WebSocket. Modules register named commands
// with sas_terminal_register_command(); a client sends a command line and the
// output is streamed back to that client only.
//
// Built-in commands: "help", "sysinfo", "tasks", "boot", "wifi", "reboot".

#ifndef SAS_TERMINAL_H
#define SAS_TERMINAL_H

#include <stddef.h>

#include "../core/sas_status.h"   // sas_status_t
#include "../web/sas_websocket.h" // sas_ws_client_t

// The constants come before the types here because the types below use them
// for their array sizes.

#define SAS_TERM_ERR_BASE (0x8C00U) ///< Custom status base for this module.
/*!
    \brief   sas_terminal_execute() failed: argv[0] did not match any
             registered command; nothing was executed.
*/
#define SAS_TERM_ERR_CMD_NOT_FOUND (SAS_TERM_ERR_BASE + 1U)
/*!
    \brief   sas_terminal_execute() failed: the client's session token did not
             pass Authentication module validation.
*/
#define SAS_TERM_ERR_UNAUTHORIZED (SAS_TERM_ERR_BASE + 2U)
/*!
    \brief   Command output reached SAS_TERM_MAX_OUTPUT_LEN before completing;
             remaining output was discarded and the truncation was reported to
             the client instead of being silently dropped.
*/
#define SAS_TERM_ERR_OUTPUT_TRUNCATED (SAS_TERM_ERR_BASE + 3U)

/*!
    \brief Maximum command name length in bytes, including the NUL terminator.
*/
#define SAS_TERM_MAX_CMD_NAME_LEN (16U)

/*!
    \brief Maximum number of entries in the static command table.
*/
#define SAS_TERM_MAX_COMMANDS (32U)

/*!
    \brief Maximum output, in bytes, one command may produce; enforced by
    sas_terminal_execute(). Anything beyond it is discarded and reported to
    the client as truncated.
*/
#define SAS_TERM_MAX_OUTPUT_LEN (2048U)

#define SAS_TERM_MAX_ARGS     (8U) ///< Most arguments (including argv[0]) a command line may hold.
#define SAS_TERM_MAX_LINE_LEN (128U) ///< Longest command line accepted, including the NUL.

/*!
    \brief   Command handler registered with sas_terminal_register_command().
    \param   argc   Argument count, including argv[0] (the command name).
    \param   argv   Tokenized argument vector; valid only for the duration
                    of the call.
    \param   client Non-NULL WebSocket client the command was invoked from;
                    handlers write their response to this client with
                    sas_terminal_printf().
    \return  SAS_STATUS_OK on success; any other sas_status_t is reported
             to client.
*/
typedef sas_status_t (*sas_terminal_cmd_fn)(int argc, char **argv, sas_ws_client_t *client);

/*! \brief A single command registered in the static command table. */
typedef struct
{
    char name[SAS_TERM_MAX_CMD_NAME_LEN]; ///< Name matched against argv[0].
    sas_terminal_cmd_fn handler;          ///< Handler invoked on a match.
} sas_terminal_command_t;

/*!
    \brief Per-client terminal session, isolating output buffering per client.
*/
typedef struct
{
    sas_ws_client_t client;                   ///< Owning WebSocket client.
    char output_buf[SAS_TERM_MAX_OUTPUT_LEN]; ///< Output pending flush to client.
    size_t output_len;                        ///< Bytes currently held in output_buf.
} sas_terminal_session_t;

sas_status_t sas_terminal_init(void);

sas_status_t sas_terminal_start(void);

sas_status_t sas_terminal_stop(void);

sas_status_t sas_terminal_deinit(void);

sas_status_t sas_terminal_register_command(const sas_terminal_command_t *cmd);

sas_status_t sas_terminal_execute(sas_ws_client_t *client, const char *command_line);

sas_status_t sas_terminal_printf(sas_ws_client_t *client, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* SAS_TERMINAL_H */
