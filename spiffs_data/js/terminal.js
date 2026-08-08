/*
 * SAS Terminal view client-side logic skeleton.
 *
 * Talks to the device over the "terminal_cmd" / "terminal_output"
 * WebSocket messages. Device-side counterpart: main/src/terminal/sas_terminal.{c,h}.
 *
 * TODO:
 * 1. Render "terminal_output" events into the scrollback pane in a
 *    monospace font, appending a visible "[output truncated]" marker
 *    when the server reports SAS_TERM_ERR_OUTPUT_TRUNCATED.
 * 2. Maintain a client-side-only command history navigable with
 *    Up/Down arrow keys (not persisted server-side).
 * 3. On submit, send a "terminal_cmd" WebSocket message and echo the
 *    submitted command line into the scrollback immediately, before the
 *    server response arrives.
 * 4. When Authentication is enabled and the session token is invalid or
 *    absent, disable the input and direct the user to the Login view.
 */
