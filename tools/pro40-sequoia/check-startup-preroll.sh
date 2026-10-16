#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/pro40-sequoia/startup-preroll
mkdir -p "$out"
flags=(-std=c++20 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined)
wire=(ASFWDriver/Audio/Wire/IEC61883/CipHeader.cpp ASFWDriver/Audio/Wire/IEC61883/DbcCounter.cpp)
for test in StartupPreroll SustainedNativeReplay SustainedInput SustainedBridge Replay Packet SustainedClockPublication; do
  xcrun clang++ "${flags[@]}" "tools/pro40-sequoia/${test}Checks.cpp" "${wire[@]}" \
    -framework CoreAudio -framework CoreFoundation -o "$out/${test}Checks"
  "$out/${test}Checks"
done
# Optional path to the private 2026-09-20 rejected RX0 capture. Never uploaded.
if [[ $# == 1 ]]; then "$out/StartupPrerollChecks" "$1"; fi
