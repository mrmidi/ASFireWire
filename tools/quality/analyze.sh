#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

if ! command -v xcodebuild >/dev/null 2>&1; then
  echo "xcodebuild not found."
  exit 2
fi

DERIVED_DATA_PATH="${ROOT}/build/DerivedData"
mkdir -p "${DERIVED_DATA_PATH}"

set -o pipefail
if command -v xcpretty >/dev/null 2>&1; then
  xcodebuild \
    -project ASFW.xcodeproj \
    -scheme ASFW \
    -configuration Debug \
    -derivedDataPath "${DERIVED_DATA_PATH}" \
    analyze \
    | tee analyze.log \
    | xcpretty
else
  xcodebuild \
    -project ASFW.xcodeproj \
    -scheme ASFW \
    -configuration Debug \
    -derivedDataPath "${DERIVED_DATA_PATH}" \
    analyze \
    | tee analyze.log
fi
