#!/usr/bin/env bash
#==============================================================================
# @file    run.sh
# @brief   Build, flash and monitor the SAS firmware with ESP-IDF.
#
# Run it with --help for the options.
#==============================================================================

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/run.sh [steps] [--port PORT]

Steps (combine freely; they always run in this order):
  --clean      Delete everything generated or downloaded: the build output,
               the fetched components and the editor's index cache
  --build      Build the firmware (GNU Make, not Ninja)
  --flash      Flash the firmware (it includes the dashboard)
  --monitor    Open the serial monitor (exit with Ctrl+])
  --all        All four: clean, build, flash, monitor

Options:
  --port PORT  Serial port (default: /dev/ttyACM0)
  --help       Show this help

Note: the dashboard is part of the firmware image. Flashing does not touch
the filesystem, so saved Wi-Fi networks, the login and the device name are
kept.
EOF
}

#------------------------------------------------------------------------------
# Read the command line
#------------------------------------------------------------------------------
PORT=/dev/ttyACM0
CLEAN=no
BUILD=no
FLASH=no
MONITOR=no

if [ "$#" -eq 0 ]; then
    usage
    exit 2
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --clean)   CLEAN=yes ;;
        --build)   BUILD=yes ;;
        --flash)   FLASH=yes ;;
        --monitor) MONITOR=yes ;;
        --all)     CLEAN=yes; BUILD=yes; FLASH=yes; MONITOR=yes ;;
        --port)    PORT="${2:?--port needs a value, e.g. --port /dev/ttyACM0}"; shift ;;
        --help)    usage; exit 0 ;;
        *)         echo "error: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
    shift
done

#------------------------------------------------------------------------------
# Check the board and find ESP-IDF
#------------------------------------------------------------------------------
if [ "$FLASH" = yes ] || [ "$MONITOR" = yes ]; then
    if [ ! -e "$PORT" ]; then
        echo "error: serial port $PORT not found - is the board connected?" >&2
        exit 1
    fi
fi

# 'idf' runs idf.py. In a terminal that has the ESP-IDF environment loaded,
# idf.py is already on the PATH and is used as it is.
idf() { idf.py "$@"; }

# Otherwise load the environment from the ESP-IDF installer's activation
# script. That script cannot be sourced from inside another script, but run
# with -e it prints the variables it would set, one NAME=value per line.
if ! command -v idf.py >/dev/null 2>&1; then
    activate="$(ls "$HOME"/.espressif/tools/activate_idf_*.sh 2>/dev/null | tail -n 1)"
    if [ -z "$activate" ]; then
        echo "error: ESP-IDF not found - load its environment in this terminal first" >&2
        exit 2
    fi

    while IFS='=' read -r name value; do
        if [ "$name" = PATH ]; then
            export PATH="$value:$PATH"   # add the tools, keep the system PATH
        else
            export "$name=$value"
        fi
    done < <(sh "$activate" -e)

    idf() { "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"; }
fi

#------------------------------------------------------------------------------
# Run the steps
#------------------------------------------------------------------------------
# Work from the project root, one level up from tools/.
cd "$(dirname "${BASH_SOURCE[0]}")/.."

# Everything a build generates or downloads, so the next build starts from
# nothing but the tracked sources:
#   build/               compiled objects, firmware and filesystem images
#   managed_components/  components fetched by the ESP-IDF component manager
#                        (downloaded again on the next build, needs a network)
#   .cache/              the editor's (clangd) code index
# sdkconfig is configuration, not a cache, and is kept.
if [ "$CLEAN" = yes ]; then
    echo "==> Deleting build output, fetched components and caches"
    rm -rf build managed_components .cache
fi

# -G "Unix Makefiles" selects GNU Make. idf.py runs it as "make -j <cores + 2>",
# so every CPU core is already compiling in parallel. -G only takes effect when
# the build directory is first set up; after that idf.py remembers it, so
# passing it every time does no harm.
BUILD_OPTIONS=(-G "Unix Makefiles")

if [ "$BUILD" = yes ]; then
    echo "==> Building on $(nproc) cores"
    idf "${BUILD_OPTIONS[@]}" build
fi

# 'flash' rebuilds first if anything is out of date.
if [ "$FLASH" = yes ]; then
    echo "==> Flashing to $PORT"
    idf "${BUILD_OPTIONS[@]}" -p "$PORT" flash
fi

if [ "$MONITOR" = yes ]; then
    echo "==> Serial monitor on $PORT (exit with Ctrl+])"
    idf -p "$PORT" monitor
fi
