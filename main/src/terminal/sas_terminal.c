/*!
    \file    sas_terminal.c
    \brief   Implementation skeleton for the Terminal module.

    \description
    See sas_terminal.h for the public contract. This file currently contains
    only the lifecycle and command-dispatch API stubs; behaviour is filled
    in per module during implementation, in Development Order.

    \dependencies
    - WebSocket (main/src/web/sas_websocket.h): sas_ws_client_t addressing
      and sas_ws_send_to_client() output streaming.
    - Event Bus (main/src/core/sas_event_bus.h): not yet included.
*/

/* 1. Includes */
#include "sas_terminal.h"

#include <stdbool.h>
#include <stddef.h>

/* 2. Private macros */
/* (none yet: tokenization limits (max argc) to be defined during
 * implementation) */

/* 3. Private typedefs */
/* (none yet: the static command table and the static SAS_WS_MAX_CLIENTS-
 * sized sas_terminal_session_t pool are to be added during
 * implementation) */

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started     = false;

/* 5. Forward declarations */
static sas_status_t dispatch_command(sas_ws_client_t *client, const char *command_line);

/* 6. Public functions */

sas_status_t sas_terminal_init(void)
{
    /* TODO:
     * 1. Allocate the static SAS_TERM_MAX_COMMANDS-entry command table.
     * 2. Allocate the static SAS_WS_MAX_CLIENTS-sized pool of
     *    sas_terminal_session_t entries.
     * 3. Use static allocation only, with no dynamic allocation after
     *    init. */
    s_initialized = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_terminal_start(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Mark the command table and session pool as accepting dispatch.
     * 2. Command handlers run on the calling task; no dedicated terminal
     *    task is created here. */
    s_started = true;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_terminal_stop(void)
{
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Stop accepting new sas_terminal_execute() calls.
     * 2. Leave the command table and session pool acquired in
     *    sas_terminal_init() untouched (stop() must not release
     *    resources). */
    s_started = false;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_terminal_deinit(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    /* TODO:
     * 1. Release the command table and session pool acquired in
     *    sas_terminal_init().
     * 2. Reset module state to uninitialized. */
    s_initialized = false;
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_terminal_register_command(const sas_terminal_command_t *cmd)
{
    if ((cmd == NULL) || (cmd->handler == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Copy cmd into the next free static command table slot; other
     *    modules call this from their own init/start.
     * 2. Return SAS_ERR_NO_MEM once SAS_TERM_MAX_COMMANDS entries are
     *    already in use. */
    return SAS_ERR_NOT_SUPPORTED;
}

sas_status_t sas_terminal_execute(sas_ws_client_t *client, const char *command_line)
{
    if ((client == NULL) || (command_line == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }

    /* TODO:
     * 1. Where Authentication is enabled, validate the client's session
     *    token via the Authentication module (forward reference to
     *    main/src/auth/sas_auth.h; not included here) before dispatch.
     * 2. Return SAS_TERM_ERR_UNAUTHORIZED on failure.
     * 3. Otherwise forward to dispatch_command(), which tokenizes
     *    command_line, looks up argv[0] in the static command table, and
     *    streams output back to client. */
    return SAS_ERR_NOT_SUPPORTED;
}

/* 7. Private functions */

/*!
    \brief   Tokenize command_line, look up argv[0] in the static command
             table, and dispatch to the matching handler, buffering and
             flushing output to client.
    \param   client       Non-NULL WebSocket client to report output/errors
                          to.
    \param   command_line Non-NULL, NUL-terminated line to tokenize and
                          dispatch.
    \return  SAS_STATUS_OK on success; SAS_TERM_ERR_CMD_NOT_FOUND if
             argv[0] is not registered (nothing is executed);
             SAS_TERM_ERR_OUTPUT_TRUNCATED if output reached
             SAS_TERM_MAX_OUTPUT_LEN before completing.
*/
static sas_status_t dispatch_command(sas_ws_client_t *client, const char *command_line)
{
    (void)client;
    (void)command_line;

    /* TODO:
     * 1. Tokenize command_line into argc/argv.
     * 2. Look up argv[0] in the static command table; if no entry
     *    matches, send an error string to client via
     *    sas_ws_send_to_client() without invoking any handler and return
     *    SAS_TERM_ERR_CMD_NOT_FOUND.
     * 3. Look up (or allocate from) the per-client sas_terminal_session_t
     *    in the static SAS_WS_MAX_CLIENTS-sized pool so output buffering
     *    stays isolated per client.
     * 4. Invoke the matched handler and flush session->output_buf to
     *    client incrementally via sas_ws_send_to_client(client,
     *    SAS_WS_MSG_TERMINAL_OUTPUT, ...) as output is produced, rather
     *    than accumulating the full response before sending.
     * 5. Once session->output_len reaches SAS_TERM_MAX_OUTPUT_LEN,
     *    discard further output, report SAS_TERM_ERR_OUTPUT_TRUNCATED to
     *    client instead of silently dropping it, and return
     *    SAS_TERM_ERR_OUTPUT_TRUNCATED. */
    return SAS_ERR_NOT_SUPPORTED;
}
