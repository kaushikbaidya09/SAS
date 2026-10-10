/*
 * SAS Dashboard client-side logic.
 *
 * Covers the shell, Home, Wi-Fi, BLE, and Logs views against the REST
 * endpoints and WebSocket event schema served by the device (see
 * main/src/web/sas_rest_api.c and main/src/web/sas_websocket.c). Files,
 * OTA, and Login views are not built here - their device-side modules
 * are out of scope for this change and remain stubs.
 *
 * The auth token variable below is kept for parity with the WebSocket/
 * REST contract's requires_auth flag, but every route this build
 * registers has requires_auth = false, so it is never populated; per the
 * original design note, it would be held in memory only, never
 * localStorage, if Authentication is wired in later.
 */
(function () {
  "use strict";

  /* WebSocket envelope "type" values; must match sas_ws_msg_type_t
   * (main/src/web/sas_websocket.h). */
  var WS_MSG_WIFI_STATE = 1;
  var WS_MSG_BLE_DEVICE = 3;
  var WS_MSG_LOG_ENTRY = 5;

  var MAX_LOG_LINES = 500;

  var authToken = null; // eslint-disable-line no-unused-vars

  var state = {
    connState: "connecting", // "connecting" | "connected" | "disconnected"
    wifiResults: [],
    wifiScanInFlight: false,
    bleDevices: {}, // mac -> device
    logLines: [], // {timestamp_us, level, tag, message}
  };

  var els = {};
  var ws = null;
  var wsReconnectTimer = null;

  /* ---- view registry ---- */
  var VIEWS = [
    { id: "home", label: "Home", render: renderHome },
    { id: "wifi", label: "Wi-Fi", render: renderWifi },
    { id: "ble", label: "BLE", render: renderBle },
    { id: "logs", label: "Logs", render: renderLogs },
  ];

  document.addEventListener("DOMContentLoaded", function () {
    buildShell();
    connectWebSocket();
    window.addEventListener("hashchange", routeToCurrentHash);
    routeToCurrentHash();
  });

  /* ---- shell ---- */

  function buildShell() {
    var app = document.getElementById("app");
    app.innerHTML = "";

    var shell = document.createElement("div");
    shell.className = "sas-shell";

    var header = document.createElement("header");
    header.className = "sas-header";

    var title = document.createElement("div");
    title.className = "sas-title";
    title.textContent = "SAS Dashboard";
    header.appendChild(title);

    var conn = document.createElement("div");
    conn.className = "sas-conn";
    conn.setAttribute("data-state", "connecting");
    var dot = document.createElement("span");
    dot.className = "sas-conn-dot";
    var connText = document.createElement("span");
    connText.textContent = "Connecting...";
    conn.appendChild(dot);
    conn.appendChild(connText);
    header.appendChild(conn);
    els.conn = conn;
    els.connText = connText;

    var nav = document.createElement("nav");
    nav.className = "sas-nav";
    nav.setAttribute("aria-label", "Dashboard sections");
    var ul = document.createElement("ul");
    VIEWS.forEach(function (view) {
      var li = document.createElement("li");
      var a = document.createElement("a");
      a.href = "#" + view.id;
      a.textContent = view.label;
      a.dataset.viewId = view.id;
      li.appendChild(a);
      ul.appendChild(li);
    });
    nav.appendChild(ul);
    els.nav = nav;

    var main = document.createElement("main");
    main.className = "sas-main";
    var viewContainer = document.createElement("div");
    viewContainer.className = "sas-view";
    main.appendChild(viewContainer);
    els.viewContainer = viewContainer;

    shell.appendChild(header);
    shell.appendChild(nav);
    shell.appendChild(main);
    app.appendChild(shell);
  }

  function setConnState(newState, label) {
    state.connState = newState;
    els.conn.setAttribute("data-state", newState);
    els.connText.textContent = label;
  }

  /* ---- routing ---- */

  function routeToCurrentHash() {
    var id = (window.location.hash || "#home").replace("#", "");
    var view = VIEWS.filter(function (v) {
      return v.id === id;
    })[0];
    if (!view) {
      view = VIEWS[0];
    }

    var links = els.nav.querySelectorAll("a");
    for (var i = 0; i < links.length; i++) {
      if (links[i].dataset.viewId === view.id) {
        links[i].setAttribute("aria-current", "page");
      } else {
        links[i].removeAttribute("aria-current");
      }
    }

    els.viewContainer.innerHTML = "";
    els.viewContainer.dataset.viewId = view.id;
    view.render(els.viewContainer);
  }

  function currentViewId() {
    return els.viewContainer.dataset.viewId;
  }

  /* ---- WebSocket ---- */

  function connectWebSocket() {
    var url = "wss://" + window.location.host + "/ws";
    setConnState("connecting", "Connecting...");

    try {
      ws = new WebSocket(url);
    } catch (e) {
      scheduleReconnect();
      return;
    }

    ws.onopen = function () {
      setConnState("connected", "Connected");
    };
    ws.onclose = function () {
      setConnState("disconnected", "Disconnected");
      scheduleReconnect();
    };
    ws.onerror = function () {
      setConnState("disconnected", "Disconnected");
    };
    ws.onmessage = function (evt) {
      handleWsMessage(evt.data);
    };
  }

  function scheduleReconnect() {
    if (wsReconnectTimer !== null) {
      return;
    }
    wsReconnectTimer = window.setTimeout(function () {
      wsReconnectTimer = null;
      connectWebSocket();
    }, 3000);
  }

  function handleWsMessage(raw) {
    var envelope;
    try {
      envelope = JSON.parse(raw);
    } catch (e) {
      return;
    }
    if (!envelope || typeof envelope.type !== "number") {
      return;
    }

    switch (envelope.type) {
    case WS_MSG_WIFI_STATE:
      if (currentViewId() === "wifi") {
        setWifiStateText(envelope.data && envelope.data.state);
      }
      break;
    case WS_MSG_BLE_DEVICE:
      upsertBleDevice(envelope.data);
      if (currentViewId() === "ble") {
        renderBleTable();
      }
      break;
    case WS_MSG_LOG_ENTRY:
      appendLogLine(envelope.data);
      if (currentViewId() === "logs") {
        appendLogLineToDom(envelope.data);
      }
      break;
    default:
      break;
    }
  }

  /* ---- REST helper ---- */

  function fetchJSON(path) {
    return fetch(path, { headers: { Accept: "application/json" } }).then(function (resp) {
      if (!resp.ok) {
        throw new Error("HTTP " + resp.status);
      }
      return resp.json();
    });
  }

  /* ---- Home view ---- */

  function renderHome(container) {
    var h1 = document.createElement("h1");
    h1.textContent = "Home";
    container.appendChild(h1);

    var p = document.createElement("p");
    p.className = "sas-status-text";
    p.textContent =
      "Live Wi-Fi scan results, BLE scan results, and device logs are available from the " +
      "nav on the left.";
    container.appendChild(p);
  }

  /* ---- Wi-Fi view ---- */

  var WIFI_AUTH_MODE_NAMES = {
    0: "Open",
    1: "WEP",
    2: "WPA-PSK",
    3: "WPA2-PSK",
    4: "WPA/WPA2-PSK",
    5: "WPA2-Enterprise",
    6: "WPA3-PSK",
    7: "WPA2/WPA3-PSK",
    8: "WAPI-PSK",
    9: "OWE",
  };

  function wifiAuthModeName(mode) {
    return WIFI_AUTH_MODE_NAMES[mode] || "Auth mode " + mode;
  }

  function renderWifi(container) {
    var h1 = document.createElement("h1");
    h1.textContent = "Wi-Fi";
    container.appendChild(h1);

    var actions = document.createElement("div");
    actions.className = "sas-view-actions";
    var scanBtn = document.createElement("button");
    scanBtn.className = "sas-btn";
    scanBtn.textContent = "Scan";
    scanBtn.addEventListener("click", runWifiScan);
    var statusText = document.createElement("span");
    statusText.className = "sas-status-text";
    actions.appendChild(scanBtn);
    actions.appendChild(statusText);
    container.appendChild(actions);
    els.wifiScanBtn = scanBtn;
    els.wifiStatusText = statusText;

    var tableWrap = document.createElement("div");
    tableWrap.className = "sas-table-wrap";
    container.appendChild(tableWrap);
    els.wifiTableWrap = tableWrap;

    renderWifiTable();
    if (state.wifiResults.length === 0) {
      runWifiScan();
    }
  }

  function setWifiStateText(name) {
    if (els.wifiStatusText && name) {
      els.wifiStatusText.textContent = "Connection state: " + name;
    }
  }

  function runWifiScan() {
    if (state.wifiScanInFlight) {
      return;
    }
    state.wifiScanInFlight = true;
    if (els.wifiScanBtn) {
      els.wifiScanBtn.disabled = true;
    }
    if (els.wifiStatusText) {
      els.wifiStatusText.textContent = "Scanning...";
    }

    fetchJSON("/api/v1/wifi/scan")
      .then(function (results) {
        state.wifiResults = results || [];
        if (els.wifiStatusText) {
          els.wifiStatusText.textContent = state.wifiResults.length + " access point(s) found";
        }
        renderWifiTable();
      })
      .catch(function () {
        if (els.wifiStatusText) {
          els.wifiStatusText.textContent = "Scan failed";
        }
      })
      .then(function () {
        state.wifiScanInFlight = false;
        if (els.wifiScanBtn) {
          els.wifiScanBtn.disabled = false;
        }
      });
  }

  function renderWifiTable() {
    if (!els.wifiTableWrap) {
      return;
    }
    els.wifiTableWrap.innerHTML = "";

    if (state.wifiResults.length === 0) {
      var empty = document.createElement("div");
      empty.className = "sas-empty";
      empty.textContent = "No access points found yet.";
      els.wifiTableWrap.appendChild(empty);
      return;
    }

    var table = document.createElement("table");
    table.className = "sas-table";
    table.innerHTML =
      "<thead><tr><th>SSID</th><th>Signal (dBm)</th><th>Security</th></tr></thead>";
    var tbody = document.createElement("tbody");
    state.wifiResults
      .slice()
      .sort(function (a, b) {
        return b.rssi - a.rssi;
      })
      .forEach(function (ap) {
        var tr = document.createElement("tr");
        appendCell(tr, ap.ssid || "(hidden)");
        appendCell(tr, String(ap.rssi));
        appendCell(tr, wifiAuthModeName(ap.auth_mode));
        tbody.appendChild(tr);
      });
    table.appendChild(tbody);
    els.wifiTableWrap.appendChild(table);
  }

  /* ---- BLE view ---- */

  function upsertBleDevice(device) {
    if (!device || !device.mac) {
      return;
    }
    state.bleDevices[device.mac] = device;
  }

  function renderBle(container) {
    var h1 = document.createElement("h1");
    h1.textContent = "BLE";
    container.appendChild(h1);

    var p = document.createElement("p");
    p.className = "sas-status-text";
    p.textContent = "Passively observed BLE advertisers, updated live as new devices are seen.";
    container.appendChild(p);

    var tableWrap = document.createElement("div");
    tableWrap.className = "sas-table-wrap";
    container.appendChild(tableWrap);
    els.bleTableWrap = tableWrap;

    fetchJSON("/api/v1/ble/devices")
      .then(function (devices) {
        (devices || []).forEach(upsertBleDevice);
        renderBleTable();
      })
      .catch(function () {
        renderBleTable();
      });
    renderBleTable();
  }

  function renderBleTable() {
    if (!els.bleTableWrap) {
      return;
    }
    var devices = Object.keys(state.bleDevices).map(function (mac) {
      return state.bleDevices[mac];
    });

    els.bleTableWrap.innerHTML = "";
    if (devices.length === 0) {
      var empty = document.createElement("div");
      empty.className = "sas-empty";
      empty.textContent = "No BLE devices observed yet.";
      els.bleTableWrap.appendChild(empty);
      return;
    }

    var table = document.createElement("table");
    table.className = "sas-table";
    table.innerHTML = "<thead><tr><th>MAC</th><th>Name</th><th>RSSI</th><th>Last seen</th></tr></thead>";
    var tbody = document.createElement("tbody");
    devices
      .sort(function (a, b) {
        return b.last_seen_us - a.last_seen_us;
      })
      .forEach(function (dev) {
        var tr = document.createElement("tr");
        appendCell(tr, dev.mac);
        appendCell(tr, dev.name || "(unnamed)");
        appendCell(tr, String(dev.rssi));
        appendCell(tr, formatDeviceUptime(dev.last_seen_us));
        tbody.appendChild(tr);
      });
    table.appendChild(tbody);
    els.bleTableWrap.appendChild(table);
  }

  function formatDeviceUptime(lastSeenUs) {
    if (typeof lastSeenUs !== "number") {
      return "";
    }
    return (lastSeenUs / 1e6).toFixed(1) + "s since boot";
  }

  /* ---- Logs view ---- */

  function appendLogLine(entry) {
    if (!entry) {
      return;
    }
    state.logLines.push(entry);
    if (state.logLines.length > MAX_LOG_LINES) {
      state.logLines.shift();
    }
  }

  function renderLogs(container) {
    var h1 = document.createElement("h1");
    h1.textContent = "Logs";
    container.appendChild(h1);

    var pane = document.createElement("div");
    pane.className = "sas-log-pane";
    pane.setAttribute("role", "log");
    pane.setAttribute("aria-live", "polite");
    container.appendChild(pane);
    els.logPane = pane;

    fetchJSON("/api/v1/logs")
      .then(function (entries) {
        (entries || []).forEach(function (entry) {
          appendLogLine(entry);
        });
        renderLogPane();
      })
      .catch(function () {
        renderLogPane();
      });
    renderLogPane();
  }

  function renderLogPane() {
    if (!els.logPane) {
      return;
    }
    els.logPane.innerHTML = "";
    if (state.logLines.length === 0) {
      var empty = document.createElement("div");
      empty.className = "sas-empty";
      empty.textContent = "No log entries yet.";
      els.logPane.appendChild(empty);
      return;
    }
    state.logLines.forEach(function (entry) {
      els.logPane.appendChild(buildLogLineEl(entry));
    });
    els.logPane.scrollTop = els.logPane.scrollHeight;
  }

  function appendLogLineToDom(entry) {
    if (!els.logPane || !entry) {
      return;
    }
    if (els.logPane.querySelector(".sas-empty")) {
      els.logPane.innerHTML = "";
    }
    els.logPane.appendChild(buildLogLineEl(entry));
    while (els.logPane.childElementCount > MAX_LOG_LINES) {
      els.logPane.removeChild(els.logPane.firstChild);
    }
    els.logPane.scrollTop = els.logPane.scrollHeight;
  }

  function buildLogLineEl(entry) {
    var line = document.createElement("div");
    line.className = "sas-log-line";

    var ts = document.createElement("span");
    ts.className = "sas-log-ts";
    ts.textContent = formatLogTimestamp(entry.timestamp_us);
    line.appendChild(ts);

    var pill = document.createElement("span");
    pill.className = "sas-pill";
    pill.setAttribute("data-level", entry.level || "INFO");
    pill.textContent = entry.level || "INFO";
    line.appendChild(pill);

    var tag = document.createElement("span");
    tag.className = "sas-log-tag";
    tag.textContent = entry.tag || "";
    line.appendChild(tag);

    var msg = document.createElement("span");
    msg.textContent = entry.message || "";
    line.appendChild(msg);

    return line;
  }

  function formatLogTimestamp(timestampUs) {
    if (typeof timestampUs !== "number") {
      return "";
    }
    return (timestampUs / 1e6).toFixed(3) + "s";
  }

  /* ---- shared DOM helper ---- */

  function appendCell(tr, text) {
    var td = document.createElement("td");
    td.textContent = text;
    tr.appendChild(td);
  }
})();
