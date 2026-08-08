/*
 * SAS Dashboard client-side logic skeleton.
 *
 * Covers the shell, Home, Wi-Fi, Files, OTA, BLE, and Login views against
 * the REST endpoints and WebSocket event schema served by the device. No
 * device-side .c/.h module backs this file.
 *
 * TODO:
 * 1. Open the wss:// connection and reflect connected/reconnecting state
 *    in the header indicator.
 * 2. Fetch each view's one-shot REST data on view entry, and patch the
 *    DOM in place on the matching WebSocket event for live fields - do
 *    not poll or reload the page.
 * 3. Implement the Wi-Fi scan/connect flow, Files list/upload/rename/
 *    delete flow, OTA upload/URL/progress flow, BLE scan/device-table
 *    flow, and Login flow, each against the device's REST/WebSocket
 *    contract.
 * 4. Hold the auth token in memory only (a JS variable), never in
 *    localStorage.
 */
