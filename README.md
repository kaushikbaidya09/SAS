# ESP32-S3 Swiss Army Server (SAS)

ESP-IDF firmware for the ESP32-S3: Wi-Fi manager, BLE scanner, web server with
REST API and WebSocket, file manager, OTA updates, terminal and a web dashboard.
The code follows BARR-C:2018, MISRA C:2012 and Doxygen conventions as set out
in `docs/CODING_STANDARD.md`.

## Features

- **Dashboard** for desktop and phone: Home (chip, memory, uptime, boot
  status), Wi-Fi, BLE, Files, OTA, Terminal, Logs and Account views.
- **Wi-Fi**: scan and connect from the dashboard. With no stored network the
  device opens its own access point, `SAS_KB`.
- **Found by name**: the device answers at `http://sas_kb.local` (mDNS), so its
  IP address does not need to be known. The name can be changed in the Wi-Fi
  view; give each board its own name if several share a network.
- **BLE**: passive scan listing nearby advertisers. SAS never connects to them.
- **Files**: list, upload, download, rename and delete on the LittleFS partition.
- **OTA**: firmware update by file upload or from a URL, with rollback if the new
  image fails to boot.
- **Terminal**: `help`, `sysinfo`, `tasks`, `boot`, `wifi`, `reboot`.
- **Login**: session tokens, per-client throttling of failed attempts.
- **REST API and WebSocket** under `/api/v1` and `/ws`.

## First use

1. Build and flash (see [Build](#build)).
2. Join the Wi-Fi network `SAS_KB` (open, no password) and browse to
   `http://192.168.4.1`.
3. Log in with `admin` / `admin` and change the password in the Account view.
4. In the Wi-Fi view press Scan, choose a network and connect. The access point
   switches off once the device joins that network; it comes back if the
   connection fails.
5. Reopen the dashboard at `http://sas_kb.local`. If the name does not resolve
   (some networks block mDNS), use the IP address printed on the serial
   monitor.

## Creating a new module

1. Copy `templates/template.h` and `templates/template.c` into the module's
   subsystem folder, e.g. `main/src/network/sas_mqtt.h` and `sas_mqtt.c`.
2. Replace `template` / `TEMPLATE` with `sas_mqtt` / `SAS_MQTT` throughout.
3. Fill in the `\brief` lines and delete the parts you do not need.
4. Run `docs/tools/format.sh` and the `check` and `docs` targets below; all three must be clean.

A new subsystem folder also needs adding to `SRC_DIRS` and `INCLUDE_DIRS` in
`main/CMakeLists.txt`.

## Build

Requires ESP-IDF v6.0 and GNU Make. `docs/tools/run.sh` finds ESP-IDF by itself;
the plain `idf.py` commands need its environment loaded in the terminal first.

```sh
docs/tools/run.sh --build            # build the firmware
docs/tools/run.sh --flash            # flash firmware and dashboard to the board
docs/tools/run.sh --monitor          # serial monitor (exit with Ctrl+])
docs/tools/run.sh --all              # clean, build from scratch, flash, monitor
docs/tools/run.sh --clean            # delete everything generated or downloaded
docs/tools/run.sh --flash --port /dev/ttyUSB0   # another serial port (default /dev/ttyACM0)

docs/tools/format.sh                 # format every .c/.h file (--check to only report)
cmake --build build --target check   # cppcheck static analysis, fails on any finding
cmake --build build --target docs    # Doxygen HTML → build/docs/html/index.html
```

The steps can be combined (`run.sh --build --flash --monitor`) and always run in
the order clean, build, flash, monitor.

The same without the script:

```sh
idf.py -G "Unix Makefiles" build     # first build: configure for GNU Make
idf.py build                         # later builds reuse that generator
idf.py -p /dev/ttyACM0 flash monitor
```

The build uses GNU Make, not Ninja: a build directory configured for Ninja is
refused with a message saying how to switch. Make is run on every CPU core.

All output goes to `build/`. `--clean` deletes it together with
`managed_components/` (components fetched by ESP-IDF, downloaded again on the
next build) and `.cache/` (the editor's code index). `sdkconfig` is kept.

Every build first runs `docs/tools/format.sh`, so all `.c`/`.h` files are
reformatted in place before they are compiled (skipped with a warning if
`clang-format` is not installed). Third-party code in `managed_components/` is
never touched.

The build also compiles `templates/template.c` with the full warning set
(`-Wall -Wextra -Werror -Wconversion -pedantic` and more), so the blank
skeletons always meet the standard.

### What flashing overwrites

`--flash` writes the bootloader, partition table, firmware and the LittleFS
image built from `littlefs_data/`. The device keeps its settings in that same
filesystem, so a flash resets the saved Wi-Fi network, the login password and
the device name to their defaults. To update a device without losing them:

- **Firmware**: upload `build/SAS.bin` in the dashboard's OTA view, then reboot.
- **Dashboard files**: upload the changed files from `littlefs_data/` to the same
  paths in the Files view, e.g. `js/dashboard.js` to `/js/dashboard.js`.

## Conformance of the existing code

The existing modules follow the standard's file headers, section order,
function header placement, comment style, include guards and formatting.
They do not yet follow its naming prefixes (section 6: `g_`, `p_`, `b_`,
module prefix on private functions) or its single-exit and constant-on-the-left
rules (section 7), and `cppcheck` still reports findings in them.

## Known limitations

- **Plain HTTP.** `SAS_HTTP_ENABLE_SSL` in `main/src/web/sas_http_server.c` is
  `0`, so the dashboard and API are served unencrypted and the login is sent in
  clear text. Do not rely on it on a network you do not trust.
- **Default login.** A device with no stored credential accepts `admin` /
  `admin` until the password is changed.
- **Software credential key.** The design calls for an eFuse key used through
  the HMAC peripheral. No key is provisioned on a development board, so a
  software key stored in the configuration file is used instead. The firmware
  never burns eFuses.
- **No wall clock.** Timestamps in logs and BLE results are time since boot.
- **No unit tests yet.**
