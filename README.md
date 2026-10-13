# ESP32-S3 Swiss Army Server (SAS)

ESP-IDF firmware for the ESP32-S3: Wi-Fi manager, BLE scanner, HTTPS server with
REST API and WebSocket, file manager, OTA updates, terminal and a web dashboard.
The code follows BARR-C:2018, MISRA C:2012 and Doxygen conventions as set out
in `docs/CODING_STANDARD.md`.

## Structure

| Path | Contents |
|---|---|
| `main/src/` | Firmware sources: `main.c` boot sequencing plus one folder per subsystem (`core`, `network`, `storage`, `web`, `terminal`, `auth`), one `.c`/`.h` pair per module |
| `main/certs/` | Development-only self-signed TLS certificate and key |
| `littlefs_data/` | Web dashboard, flashed as the LittleFS image |
| `partitions/` | Partition table |
| `templates/template.c`, `templates/template.h` | Blank module skeletons: copy and rename for each new module |
| `docs/CODING_STANDARD.md` | File/function header formats, inline comment rules, naming, MISRA essentials |
| `CMakeLists.txt` | ESP-IDF build, cppcheck, clang-format, Doxygen |
| `version.txt` | Firmware version in [Semantic Versioning](https://semver.org) format (`MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]`), read by CMake, embedded in the image and shown in the docs |
| `docs/tools/format.sh` | Scans the project for `.c`/`.h` files and formats them with clang-format (`--check` only reports) |
| `docs/tools/Doxyfile.in`, `docs/tools/.clang-format` | Documentation config template (filled in by CMake) and formatting config |

## Creating a new module

1. Copy `templates/template.h` and `templates/template.c` into the module's
   subsystem folder, e.g. `main/src/network/sas_mqtt.h` and `sas_mqtt.c`.
2. Replace `template` / `TEMPLATE` with `sas_mqtt` / `SAS_MQTT` throughout.
3. Fill in the `\brief` lines and delete the parts you do not need.
4. Run `docs/tools/format.sh` and the `check` and `docs` targets below; all three must be clean.

A new subsystem folder also needs adding to `SRC_DIRS` and `INCLUDE_DIRS` in
`main/CMakeLists.txt`.

## Build

Requires ESP-IDF v6.0 with its environment exported (`. $IDF_PATH/export.sh`).

```sh
idf.py build                         # build the firmware
idf.py -p /dev/ttyACM0 flash monitor # flash firmware and dashboard, open the console

docs/tools/format.sh                 # format every .c/.h file (--check to only report)
cmake --build build --target check   # cppcheck static analysis, fails on any finding
cmake --build build --target docs    # Doxygen HTML → build/docs/html/index.html
```

All output goes to `build/`.

Every build first runs `docs/tools/format.sh`, so all `.c`/`.h` files are
reformatted in place before they are compiled (skipped with a warning if
`clang-format` is not installed). Third-party code in `managed_components/` is
never touched.

The build also compiles `templates/template.c` with the full warning set
(`-Wall -Wextra -Werror -Wconversion -pedantic` and more), so the blank
skeletons always meet the standard.

## Conformance of the existing code

The existing modules follow the standard's file headers, section order,
function header placement, comment style, include guards and formatting.
They do not yet follow its naming prefixes (section 6: `g_`, `p_`, `b_`,
module prefix on private functions) or its single-exit and constant-on-the-left
rules (section 7), and `cppcheck` still reports findings in them.
