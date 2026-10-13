#!/usr/bin/env bash
#==============================================================================
# @file    format.sh
# @brief   Scan the project for C source files and format them with
#          clang-format, using the rules in docs/tools/.clang-format.
#
# Usage:
#   docs/tools/format.sh           Reformat every .c and .h file in place
#   docs/tools/format.sh --check   Change nothing; list files that need
#                                  formatting and exit with status 1 if any
#==============================================================================

set -euo pipefail

# Project root is two levels up from docs/tools/.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STYLE="file:$ROOT/docs/tools/.clang-format"
MODE="format"

case "${1:-}" in
    "")      ;;
    --check) MODE="check" ;;
    *)       echo "Usage: $0 [--check]" >&2; exit 2 ;;
esac

if ! command -v clang-format >/dev/null 2>&1; then
    echo "error: clang-format not found (e.g. 'sudo apt install clang-format')" >&2
    exit 2
fi

# Every .c and .h file in the project, skipping build output, generated docs
# and third-party code fetched by the ESP-IDF component manager.
mapfile -d '' FILES < <(find "$ROOT" \
    \( -path "$ROOT/build" -o -path "$ROOT/docs" -o -path "$ROOT/.git" \
       -o -path "$ROOT/managed_components" -o -path "$ROOT/.cache" \) -prune \
    -o -type f \( -name '*.c' -o -name '*.h' \) -print0 | sort -z)

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "No source files found."
    exit 0
fi

CHANGED=0

for file in "${FILES[@]}"; do
    name="${file#"$ROOT"/}"

    if clang-format --style="$STYLE" "$file" | cmp -s - "$file"; then
        continue
    fi

    CHANGED=$((CHANGED + 1))

    if [ "$MODE" = "check" ]; then
        echo "needs formatting: $name"
    else
        clang-format --style="$STYLE" -i "$file"
        echo "formatted: $name"
    fi
done

echo "Scanned ${#FILES[@]} file(s), $CHANGED $(
    [ "$MODE" = "check" ] && echo "need formatting" || echo "reformatted")."

if [ "$MODE" = "check" ] && [ "$CHANGED" -gt 0 ]; then
    exit 1
fi
