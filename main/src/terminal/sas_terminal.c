/*!
    \file    sas_terminal.c
    \brief   Implementation of the Terminal module.

    \description
    See sas_terminal.h for the public contract. Commands arrive as
    "terminal_cmd" WebSocket messages (claimed from the WebSocket module in
    sas_terminal_start()) and run to completion on the HTTP Server's task;
    no dedicated terminal task exists.

    \dependencies
    - WebSocket (main/src/web/sas_websocket.h): sas_ws_client_t addressing,
      the "terminal_cmd" message hook, and sas_ws_send_to_client() output
      streaming.
    - Authentication (main/src/auth/sas_auth.h): per-command token check.
    - System Information (main/src/core/sas_sysinfo.h), Wi-Fi Manager
      (main/src/network/sas_wifi_manager.h): built-in commands.
*/

/* 1. Includes */
#include "sas_terminal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "esp_timer.h"

#include "../auth/sas_auth.h"
#include "../core/sas_logger.h"
#include "../core/sas_sysinfo.h"
#include "../network/sas_wifi_manager.h"

static const char *TAG = "sas_term";

/* 2. Private macros */
#define SAS_TERM_WS_MSG_TYPE "terminal_cmd" ///< Client-to-device message this module claims.

/* JSON-escaped bytes of output text one "terminal_output" message may carry: a message is a JSON
 * envelope capped at SAS_WS_MESSAGE_MAX_LEN, less the envelope's own fields. Output is restricted
 * to printable ASCII plus '\n' and '\t' (see append_output()), so a byte escapes to at most two. */
#define SAS_TERM_CHUNK_BUDGET (SAS_WS_MESSAGE_MAX_LEN - 96U)

#define SAS_TERM_REBOOT_DELAY_US (750000ULL) ///< Lets "reboot"'s own output reach the client.

/* 3. Private typedefs */

/*! Flush bookkeeping kept alongside each sas_terminal_session_t. */
typedef struct {
    bool in_use;        ///< false if this session slot is free.
    bool executing;     ///< true only while a command handler is running for this client.
    bool truncated;     ///< Output hit SAS_TERM_MAX_OUTPUT_LEN during the current command.
    size_t flushed_len; ///< Bytes of output_buf already sent to the client.
    uint32_t last_used; ///< s_use_counter value at the client's most recent command.
} sas_terminal_session_state_t;

/* 4. Private variables */
static bool s_initialized = false;
static bool s_started = false;

static sas_terminal_command_t s_commands[SAS_TERM_MAX_COMMANDS];
static size_t s_command_count = 0U;

static sas_terminal_session_t s_sessions[SAS_WS_MAX_CLIENTS];
static sas_terminal_session_state_t s_session_state[SAS_WS_MAX_CLIENTS];
static uint32_t s_use_counter = 0U;

/* 5. Forward declarations */
static sas_status_t dispatch_command(sas_ws_client_t *client, const char *command_line);
static void on_terminal_cmd(const sas_ws_client_t *client, const cJSON *data);
static sas_status_t cmd_help(int argc, char **argv, sas_ws_client_t *client);
static sas_status_t cmd_sysinfo(int argc, char **argv, sas_ws_client_t *client);
static sas_status_t cmd_tasks(int argc, char **argv, sas_ws_client_t *client);
static sas_status_t cmd_boot(int argc, char **argv, sas_ws_client_t *client);
static sas_status_t cmd_wifi(int argc, char **argv, sas_ws_client_t *client);
static sas_status_t cmd_reboot(int argc, char **argv, sas_ws_client_t *client);
static void reboot_timer_callback(void *arg);
static size_t chunk_fit(const char *text, size_t len);
static size_t acquire_session(const sas_ws_client_t *client);
static size_t find_session(const sas_ws_client_t *client);
static void append_output(size_t index, const char *text, size_t len);
static void flush_output(size_t index, bool final);
static void send_chunk(size_t index, const char *text, size_t len, bool truncated);
static const sas_terminal_command_t *find_command(const char *name);
static int tokenize(char *line, char **argv, int max_args);

/* 6. Public functions */

