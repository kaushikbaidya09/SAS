/*
 * SAS Dashboard client-side logic.
 *
 * Covers the shell and the Home, Wi-Fi, BLE, Files, OTA, Logs, Account and
 * Login views against the REST endpoints and WebSocket events served by
 * the device (docs/REST_API_Specification.md; device side:
 * main/src/web/sas_rest_api.c and main/src/web/sas_websocket.c). The
 * Terminal view lives in js/terminal.js and plugs in through
 * window.SASTerminal.
 *
 * The session token is held in memory only (never localStorage), so it is
 * gone on reload and a shared browser does not keep it.
 */
(function () {
  "use strict";

  var API = "/api/v1";
  var MAX_LOG_LINES = 500;

  var state = {
    connState: "connecting", // "connecting" | "connected" | "disconnected"
    authEnabled: true, // until GET /auth/status says otherwise
    authToken: null,
    pendingViewId: null, // view to return to after a successful login
    wifi: null, // {state, ssid, rssi}
    wifiResults: [],
    wifiScanInFlight: false,
    bleDevices: {}, // mac -> device
    bleSort: { key: "rssi", descending: true },
    logLines: [], // {timestamp_us, level, tag, message}
    filesPath: "/",
    ota: { state: "idle", percent_complete: 0 },
  };

  var els = {};
  var ws = null;
  var wsReconnectTimer = null;

  /* ---- view registry; "open" views render without a login ---- */
  var VIEWS = [
    { id: "home", label: "Home", render: renderHome, open: true },
    { id: "wifi", label: "Wi-Fi", render: renderWifi },
    { id: "ble", label: "BLE", render: renderBle },
    { id: "files", label: "Files", render: renderFiles },
    { id: "ota", label: "OTA", render: renderOta },
    { id: "terminal", label: "Terminal", render: renderTerminal },
    { id: "logs", label: "Logs", render: renderLogs },
    { id: "account", label: "Account", render: renderAccount },
  ];

  document.addEventListener("DOMContentLoaded", function () {
    buildShell();
    window.addEventListener("hashchange", routeToCurrentHash);

    request("GET", "/auth/status")
      .then(function (status) {
        state.authEnabled = !!status.enabled;
      })
      .catch(function () {
        /* keep the safe default: assume a login is required */
      })
      .then(function () {
        updateAuthControl();
        connectWebSocket();
        routeToCurrentHash();
      });
  });

  /* ---- shell ---- */

  function buildShell() {
    var app = document.getElementById("app");
    app.innerHTML = "";

    var shell = el("div", "sas-shell");
    var header = el("header", "sas-header");
    header.appendChild(el("div", "sas-title", "SAS Dashboard"));

    var conn = el("div", "sas-conn");
    conn.setAttribute("data-state", "connecting");
    conn.appendChild(el("span", "sas-conn-dot"));
    var connText = el("span", null, "Connecting...");
    conn.appendChild(connText);
    header.appendChild(conn);
    els.conn = conn;
    els.connText = connText;

    var authBtn = el("button", "sas-btn sas-btn-quiet sas-auth-btn", "Log in");
    authBtn.addEventListener("click", onAuthControlClick);
    header.appendChild(authBtn);
    els.authBtn = authBtn;

    var nav = el("nav", "sas-nav");
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

    var main = el("main", "sas-main");
    var viewContainer = el("div", "sas-view");
    main.appendChild(viewContainer);
    els.viewContainer = viewContainer;

    var toast = el("div", "sas-toast");
    toast.setAttribute("role", "status");
    toast.hidden = true;
    els.toast = toast;

    shell.appendChild(header);
    shell.appendChild(nav);
    shell.appendChild(main);
    app.appendChild(shell);
    app.appendChild(toast);
  }

  function setConnState(newState, label) {
    state.connState = newState;
    els.conn.setAttribute("data-state", newState);
    els.connText.textContent = label;
  }

  function updateAuthControl() {
    els.authBtn.hidden = !state.authEnabled;
    els.authBtn.textContent = state.authToken ? "Log out" : "Log in";
  }

  function onAuthControlClick() {
    if (!state.authToken) {
      showLogin(currentViewId());
      return;
    }
    request("POST", "/auth/logout")
      .catch(function () {
        /* the token is discarded locally either way */
      })
      .then(function () {
        setToken(null);
        showLogin("home");
      });
  }

  var toastTimer = null;
  function toast(message) {
    els.toast.textContent = message;
    els.toast.hidden = false;
    window.clearTimeout(toastTimer);
    toastTimer = window.setTimeout(function () {
      els.toast.hidden = true;
    }, 4000);
  }

  /* ---- routing ---- */

  function isLoggedIn() {
    return !state.authEnabled || !!state.authToken;
  }

  function routeToCurrentHash() {
    var id = (window.location.hash || "#home").replace("#", "");
    var view = VIEWS.filter(function (v) {
      return v.id === id;
    })[0];
    if (!view) {
      view = VIEWS[0];
    }

    /* Login gate: every view except Home needs a session where Authentication is enabled. */
    if (!view.open && !isLoggedIn()) {
      showLogin(view.id);
      return;
    }
    showView(view);
  }

  function showView(view) {
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

  function showLogin(returnToViewId) {
    state.pendingViewId = returnToViewId || "home";
    showView({ id: "login", render: renderLogin });
  }

  function currentViewId() {
    return els.viewContainer.dataset.viewId;
  }

  /* ---- WebSocket ---- */

  function connectWebSocket() {
    var scheme = window.location.protocol === "https:" ? "wss://" : "ws://";
    var url = scheme + window.location.host + "/ws";
    if (state.authToken) {
      url += "?token=" + encodeURIComponent(state.authToken);
    }
    setConnState("connecting", "Connecting...");

    var socket;
    try {
      socket = new WebSocket(url);
    } catch (e) {
      scheduleReconnect();
      return;
    }
    ws = socket;

    socket.onopen = function () {
      if (ws === socket) {
        setConnState("connected", "Connected");
      }
    };
    socket.onclose = function () {
      if (ws === socket) {
        setConnState("disconnected", "Disconnected");
        scheduleReconnect();
      }
    };
    socket.onmessage = function (evt) {
      handleWsMessage(evt.data);
    };
  }

  /* The device ties a socket to the token it was opened with, so a login or logout reopens it. */
  function reconnectWebSocket() {
    var old = ws;
    ws = null;
    if (old) {
      old.close();
    }
    window.clearTimeout(wsReconnectTimer);
    wsReconnectTimer = null;
    connectWebSocket();
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

  function wsSend(type, data) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return false;
    }
    ws.send(JSON.stringify({ type: type, data: data }));
    return true;
  }

  function handleWsMessage(raw) {
    var envelope;
    try {
      envelope = JSON.parse(raw);
    } catch (e) {
      return;
    }
    if (!envelope || typeof envelope.type !== "string") {
      return;
    }
    var data = envelope.data || {};

    switch (envelope.type) {
    case "sysinfo":
      updateRuntimeCard(data);
      break;
    case "wifi_state":
      state.wifi = data;
      updateWifiStatus();
      break;
    case "ota_progress":
      state.ota = data;
      updateOtaStatus();
      break;
    case "ble_device":
      upsertBleDevice(data);
      if (currentViewId() === "ble") {
        renderBleTable();
      }
      break;
    case "log_entry":
      appendLogLine(data);
      if (currentViewId() === "logs") {
        appendLogLineToDom(data);
      }
      break;
    case "terminal_output":
      if (window.SASTerminal) {
        window.SASTerminal.onOutput(data);
      }
      break;
    default:
      break;
    }
  }

  /* ---- REST helper ---- */

  function authHeaders(headers) {
    headers = headers || {};
    if (state.authToken) {
      headers.Authorization = "Bearer " + state.authToken;
    }
    return headers;
  }

  /*
   * Resolves with the parsed JSON body. Rejects with an Error carrying
   * .status, .code (the envelope's error_code) and .retryAfter (seconds,
   * from Retry-After). A 401 on anything but the login call means the
   * session is gone: the token is dropped and the Login view shown.
   */
  function request(method, path, body) {
    var options = { method: method, headers: authHeaders({ Accept: "application/json" }) };
    if (body !== undefined) {
      options.headers["Content-Type"] = "application/json";
      options.body = JSON.stringify(body);
    }

    return fetch(API + path, options).then(function (resp) {
      return resp
        .json()
        .catch(function () {
          return {};
        })
        .then(function (json) {
          if (resp.ok) {
            return json;
          }
          var err = new Error(json.message || "HTTP " + resp.status);
          err.status = resp.status;
          err.code = json.error_code || "";
          err.retryAfter = parseInt(resp.headers.get("Retry-After"), 10) || 0;
          if (resp.status === 401 && path !== "/auth/login") {
            handleSessionLost();
          }
          throw err;
        });
    });
  }

  function handleSessionLost() {
    if (!state.authEnabled || currentViewId() === "login") {
      return;
    }
    var returnTo = currentViewId();
    setToken(null);
    if (returnTo !== "home") {
      showLogin(returnTo);
    }
  }

  function setToken(token) {
    var changed = state.authToken !== token;
    state.authToken = token;
    updateAuthControl();
    if (changed) {
      reconnectWebSocket();
    }
  }

  /* Raw-body upload with a browser-side progress callback (fetch has no upload progress). */
  function uploadWithProgress(path, file, onProgress) {
    return new Promise(function (resolve, reject) {
      var xhr = new XMLHttpRequest();
      xhr.open("POST", API + path);
      xhr.setRequestHeader("Content-Type", "application/octet-stream");
      if (state.authToken) {
        xhr.setRequestHeader("Authorization", "Bearer " + state.authToken);
      }
      xhr.upload.onprogress = function (evt) {
        if (evt.lengthComputable) {
          onProgress(Math.round((evt.loaded * 100) / evt.total));
        }
      };
      xhr.onload = function () {
        var json = {};
        try {
          json = JSON.parse(xhr.responseText);
        } catch (e) {
          /* non-JSON body: fall through with {} */
        }
        if (xhr.status >= 200 && xhr.status < 300) {
          resolve(json);
        } else {
          var err = new Error(json.message || "HTTP " + xhr.status);
          err.status = xhr.status;
          err.code = json.error_code || "";
          reject(err);
        }
      };
      xhr.onerror = function () {
        reject(new Error("Upload failed: connection lost"));
      };
      xhr.send(file);
    });
  }

  /* ---- Login view ---- */

  function renderLogin(container) {
    container.appendChild(el("h1", null, "Log in"));

    var form = el("form", "sas-form");
    var user = inputField(form, "Username", "text", "username");
    var pass = inputField(form, "Password", "password", "current-password");
    var submit = el("button", "sas-btn", "Log in");
    submit.type = "submit";
    form.appendChild(submit);
    var error = el("p", "sas-error");
    error.setAttribute("role", "alert");
    form.appendChild(error);
    container.appendChild(form);
    user.focus();

    form.addEventListener("submit", function (evt) {
      evt.preventDefault();
      error.textContent = "";
      submit.disabled = true;

      request("POST", "/auth/login", { username: user.value, password: pass.value })
        .then(function (session) {
          pass.value = "";
          setToken(session.token);
          var target = state.pendingViewId || "home";
          state.pendingViewId = null;
          if (window.location.hash === "#" + target) {
            routeToCurrentHash();
          } else {
            window.location.hash = "#" + target;
          }
        })
        .catch(function (err) {
          pass.value = ""; /* the username is deliberately left in place */
          if (err.code === "rate_limited") {
            error.textContent = "Too many attempts, try again shortly";
            window.setTimeout(function () {
              submit.disabled = false;
            }, Math.max(err.retryAfter, 1) * 1000);
            return;
          }
          error.textContent =
            err.code === "unauthorized" ? "Wrong username or password" : err.message;
          submit.disabled = false;
        });
    });
  }

  /* ---- Home view ---- */

  function renderHome(container) {
    var actions = el("div", "sas-view-actions");
    actions.appendChild(el("h1", null, "Home"));
    var refresh = el("button", "sas-btn sas-btn-quiet", "Refresh");
    refresh.setAttribute("aria-label", "Refresh device information");
    refresh.addEventListener("click", loadHome);
    actions.appendChild(refresh);
    container.appendChild(actions);

    var grid = el("div", "sas-cards");
    els.chipCard = card(grid, "Chip");
    els.runtimeCard = card(grid, "Runtime");
    els.wifiCard = card(grid, "Wi-Fi");
    els.bootCard = card(grid, "Boot status");
    container.appendChild(grid);

    loadHome();
  }

  function loadHome() {
    request("GET", "/system/info")
      .then(function (info) {
        setRows(els.chipCard, [
          ["Model", info.chip_model],
          ["Revision", String(info.chip_revision)],
          ["Flash", formatBytes(info.flash_size_bytes)],
          ["Firmware", info.firmware_version || "unknown"],
        ]);
      })
      .catch(cardError(els.chipCard));

    Promise.all([request("GET", "/system/runtime"), request("GET", "/system/reset-reason")])
      .then(function (results) {
        els.runtimeCard.dataset.resetReason = results[1].reset_reason;
        updateRuntimeCard(results[0]);
      })
      .catch(cardError(els.runtimeCard));

    request("GET", "/system/boot-status")
      .then(function (boot) {
        setRows(
          els.bootCard,
          (boot.modules || []).map(function (m) {
            return [m.name, m.status.replace("_", " "), m.status];
          })
        );
      })
      .catch(cardError(els.bootCard));

    /* /wifi/status needs a session; before login the card fills from "wifi_state" events. */
    if (isLoggedIn()) {
      request("GET", "/wifi/status")
        .then(function (wifi) {
          state.wifi = wifi;
          updateWifiStatus();
        })
        .catch(cardError(els.wifiCard));
    } else {
      updateWifiStatus();
    }
  }

  function updateRuntimeCard(info) {
    if (currentViewId() !== "home" || !els.runtimeCard) {
      return;
    }
    setRows(els.runtimeCard, [
      ["Free heap", formatBytes(info.free_heap_bytes)],
      ["Minimum free heap", formatBytes(info.min_free_heap_bytes)],
      ["Uptime", formatDuration(info.uptime_seconds)],
      ["Last reset", (els.runtimeCard.dataset.resetReason || "").replace(/_/g, " ")],
    ]);
  }

  /* Wi-Fi status appears on both the Home card and the Wi-Fi view. */
  function updateWifiStatus() {
    var wifi = state.wifi;
    if (currentViewId() === "home" && els.wifiCard) {
      if (!wifi) {
        setRows(els.wifiCard, [["State", isLoggedIn() ? "unknown" : "log in to view"]]);
      } else {
        setRows(els.wifiCard, wifiRows(wifi));
      }
    }
    if (currentViewId() === "wifi" && els.wifiStatusRows && wifi) {
      setRows(els.wifiStatusRows, wifiRows(wifi));
    }
  }

  function wifiRows(wifi) {
    return [
      ["State", (wifi.state || "unknown").replace("_", " "), wifi.state],
      ["SSID", wifi.ssid || "(none)"],
      ["RSSI", wifi.state === "connected" ? wifi.rssi + " dBm" : "n/a"],
    ];
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
    container.appendChild(el("h1", null, "Wi-Fi"));

    var statusCard = el("div", "sas-card");
    statusCard.appendChild(el("h2", null, "Status"));
    var rows = el("dl", "sas-rows");
    statusCard.appendChild(rows);
    container.appendChild(statusCard);
    els.wifiStatusRows = statusCard;

    var actions = el("div", "sas-view-actions");
    var scanBtn = el("button", "sas-btn", "Scan");
    scanBtn.addEventListener("click", runWifiScan);
    var statusText = el("span", "sas-status-text");
    actions.appendChild(scanBtn);
    actions.appendChild(statusText);
    container.appendChild(actions);
    els.wifiScanBtn = scanBtn;
    els.wifiStatusText = statusText;

    var tableWrap = el("div", "sas-table-wrap");
    container.appendChild(tableWrap);
    els.wifiTableWrap = tableWrap;

    var formWrap = el("div");
    container.appendChild(formWrap);
    els.wifiFormWrap = formWrap;

    request("GET", "/wifi/status")
      .then(function (wifi) {
        state.wifi = wifi;
        updateWifiStatus();
      })
      .catch(function () {
        /* the next "wifi_state" event fills it in */
      });

    renderWifiTable();
    if (state.wifiResults.length === 0) {
      runWifiScan();
    }
  }

  function runWifiScan() {
    if (state.wifiScanInFlight) {
      return;
    }
    state.wifiScanInFlight = true;
    els.wifiScanBtn.disabled = true;
    els.wifiScanBtn.classList.add("sas-busy");
    els.wifiStatusText.textContent = "Scanning...";

    request("GET", "/wifi/scan")
      .then(function (result) {
        state.wifiResults = result.networks || [];
        els.wifiStatusText.textContent = state.wifiResults.length + " access point(s) found";
        renderWifiTable();
      })
      .catch(function (err) {
        els.wifiStatusText.textContent = "Scan failed: " + err.message;
      })
      .then(function () {
        state.wifiScanInFlight = false;
        if (els.wifiScanBtn) {
          els.wifiScanBtn.disabled = false;
          els.wifiScanBtn.classList.remove("sas-busy");
        }
      });
  }

  function signalBars(rssi) {
    var level = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
    var bars = el("span", "sas-bars");
    bars.setAttribute("aria-label", rssi + " dBm");
    bars.title = rssi + " dBm";
    for (var i = 1; i <= 4; i++) {
      var bar = el("span", i <= level ? "on" : "");
      bar.style.height = i * 25 + "%";
      bars.appendChild(bar);
    }
    return bars;
  }

  function renderWifiTable() {
    if (!els.wifiTableWrap) {
      return;
    }
    els.wifiTableWrap.innerHTML = "";

    if (state.wifiResults.length === 0) {
      els.wifiTableWrap.appendChild(el("div", "sas-empty", "No access points found yet."));
      return;
    }

    var table = el("table", "sas-table");
    table.innerHTML =
      "<thead><tr><th>SSID</th><th>Signal</th><th>Security</th><th></th></tr></thead>";
    var tbody = document.createElement("tbody");
    state.wifiResults
      .slice()
      .sort(function (a, b) {
        return b.rssi - a.rssi;
      })
      .forEach(function (ap) {
        var tr = document.createElement("tr");
        appendCell(tr, ap.ssid || "(hidden)");
        tr.appendChild(document.createElement("td")).appendChild(signalBars(ap.rssi));
        appendCell(tr, (ap.auth_mode !== 0 ? "🔒 " : "") + wifiAuthModeName(ap.auth_mode));
        var connect = el("button", "sas-btn sas-btn-quiet", "Connect");
        connect.disabled = !ap.ssid;
        connect.addEventListener("click", function () {
          renderWifiConnectForm(ap);
        });
        tr.appendChild(document.createElement("td")).appendChild(connect);
        tbody.appendChild(tr);
      });
    table.appendChild(tbody);
    els.wifiTableWrap.appendChild(table);
  }

  function renderWifiConnectForm(ap) {
    var wrap = els.wifiFormWrap;
    wrap.innerHTML = "";

    var form = el("form", "sas-form sas-card");
    form.appendChild(el("h2", null, "Connect to a network"));
    var ssid = inputField(form, "SSID", "text", "off");
    ssid.value = ap.ssid;
    var pass = inputField(form, "Password", "password", "off");
    var submit = el("button", "sas-btn", "Connect");
    submit.type = "submit";
    form.appendChild(submit);
    var message = el("p", "sas-error");
    message.setAttribute("role", "alert");
    form.appendChild(message);
    wrap.appendChild(form);
    (ap.auth_mode === 0 ? submit : pass).focus();

    form.addEventListener("submit", function (evt) {
      evt.preventDefault();
      message.textContent = "";
      submit.disabled = true;
      request("POST", "/wifi/connect", { ssid: ssid.value, password: pass.value })
        .then(function () {
          pass.value = "";
          message.className = "sas-status-text";
          message.textContent =
            "Connecting... if the device moves to another network, reopen the dashboard there.";
        })
        .catch(function (err) {
          message.className = "sas-error";
          message.textContent = err.message;
        })
        .then(function () {
          submit.disabled = false;
        });
    });
  }

  /* ---- BLE view ---- */

  function upsertBleDevice(device) {
    if (!device || !device.mac) {
      return;
    }
    state.bleDevices[device.mac] = device;
  }

  function renderBle(container) {
    container.appendChild(el("h1", null, "BLE"));

    var actions = el("div", "sas-view-actions");
    var startBtn = el("button", "sas-btn", "Start scan");
    var stopBtn = el("button", "sas-btn sas-btn-quiet", "Stop scan");
    var status = el("span", "sas-status-text", "Passive scan: SAS never connects to a device.");
    startBtn.addEventListener("click", function () {
      bleScan("start", status);
    });
    stopBtn.addEventListener("click", function () {
      bleScan("stop", status);
    });
    actions.appendChild(startBtn);
    actions.appendChild(stopBtn);
    actions.appendChild(status);
    container.appendChild(actions);

    var tableWrap = el("div", "sas-table-wrap");
    container.appendChild(tableWrap);
    els.bleTableWrap = tableWrap;

    request("GET", "/ble/devices")
      .then(function (result) {
        (result.devices || []).forEach(upsertBleDevice);
      })
      .catch(function () {
        /* table still fills from "ble_device" events */
      })
      .then(renderBleTable);
    renderBleTable();
  }

  function bleScan(action, status) {
    request("POST", "/ble/scan/" + action)
      .then(function () {
        status.textContent = action === "start" ? "Scanning..." : "Scan stopped.";
      })
      .catch(function (err) {
        status.textContent = "Could not " + action + " the scan: " + err.message;
      });
  }

  var BLE_COLUMNS = [
    { key: "mac", label: "MAC" },
    { key: "name", label: "Name" },
    { key: "rssi", label: "RSSI" },
    { key: "last_seen_us", label: "Last seen" },
  ];

  function renderBleTable() {
    if (!els.bleTableWrap) {
      return;
    }
    var devices = Object.keys(state.bleDevices).map(function (mac) {
      return state.bleDevices[mac];
    });

    els.bleTableWrap.innerHTML = "";
    if (devices.length === 0) {
      els.bleTableWrap.appendChild(el("div", "sas-empty", "No BLE devices observed yet."));
      return;
    }

    var sort = state.bleSort;
    devices.sort(function (a, b) {
      var x = a[sort.key];
      var y = b[sort.key];
      var order = x < y ? -1 : x > y ? 1 : 0;
      return sort.descending ? -order : order;
    });

    var table = el("table", "sas-table");
    var headRow = document.createElement("tr");
    BLE_COLUMNS.forEach(function (column) {
      var th = document.createElement("th");
      var btn = el("button", "sas-sort", column.label);
      if (sort.key === column.key) {
        btn.textContent += sort.descending ? " ▼" : " ▲";
        th.setAttribute("aria-sort", sort.descending ? "descending" : "ascending");
      }
      btn.addEventListener("click", function () {
        state.bleSort = {
          key: column.key,
          descending: sort.key === column.key ? !sort.descending : column.key !== "mac",
        };
        renderBleTable();
      });
      th.appendChild(btn);
      headRow.appendChild(th);
    });
    table.appendChild(document.createElement("thead")).appendChild(headRow);

    var tbody = document.createElement("tbody");
    devices.forEach(function (dev) {
      var tr = document.createElement("tr");
      appendCell(tr, dev.mac);
      appendCell(tr, dev.name || "(unnamed)");
      appendCell(tr, dev.rssi + " dBm");
      appendCell(tr, formatSinceBoot(dev.last_seen_us));
      tbody.appendChild(tr);
    });
    table.appendChild(tbody);
    els.bleTableWrap.appendChild(table);
  }

  /* ---- Files view ---- */

  function joinPath(dir, name) {
    return (dir === "/" ? "" : dir) + "/" + name;
  }

  function renderFiles(container) {
    container.appendChild(el("h1", null, "Files"));

    els.filesUsage = el("div", "sas-usage");
    container.appendChild(els.filesUsage);

    els.filesCrumbs = el("nav", "sas-crumbs");
    els.filesCrumbs.setAttribute("aria-label", "Current directory");
    container.appendChild(els.filesCrumbs);

    els.filesTableWrap = el("div", "sas-table-wrap");
    container.appendChild(els.filesTableWrap);

    var form = el("form", "sas-form sas-card");
    form.appendChild(el("h2", null, "Upload"));
    var file = inputField(form, "File", "file");
    var dest = inputField(form, "Destination path", "text", "off");
    var submit = el("button", "sas-btn", "Upload");
    submit.type = "submit";
    form.appendChild(submit);
    var progress = progressBar(form);
    container.appendChild(form);
    els.filesDest = dest;

    file.addEventListener("change", function () {
      if (file.files.length > 0) {
        dest.value = joinPath(state.filesPath, file.files[0].name);
      }
    });

    form.addEventListener("submit", function (evt) {
      evt.preventDefault();
      if (file.files.length === 0 || !dest.value) {
        toast("Choose a file and a destination path first");
        return;
      }
      submit.disabled = true;
      progress.set(0);
      uploadWithProgress(
        "/files/upload?path=" + encodeURIComponent(dest.value),
        file.files[0],
        progress.set
      )
        .then(function () {
          toast("Uploaded " + dest.value);
          form.reset();
          loadFiles();
        })
        .catch(function (err) {
          toast("Upload failed: " + err.message); /* listing is left unchanged */
        })
        .then(function () {
          submit.disabled = false;
          progress.hide();
        });
    });

    loadFiles();
  }

  function loadFiles() {
    request("GET", "/files/usage")
      .then(function (usage) {
        els.filesUsage.innerHTML = "";
        var bar = progressBar(els.filesUsage);
        bar.set(usage.total_bytes ? Math.round((usage.used_bytes * 100) / usage.total_bytes) : 0);
        els.filesUsage.appendChild(
          el(
            "span",
            "sas-status-text",
            formatBytes(usage.used_bytes) + " used of " + formatBytes(usage.total_bytes)
          )
        );
      })
      .catch(function () {
        els.filesUsage.textContent = "";
      });

    renderCrumbs();
    request("GET", "/files?path=" + encodeURIComponent(state.filesPath))
      .then(function (result) {
        renderFilesTable(result.entries || []);
      })
      .catch(function (err) {
        els.filesTableWrap.innerHTML = "";
        els.filesTableWrap.appendChild(el("div", "sas-empty", "Could not list: " + err.message));
      });
  }

  function openDirectory(path) {
    state.filesPath = path;
    loadFiles();
  }

  function renderCrumbs() {
    els.filesCrumbs.innerHTML = "";
    var parts = state.filesPath.split("/").filter(Boolean);
    var path = "";

    function crumb(label, target) {
      var btn = el("button", "sas-link", label);
      btn.addEventListener("click", function () {
        openDirectory(target);
      });
      els.filesCrumbs.appendChild(btn);
    }

    crumb("root", "/");
    parts.forEach(function (part) {
      path += "/" + part;
      els.filesCrumbs.appendChild(el("span", "sas-status-text", " / "));
      crumb(part, path);
    });
  }

  function renderFilesTable(entries) {
    var wrap = els.filesTableWrap;
    wrap.innerHTML = "";
    if (entries.length === 0) {
      wrap.appendChild(el("div", "sas-empty", "This directory is empty."));
      return;
    }

    entries.sort(function (a, b) {
      if (a.is_dir !== b.is_dir) {
        return a.is_dir ? -1 : 1;
      }
      return a.name < b.name ? -1 : 1;
    });

    var table = el("table", "sas-table");
    table.innerHTML =
      "<thead><tr><th>Name</th><th>Size</th><th>Type</th><th>Actions</th></tr></thead>";
    var tbody = document.createElement("tbody");
    entries.forEach(function (entry) {
      tbody.appendChild(fileRow(entry));
    });
    table.appendChild(tbody);
    wrap.appendChild(table);
  }

  function fileRow(entry) {
    var path = joinPath(state.filesPath, entry.name);
    var tr = document.createElement("tr");

    var nameCell = document.createElement("td");
    if (entry.is_dir) {
      var open = el("button", "sas-link", entry.name + "/");
      open.addEventListener("click", function () {
        openDirectory(path);
      });
      nameCell.appendChild(open);
    } else {
      nameCell.textContent = entry.name;
    }
    tr.appendChild(nameCell);
    appendCell(tr, entry.is_dir ? "" : formatBytes(entry.size_bytes));
    appendCell(tr, entry.is_dir ? "Directory" : "File");

    var actions = el("td", "sas-row-actions");
    if (!entry.is_dir) {
      var download = el("button", "sas-btn sas-btn-quiet", "Download");
      download.addEventListener("click", function () {
        downloadFile(path, entry.name);
      });
      actions.appendChild(download);

      var rename = el("button", "sas-btn sas-btn-quiet", "Rename");
      rename.addEventListener("click", function () {
        startRename(nameCell, entry.name);
      });
      actions.appendChild(rename);

      actions.appendChild(deleteButton(path));
    }
    tr.appendChild(actions);
    return tr;
  }

  /* The download endpoint needs the Authorization header, so a plain link cannot be used. */
  function downloadFile(path, name) {
    fetch(API + "/files/download?path=" + encodeURIComponent(path), { headers: authHeaders() })
      .then(function (resp) {
        if (!resp.ok) {
          throw new Error("HTTP " + resp.status);
        }
        return resp.blob();
      })
      .then(function (blob) {
        var a = document.createElement("a");
        a.href = URL.createObjectURL(blob);
        a.download = name;
        document.body.appendChild(a);
        a.click();
        a.remove();
        URL.revokeObjectURL(a.href);
      })
      .catch(function (err) {
        toast("Download failed: " + err.message);
      });
  }

  /* Inline rename: the name cell becomes a text field, committed on Enter or blur-with-change. */
  function startRename(nameCell, oldName) {
    var input = document.createElement("input");
    input.type = "text";
    input.value = oldName;
    input.setAttribute("aria-label", "New name for " + oldName);
    nameCell.innerHTML = "";
    nameCell.appendChild(input);
    input.focus();
    input.select();

    var done = false;
    function commit() {
      if (done) {
        return;
      }
      done = true;
      var newName = input.value.trim();
      if (!newName || newName === oldName) {
        nameCell.textContent = oldName;
        return;
      }
      request("POST", "/files/rename", {
        old_path: joinPath(state.filesPath, oldName),
        new_path: joinPath(state.filesPath, newName),
      })
        .catch(function (err) {
          toast("Rename failed: " + err.message);
        })
        .then(loadFiles);
    }

    input.addEventListener("blur", commit);
    input.addEventListener("keydown", function (evt) {
      if (evt.key === "Enter") {
        commit();
      } else if (evt.key === "Escape") {
        done = true;
        nameCell.textContent = oldName;
      }
    });
  }

  /* Inline confirm: first click arms the button for 3 seconds, a second click deletes. */
  function deleteButton(path) {
    var btn = el("button", "sas-btn sas-btn-danger", "Delete");
    var armedTimer = null;
    btn.addEventListener("click", function () {
      if (armedTimer === null) {
        btn.textContent = "Confirm delete?";
        armedTimer = window.setTimeout(function () {
          armedTimer = null;
          btn.textContent = "Delete";
        }, 3000);
        return;
      }
      window.clearTimeout(armedTimer);
      armedTimer = null;
      btn.disabled = true;
      request("DELETE", "/files?path=" + encodeURIComponent(path))
        .catch(function (err) {
          toast("Delete failed: " + err.message);
        })
        .then(loadFiles);
    });
    return btn;
  }

  /* ---- OTA view ---- */

  function renderOta(container) {
    container.appendChild(el("h1", null, "OTA update"));

    var statusCard = el("div", "sas-card");
    statusCard.appendChild(el("h2", null, "Status"));
    statusCard.appendChild(el("dl", "sas-rows"));
    container.appendChild(statusCard);
    els.otaStatusCard = statusCard;
    els.otaProgress = progressBar(statusCard);
    els.otaProgress.hide();

    var reboot = el("div", "sas-card");
    reboot.hidden = true;
    reboot.appendChild(
      el(
        "p",
        null,
        "The new firmware is installed and verified. Rebooting drops this connection until " +
          "the device rejoins the network."
      )
    );
    var rebootBtn = el("button", "sas-btn", "Reboot now");
    rebootBtn.addEventListener("click", function () {
      rebootBtn.disabled = true;
      request("POST", "/system/reboot")
        .then(function () {
          toast("Rebooting... reload this page in a few seconds");
        })
        .catch(function (err) {
          rebootBtn.disabled = false;
          toast("Reboot failed: " + err.message);
        });
    });
    reboot.appendChild(rebootBtn);
    container.appendChild(reboot);
    els.otaReboot = reboot;

    var uploadForm = el("form", "sas-form sas-card");
    uploadForm.appendChild(el("h2", null, "Upload a firmware image"));
    var file = inputField(uploadForm, "Firmware file (.bin)", "file");
    file.accept = ".bin";
    var uploadBtn = el("button", "sas-btn", "Upload and install");
    uploadBtn.type = "submit";
    uploadForm.appendChild(uploadBtn);
    var uploadProgress = progressBar(uploadForm);
    uploadProgress.hide();
    container.appendChild(uploadForm);

    var urlForm = el("form", "sas-form sas-card");
    urlForm.appendChild(el("h2", null, "Install from a URL"));
    var url = inputField(urlForm, "Image URL", "url", "off");
    url.placeholder = "https://example.com/firmware.bin";
    var urlBtn = el("button", "sas-btn", "Download and install");
    urlBtn.type = "submit";
    urlForm.appendChild(urlBtn);
    container.appendChild(urlForm);

    els.otaControls = [file, uploadBtn, url, urlBtn];
    els.otaVersion = "";

    uploadForm.addEventListener("submit", function (evt) {
      evt.preventDefault();
      if (file.files.length === 0) {
        toast("Choose a firmware file first");
        return;
      }
      /* The device cannot push "ota_progress" events while it is busy receiving this very
       * request, so upload progress comes from the browser. */
      state.ota = { state: "receiving", percent_complete: 0 };
      updateOtaStatus();
      uploadProgress.set(0);
      uploadWithProgress("/ota/upload", file.files[0], uploadProgress.set)
        .catch(function (err) {
          toast("Update failed: " + err.message);
        })
        .then(function () {
          uploadProgress.hide();
          loadOtaStatus();
        });
    });

    urlForm.addEventListener("submit", function (evt) {
      evt.preventDefault();
      request("POST", "/ota/url", { url: url.value })
        .then(function () {
          state.ota = { state: "receiving", percent_complete: 0 };
          updateOtaStatus();
        })
        .catch(function (err) {
          toast("Could not start the download: " + err.message);
        });
    });

    request("GET", "/system/info")
      .then(function (info) {
        els.otaVersion = info.firmware_version || "";
        updateOtaStatus();
      })
      .catch(function () {
        /* version row stays "unknown" */
      });
    loadOtaStatus();
  }

  function loadOtaStatus() {
    request("GET", "/ota/status")
      .then(function (status) {
        state.ota = status;
        updateOtaStatus();
      })
      .catch(function () {
        /* keep showing the last known state */
      });
  }

  function updateOtaStatus() {
    if (currentViewId() !== "ota" || !els.otaStatusCard) {
      return;
    }
    var ota = state.ota;
    var busy = ota.state === "receiving" || ota.state === "verifying";

    setRows(els.otaStatusCard, [
      ["Running firmware", els.otaVersion || "unknown"],
      ["Update state", ota.state.replace(/_/g, " "), ota.state],
    ]);
    if (busy) {
      els.otaProgress.set(ota.percent_complete || 0);
    } else {
      els.otaProgress.hide();
    }
    els.otaReboot.hidden = ota.state !== "ready_to_reboot";

    /* No second update while one is running or waiting for its reboot. */
    var lock = busy || ota.state === "ready_to_reboot";
    els.otaControls.forEach(function (control) {
      control.disabled = lock;
    });
  }

  /* ---- Terminal view (js/terminal.js) ---- */

  function renderTerminal(container) {
    container.appendChild(el("h1", null, "Terminal"));
    if (!window.SASTerminal) {
      container.appendChild(el("div", "sas-empty", "Terminal script failed to load."));
      return;
    }
    window.SASTerminal.render(container, {
      send: function (commandLine) {
        return wsSend("terminal_cmd", { command: commandLine });
      },
    });
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
    container.appendChild(el("h1", null, "Logs"));

    var pane = el("div", "sas-log-pane");
    pane.setAttribute("role", "log");
    pane.setAttribute("aria-live", "polite");
    container.appendChild(pane);
    els.logPane = pane;

    request("GET", "/logs")
      .then(function (result) {
        /* The history replaces whatever arrived live, so entries are not shown twice. */
        state.logLines = (result.entries || []).slice(-MAX_LOG_LINES);
      })
      .catch(function () {
        /* keep the live entries collected so far */
      })
      .then(renderLogPane);
    renderLogPane();
  }

  function renderLogPane() {
    if (!els.logPane) {
      return;
    }
    els.logPane.innerHTML = "";
    if (state.logLines.length === 0) {
      els.logPane.appendChild(el("div", "sas-empty", "No log entries yet."));
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
    var line = el("div", "sas-log-line");
    line.appendChild(el("span", "sas-log-ts", formatSinceBoot(entry.timestamp_us)));

    var pill = el("span", "sas-pill", entry.level || "INFO");
    pill.setAttribute("data-level", entry.level || "INFO");
    line.appendChild(pill);

    line.appendChild(el("span", "sas-log-tag", entry.tag || ""));
    line.appendChild(el("span", null, entry.message || ""));
    return line;
  }

  /* ---- Account view ---- */

  function renderAccount(container) {
    container.appendChild(el("h1", null, "Account"));

    if (!state.authEnabled) {
      container.appendChild(
        el("div", "sas-empty", "Authentication is disabled on this device (auth.enabled).")
      );
      return;
    }

    var form = el("form", "sas-form sas-card");
    form.appendChild(el("h2", null, "Change password"));
    var current = inputField(form, "Current password", "password", "current-password");
    var next = inputField(form, "New password (8 to 64 characters)", "password", "new-password");
    var repeat = inputField(form, "Repeat new password", "password", "new-password");
    var submit = el("button", "sas-btn", "Change password");
    submit.type = "submit";
    form.appendChild(submit);
    var error = el("p", "sas-error");
    error.setAttribute("role", "alert");
    form.appendChild(error);
    container.appendChild(form);

    form.addEventListener("submit", function (evt) {
      evt.preventDefault();
      error.textContent = "";
      if (next.value !== repeat.value) {
        error.textContent = "The new passwords do not match";
        return;
      }
      submit.disabled = true;
      request("POST", "/auth/change-password", {
        current_password: current.value,
        new_password: next.value,
      })
        .then(function () {
          /* The device ends every session on a credential change, this one included. */
          toast("Password changed - log in with the new password");
          setToken(null);
          showLogin("home");
        })
        .catch(function (err) {
          if (err.status === 401 && currentViewId() === "login") {
            return; /* session expired: already sent to the Login view */
          }
          error.textContent = err.message;
          submit.disabled = false;
        });
    });
  }

  /* ---- shared DOM helpers ---- */

  function el(tag, className, text) {
    var node = document.createElement(tag);
    if (className) {
      node.className = className;
    }
    if (text !== undefined) {
      node.textContent = text;
    }
    return node;
  }

  function appendCell(tr, text) {
    var td = document.createElement("td");
    td.textContent = text;
    tr.appendChild(td);
  }

  function inputField(form, labelText, type, autocomplete) {
    var label = el("label", "sas-field");
    label.appendChild(el("span", null, labelText));
    var input = document.createElement("input");
    input.type = type;
    if (autocomplete) {
      input.autocomplete = autocomplete;
    }
    label.appendChild(input);
    form.appendChild(label);
    return input;
  }

  function card(parent, title) {
    var node = el("section", "sas-card");
    node.appendChild(el("h2", null, title));
    node.appendChild(el("dl", "sas-rows"));
    parent.appendChild(node);
    return node;
  }

  /* rows: [label, value, optional data-status]; status text always accompanies its color. */
  function setRows(cardEl, rows) {
    var dl = cardEl.querySelector(".sas-rows");
    dl.innerHTML = "";
    rows.forEach(function (row) {
      dl.appendChild(el("dt", null, row[0]));
      var dd = el("dd", null, row[1]);
      if (row[2]) {
        dd.setAttribute("data-status", row[2]);
      }
      dl.appendChild(dd);
    });
  }

  function cardError(cardEl) {
    return function (err) {
      if (cardEl) {
        setRows(cardEl, [["Unavailable", err.message]]);
      }
    };
  }

  function progressBar(parent) {
    var bar = document.createElement("progress");
    bar.className = "sas-progress";
    bar.max = 100;
    bar.value = 0;
    var label = el("span", "sas-status-text");
    parent.appendChild(bar);
    parent.appendChild(label);
    return {
      set: function (percent) {
        bar.hidden = false;
        label.hidden = false;
        bar.value = percent;
        label.textContent = " " + percent + "%";
      },
      hide: function () {
        bar.hidden = true;
        label.hidden = true;
      },
    };
  }

  /* ---- formatting ---- */

  function formatBytes(bytes) {
    if (typeof bytes !== "number") {
      return "";
    }
    if (bytes >= 1048576) {
      return (bytes / 1048576).toFixed(1) + " MB";
    }
    if (bytes >= 1024) {
      return (bytes / 1024).toFixed(1) + " KB";
    }
    return bytes + " B";
  }

  function formatDuration(seconds) {
    if (typeof seconds !== "number") {
      return "";
    }
    var h = Math.floor(seconds / 3600);
    var m = Math.floor((seconds % 3600) / 60);
    var s = seconds % 60;
    return (h > 0 ? h + "h " : "") + (h > 0 || m > 0 ? m + "m " : "") + s + "s";
  }

  /* The device has no wall clock: its timestamps are microseconds since boot. */
  function formatSinceBoot(timestampUs) {
    if (typeof timestampUs !== "number") {
      return "";
    }
    return (timestampUs / 1e6).toFixed(3) + "s";
  }
})();
