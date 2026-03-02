#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

"${ROOT}/tools/quality/format.sh"
"${ROOT}/tools/quality/tidy.sh"

if [[ -x "${ROOT}/build.sh" ]]; then
  "${ROOT}/build.sh" --test-only --no-bump
else
  echo "build.sh not found or not executable."
  exit 2
fi

"${ROOT}/tools/quality/analyze.sh"
