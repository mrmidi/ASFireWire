#!/usr/bin/env bash
# Build rtl_loopback (and every other host measurement tool) into build/tools
# and copy the macOS binaries next to their sources for convenience.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cmake -S "$ROOT/tools" -B "$ROOT/build/tools" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build/tools" -j
if [[ -x "$ROOT/build/tools/rtl/rtl_loopback" ]]; then
    cp "$ROOT/build/tools/rtl/rtl_loopback" "$ROOT/tools/rtl/rtl_loopback"
    echo "built tools/rtl/rtl_loopback"
else
    echo "rtl_loopback is macOS-only; built the analysis core only" >&2
fi
