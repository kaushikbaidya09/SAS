/*
 * SAS Terminal view client-side logic.
 *
 * Talks to the device over the "terminal_cmd" / "terminal_output"
 * WebSocket messages. Device-side counterpart:
 * main/src/terminal/sas_terminal.{c,h}. The WebSocket itself is owned by
 * js/dashboard.js, which calls render() to build the view and onOutput()
 * for every "terminal_output" event; this file never opens a socket.
 *
 * Command history is client-side only and is not persisted anywhere. The
 * dashboard's login gate keeps this view from rendering without a
 * session; if the session expires afterwards the device answers each
 * command with a "not authenticated" line, which shows up in the
 * scrollback like any other output.
 */
(function () {
  "use strict";

  var MAX_SCROLLBACK_LINES = 1000;
  var MAX_HISTORY = 50;

  var scrollback = []; // {kind: "cmd" | "out" | "note", text}
  var history = [];
  var historyIndex = 0; // history.length means "the line being typed"
  var pane = null;

  function pushLine(kind, text) {
    scrollback.push({ kind: kind, text: text });
    if (scrollback.length > MAX_SCROLLBACK_LINES) {
      scrollback.shift();
    }
    if (pane && document.body.contains(pane)) {
      pane.appendChild(lineEl(scrollback[scrollback.length - 1]));
      while (pane.childElementCount > MAX_SCROLLBACK_LINES) {
        pane.removeChild(pane.firstChild);
      }
      pane.scrollTop = pane.scrollHeight;
    }
  }

  function lineEl(line) {
    var div = document.createElement("div");
    div.className = "sas-term-line sas-term-" + line.kind;
    div.textContent = line.text;
    return div;
  }

  function render(container, ctx) {
    pane = document.createElement("div");
    pane.className = "sas-log-pane sas-term-pane";
    pane.setAttribute("role", "log");
    pane.setAttribute("aria-live", "polite");
    scrollback.forEach(function (line) {
      pane.appendChild(lineEl(line));
    });
    container.appendChild(pane);

    var form = document.createElement("form");
    form.className = "sas-term-input";
    var prompt = document.createElement("span");
    prompt.textContent = "sas>";
    prompt.setAttribute("aria-hidden", "true");
    var input = document.createElement("input");
    input.type = "text";
    input.autocomplete = "off";
    input.spellcheck = false;
    input.setAttribute("aria-label", "Command");
    input.placeholder = "help";
    form.appendChild(prompt);
    form.appendChild(input);
    container.appendChild(form);

    form.addEventListener("submit", function (evt) {
      evt.preventDefault();
      var commandLine = input.value.trim();
      if (!commandLine) {
        return;
      }

      /* Echo first: the command line is on screen before the device's reply arrives. */
      pushLine("cmd", "sas> " + commandLine);
      if (history[history.length - 1] !== commandLine) {
        history.push(commandLine);
        if (history.length > MAX_HISTORY) {
          history.shift();
        }
      }
      historyIndex = history.length;
      input.value = "";

      if (!ctx.send(commandLine)) {
        pushLine("note", "[not connected - command not sent]");
      }
    });

    input.addEventListener("keydown", function (evt) {
      if (evt.key === "ArrowUp" && historyIndex > 0) {
        historyIndex--;
        input.value = history[historyIndex];
        evt.preventDefault();
      } else if (evt.key === "ArrowDown" && historyIndex < history.length) {
        historyIndex++;
        input.value = historyIndex < history.length ? history[historyIndex] : "";
        evt.preventDefault();
      }
    });

    pane.scrollTop = pane.scrollHeight;
    input.focus();
  }

  /*
   * data: { session, text, truncated }. One command's output arrives as
   * several messages cut at arbitrary points, so consecutive messages
   * extend the same scrollback entry instead of each starting a new line.
   */
  function onOutput(data) {
    if (!data) {
      return;
    }
    if (data.text) {
      var last = scrollback[scrollback.length - 1];
      if (last && last.kind === "out") {
        last.text += data.text;
        if (pane && document.body.contains(pane) && pane.lastChild) {
          pane.lastChild.textContent = last.text;
          pane.scrollTop = pane.scrollHeight;
        }
      } else {
        pushLine("out", data.text);
      }
    }
    if (data.truncated) {
      pushLine("note", "[output truncated]");
    }
  }

  window.SASTerminal = { render: render, onOutput: onOutput };
})();