sas_status_t sas_terminal_init(void)
{
    if (s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    (void)memset(s_commands, 0, sizeof(s_commands));
    (void)memset(s_sessions, 0, sizeof(s_sessions));
    (void)memset(s_session_state, 0, sizeof(s_session_state));
    s_command_count = 0U;
    s_use_counter = 0U;

    s_started = false;
    s_initialized = true;

    static const sas_terminal_command_t builtins[] = {
        {.name = "help", .handler = cmd_help},     {.name = "sysinfo", .handler = cmd_sysinfo},
        {.name = "tasks", .handler = cmd_tasks},   {.name = "boot", .handler = cmd_boot},
        {.name = "wifi", .handler = cmd_wifi},     {.name = "reboot", .handler = cmd_reboot},
    };
    for (size_t i = 0U; i < (sizeof(builtins) / sizeof(builtins[0])); i++) {
        (void)sas_terminal_register_command(&builtins[i]);
    }
    return SAS_STATUS_OK;
}

sas_status_t sas_terminal_start(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    sas_status_t status = sas_ws_register_message_handler(SAS_TERM_WS_MSG_TYPE, on_terminal_cmd);
    if (status != SAS_STATUS_OK) {
        return status;
    }

    s_started = true;
    return SAS_STATUS_OK;
}

sas_status_t sas_terminal_stop(void)
{
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    /* The command table and session pool are kept; only dispatch stops. */
    s_started = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_terminal_deinit(void)
{
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }

    (void)memset(s_commands, 0, sizeof(s_commands));
    (void)memset(s_sessions, 0, sizeof(s_sessions));
    (void)memset(s_session_state, 0, sizeof(s_session_state));
    s_command_count = 0U;

    s_started = false;
    s_initialized = false;
    return SAS_STATUS_OK;
}

sas_status_t sas_terminal_register_command(const sas_terminal_command_t *cmd)
{
    if ((cmd == NULL) || (cmd->handler == NULL) || (cmd->name[0] == '\0')) {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return SAS_ERR_INVALID_STATE;
    }
    if (s_command_count >= SAS_TERM_MAX_COMMANDS) {
        return SAS_ERR_NO_MEM;
    }

    s_commands[s_command_count] = *cmd;
    s_commands[s_command_count].name[SAS_TERM_MAX_CMD_NAME_LEN - 1U] = '\0';
    s_command_count++;
    return SAS_STATUS_OK;
}

sas_status_t sas_terminal_execute(sas_ws_client_t *client, const char *command_line)
{
    if ((client == NULL) || (command_line == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return SAS_ERR_INVALID_STATE;
    }

    /* SAS-TERM-FR-005: checked on every command, so a session that expired or was logged out
     * after the WebSocket connected is rejected too. */
    if (sas_auth_is_enabled()) {
        char token[33];
        if ((sas_ws_get_client_token(client, token, sizeof(token)) != SAS_STATUS_OK) ||
            (sas_auth_validate_token(token) != SAS_STATUS_OK)) {
            size_t index = acquire_session(client);
            static const char message[] = "error: not authenticated - log in first\n";
            append_output(index, message, sizeof(message) - 1U);
            flush_output(index, true);
            return SAS_TERM_ERR_UNAUTHORIZED;
        }
    }

    return dispatch_command(client, command_line);
}

sas_status_t sas_terminal_printf(sas_ws_client_t *client, const char *fmt, ...)
{
    if ((client == NULL) || (fmt == NULL)) {
        return SAS_ERR_INVALID_ARG;
    }

    size_t index = find_session(client);
    if ((index >= SAS_WS_MAX_CLIENTS) || !s_session_state[index].executing) {
        return SAS_ERR_INVALID_STATE;
    }

    char text[160];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (written < 0) {
        return SAS_ERR_INVALID_ARG;
    }

    size_t len = ((size_t)written < sizeof(text)) ? (size_t)written : (sizeof(text) - 1U);
    append_output(index, text, len);
    flush_output(index, false);
    return s_session_state[index].truncated ? SAS_TERM_ERR_OUTPUT_TRUNCATED : SAS_STATUS_OK;
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
    size_t index = acquire_session(client);
    sas_terminal_session_state_t *state = &s_session_state[index];

    char line[SAS_TERM_MAX_LINE_LEN];
    if (strlen(command_line) >= sizeof(line)) {
        static const char message[] = "error: command line too long\n";
        append_output(index, message, sizeof(message) - 1U);
        flush_output(index, true);
        return SAS_ERR_INVALID_ARG;
    }
    (void)strcpy(line, command_line);

    char *argv[SAS_TERM_MAX_ARGS];
    int argc = tokenize(line, argv, (int)SAS_TERM_MAX_ARGS);
    if (argc == 0) {
        flush_output(index, true); /* empty line: answer with an empty, final message */
        return SAS_STATUS_OK;
    }

    const sas_terminal_command_t *cmd = find_command(argv[0]);
    if (cmd == NULL) {
        /* SAS-TERM-FR-004: report the error, execute nothing. */
        state->executing = true;
        (void)sas_terminal_printf(client, "error: unknown command '%s' (try 'help')\n", argv[0]);
        state->executing = false;
        flush_output(index, true);
        return SAS_TERM_ERR_CMD_NOT_FOUND;
    }

    state->executing = true;
    sas_status_t status = cmd->handler(argc, argv, client);
    if (status != SAS_STATUS_OK) {
        (void)sas_terminal_printf(client, "error: '%s' failed (0x%x)\n", argv[0],
                                  (unsigned int)status);
    }
    state->executing = false;

    bool truncated = state->truncated;
    flush_output(index, true);
    if (truncated) {
        sas_log_warn("output of '%s' truncated at %u bytes", argv[0],
                     (unsigned int)SAS_TERM_MAX_OUTPUT_LEN);
        return SAS_TERM_ERR_OUTPUT_TRUNCATED;
    }
    return status;
}

/*! \brief "terminal_cmd" WebSocket message handler: data is { "command": "<line>" }. */
static void on_terminal_cmd(const sas_ws_client_t *client, const cJSON *data)
{
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(data, "command");
    if (!cJSON_IsString(command) || (command->valuestring == NULL)) {
        return;
    }

    sas_ws_client_t target = *client;
    (void)sas_terminal_execute(&target, command->valuestring);
}

/*! \brief Built-in "help": list every registered command. */
static sas_status_t cmd_help(int argc, char **argv, sas_ws_client_t *client)
{
    (void)argc;
    (void)argv;

    (void)sas_terminal_printf(client, "commands:\n");
    for (size_t i = 0U; i < s_command_count; i++) {
        (void)sas_terminal_printf(client, "  %s\n", s_commands[i].name);
    }
    return SAS_STATUS_OK;
}

/*! \brief Built-in "sysinfo": chip identity, heap, uptime and last reset reason. */
static sas_status_t cmd_sysinfo(int argc, char **argv, sas_ws_client_t *client)
{
    (void)argc;
    (void)argv;

    sas_chip_info_t chip;
    sas_runtime_info_t runtime;
    esp_reset_reason_t reset_reason = ESP_RST_UNKNOWN;
    (void)sas_sysinfo_get_chip_info(&chip);
    (void)sas_sysinfo_get_runtime_info(&runtime);
    (void)sas_sysinfo_get_reset_reason(&reset_reason);

    (void)sas_terminal_printf(client, "chip:      %s rev %u, %u MB flash\n", chip.chip_model,
                              (unsigned int)chip.chip_revision,
                              (unsigned int)(chip.flash_size_bytes / (1024U * 1024U)));
    (void)sas_terminal_printf(client, "heap:      %u bytes free (minimum %u)\n",
                              (unsigned int)runtime.free_heap_bytes,
                              (unsigned int)runtime.min_free_heap_bytes);
    (void)sas_terminal_printf(client, "uptime:    %u s\n", (unsigned int)runtime.uptime_seconds);
    (void)sas_terminal_printf(client, "reset:     reason %d\n", (int)reset_reason);
    return SAS_STATUS_OK;
}

/*! \brief Built-in "tasks": FreeRTOS task snapshot. */
static sas_status_t cmd_tasks(int argc, char **argv, sas_ws_client_t *client)
{
    (void)argc;
    (void)argv;

    /* static: too large for the HTTP Server task's stack; commands run one at a time on it. */
    static sas_task_snapshot_t tasks[SAS_SYSINFO_MAX_TASKS];
    uint8_t count = 0U;
    sas_status_t status = sas_sysinfo_get_task_snapshot(tasks, SAS_SYSINFO_MAX_TASKS, &count);
    if (status != SAS_STATUS_OK) {
        return status;
    }

    (void)sas_terminal_printf(client, "%-16s state  stack-free\n", "task");
    for (uint8_t i = 0U; i < count; i++) {
        (void)sas_terminal_printf(client, "%-16s %5u  %10u\n", tasks[i].task_name,
                                  (unsigned int)tasks[i].state,
                                  (unsigned int)tasks[i].stack_high_water_mark);
    }
    return SAS_STATUS_OK;
}

/*! \brief Built-in "boot": per-module boot status report. */
static sas_status_t cmd_boot(int argc, char **argv, sas_ws_client_t *client)
{
    (void)argc;
    (void)argv;

    sas_boot_record_t records[SAS_SYSINFO_MAX_BOOT_RECORDS];
    uint8_t count = 0U;
    sas_status_t status = sas_sysinfo_get_boot_status(records, SAS_SYSINFO_MAX_BOOT_RECORDS, &count);
    if (status != SAS_STATUS_OK) {
        return status;
    }

    for (uint8_t i = 0U; i < count; i++) {
        const char *text = "ok";
        if (records[i].status == SAS_BOOT_STATUS_FAILED) {
            text = "FAILED";
        } else if (records[i].status == SAS_BOOT_STATUS_SKIPPED_DEPENDENCY) {
            text = "skipped (dependency)";
        } else {
            /* ok */
        }
        (void)sas_terminal_printf(client, "%-14s %s\n", records[i].module_name, text);
    }
    return SAS_STATUS_OK;
}

/*! \brief Built-in "wifi": connection state, SSID and RSSI. */
static sas_status_t cmd_wifi(int argc, char **argv, sas_ws_client_t *client)
{
    (void)argc;
    (void)argv;

    static const char *const state_names[] = {"idle", "connecting", "connected", "reconnecting",
                                              "ap_fallback"};
    sas_wifi_status_t wifi;
    sas_status_t status = sas_wifi_get_status(&wifi);
    if (status != SAS_STATUS_OK) {
        return status;
    }

    const char *state = ((size_t)wifi.state < (sizeof(state_names) / sizeof(state_names[0])))
                            ? state_names[wifi.state]
                            : "unknown";
    (void)sas_terminal_printf(client, "state: %s\nssid:  %s\nrssi:  %d dBm\n", state,
                              (wifi.ssid[0] != '\0') ? wifi.ssid : "(none)", (int)wifi.rssi);
    (void)sas_terminal_printf(client, "ip:    %s\nname:  %s.local\n",
                              (wifi.ip[0] != '\0') ? wifi.ip : "(none)", wifi.hostname);
    return SAS_STATUS_OK;
}

/*! \brief Built-in "reboot": restart the device shortly after replying. */
static sas_status_t cmd_reboot(int argc, char **argv, sas_ws_client_t *client)
{
    (void)argc;
    (void)argv;

    static esp_timer_handle_t timer = NULL;
    if (timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = reboot_timer_callback,
            .name = "sas_term_reboot",
        };
        if (esp_timer_create(&timer_args, &timer) != ESP_OK) {
            timer = NULL;
            return SAS_ERR_NO_MEM;
        }
    }
    if (esp_timer_start_once(timer, SAS_TERM_REBOOT_DELAY_US) != ESP_OK) {
        return SAS_ERR_INVALID_STATE; /* already counting down */
    }

    sas_log_warn("reboot requested from the terminal");
    (void)sas_terminal_printf(client, "rebooting...\n");
    return SAS_STATUS_OK;
}

/*! \brief One-shot timer armed by cmd_reboot(). */
static void reboot_timer_callback(void *arg)
{
    (void)arg;
    esp_restart();
}

/*! \brief Get client's session slot, reset for a new command. A client without one takes a free
 * slot or, failing that, the least recently used. */
static size_t acquire_session(const sas_ws_client_t *client)
{
    size_t index = find_session(client);
    if (index >= SAS_WS_MAX_CLIENTS) {
        index = 0U;
        for (size_t i = 0U; i < SAS_WS_MAX_CLIENTS; i++) {
            if (!s_session_state[i].in_use) {
                index = i;
                break;
            }
            if (s_session_state[i].last_used < s_session_state[index].last_used) {
                index = i;
            }
        }
    }

    s_sessions[index].client = *client;
    s_sessions[index].output_len = 0U;
    s_session_state[index].in_use = true;
    s_session_state[index].executing = false;
    s_session_state[index].truncated = false;
    s_session_state[index].flushed_len = 0U;
    s_session_state[index].last_used = ++s_use_counter;
    return index;
}

/*! \brief Index of client's session slot, or SAS_WS_MAX_CLIENTS if it has none. */
static size_t find_session(const sas_ws_client_t *client)
{
    for (size_t i = 0U; i < SAS_WS_MAX_CLIENTS; i++) {
        if (s_session_state[i].in_use && (s_sessions[i].client.fd == client->fd)) {
            return i;
        }
    }
    return SAS_WS_MAX_CLIENTS;
}

/*! \brief Append text to the session's output_buf, bounded at SAS_TERM_MAX_OUTPUT_LEN; anything
 * beyond is discarded and the session marked truncated. Bytes other than printable ASCII, '\n'
 * and '\t' become '?' (keeps the JSON-escaped size predictable, see SAS_TERM_CHUNK_LEN). */
static void append_output(size_t index, const char *text, size_t len)
{
    sas_terminal_session_t *session = &s_sessions[index];

    for (size_t i = 0U; i < len; i++) {
        if (session->output_len >= SAS_TERM_MAX_OUTPUT_LEN) {
            s_session_state[index].truncated = true;
            return;
        }
        char c = text[i];
        bool printable = ((c >= ' ') && (c <= '~')) || (c == '\n') || (c == '\t');
        session->output_buf[session->output_len] = printable ? c : '?';
        session->output_len++;
    }
}

/*! \brief Longest prefix of text (at most len bytes) whose JSON-escaped size fits one message's
 * SAS_TERM_CHUNK_BUDGET. */
static size_t chunk_fit(const char *text, size_t len)
{
    size_t cost = 0U;
    size_t count = 0U;
    while (count < len) {
        char c = text[count];
        size_t char_cost = ((c == '"') || (c == '\\') || (c == '\n') || (c == '\t')) ? 2U : 1U;
        if ((cost + char_cost) > SAS_TERM_CHUNK_BUDGET) {
            break;
        }
        cost += char_cost;
        count++;
    }
    return count;
}

/*! \brief Stream buffered output to the client. A non-final flush sends only messages it can fill
 * (SAS-TERM-FR-002: output goes out as it is produced, not once the command ends); the final one
 * sends whatever is left and always ends the command with one last message, which carries the
 * truncation flag. */
static void flush_output(size_t index, bool final)
{
    sas_terminal_session_t *session = &s_sessions[index];
    sas_terminal_session_state_t *state = &s_session_state[index];

    for (;;) {
        size_t pending = session->output_len - state->flushed_len;
        size_t fit = chunk_fit(&session->output_buf[state->flushed_len], pending);
        if (fit == pending) {
            break; /* what is left does not fill a message yet */
        }
        send_chunk(index, &session->output_buf[state->flushed_len], fit, false);
        state->flushed_len += fit;
    }

    if (final) {
        size_t remaining = session->output_len - state->flushed_len;
        send_chunk(index, &session->output_buf[state->flushed_len], remaining, state->truncated);
        state->flushed_len = session->output_len;
    }
}

/*! \brief Send one "terminal_output" message: { "session", "text", "truncated" }. */
static void send_chunk(size_t index, const char *text, size_t len, bool truncated)
{
    char chunk[SAS_TERM_CHUNK_BUDGET + 1U];
    (void)memcpy(chunk, text, len);
    chunk[len] = '\0';

    char session_id[12];
    (void)snprintf(session_id, sizeof(session_id), "%d", s_sessions[index].client.fd);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return;
    }
    cJSON_AddStringToObject(data, "session", session_id);
    cJSON_AddStringToObject(data, "text", chunk);
    cJSON_AddBoolToObject(data, "truncated", truncated);
    (void)sas_ws_send_to_client(&s_sessions[index].client, SAS_WS_MSG_TERMINAL_OUTPUT, data);
    cJSON_Delete(data);
}

/*! \brief Look up name in the static command table. */
static const sas_terminal_command_t *find_command(const char *name)
{
    for (size_t i = 0U; i < s_command_count; i++) {
        if (strcmp(s_commands[i].name, name) == 0) {
            return &s_commands[i];
        }
    }
    return NULL;
}

/*! \brief Split line in place on spaces/tabs into at most max_args tokens; returns argc. */
static int tokenize(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *cursor = line;

    while (argc < max_args) {
        while ((*cursor == ' ') || (*cursor == '\t')) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        argv[argc] = cursor;
        argc++;
        while ((*cursor != '\0') && (*cursor != ' ') && (*cursor != '\t')) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        *cursor = '\0';
        cursor++;
    }
    return argc;
}
