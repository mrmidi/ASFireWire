#!/usr/bin/env bash
# swift build wiper ad-hoc-signaturen hver gang -> re-sign, ellers 0xe00002e2 paa IOServiceOpen
set -euo pipefail
cd "$(dirname "$0")"
swift build "$@"
codesign --force --sign - --entitlements CoolScanProbe.entitlements .build/debug/CoolScanProbe
echo "✅ bygd + signert: .build/debug/CoolScanProbe"
