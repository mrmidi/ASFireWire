#!/usr/bin/env bash
# Build every host measurement tool (see tools/rtl/build.sh) and copy
# hal_geometry next to its source.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
"$ROOT/tools/rtl/build.sh"
if [[ -x "$ROOT/build/tools/halprobe/hal_geometry" ]]; then
    cp "$ROOT/build/tools/halprobe/hal_geometry" "$ROOT/tools/halprobe/hal_geometry"
    echo "built tools/halprobe/hal_geometry"
fi
