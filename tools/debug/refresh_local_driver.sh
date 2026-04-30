#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DRIVER_ID="${ASFW_DRIVER_ID:-com.chrisizatt.ASFWLocal.ASFWDriver}"
APP_PATH="${ASFW_LOCAL_APP:-/Applications/ASFWLocal.app}"
EXPECTED_CDHASH="${ASFW_EXPECTED_CDHASH:-}"
SNAP_BASE="${ASFW_REFRESH_SNAPSHOT_DIR:-/tmp/asfw-refresh-$(date +%Y%m%d-%H%M%S)}"
TIMEOUT_SECONDS="${ASFW_REFRESH_TIMEOUT_SECONDS:-45}"

usage() {
  cat <<USAGE
Usage: $0 [--expected-cdhash HASH] [--snapshot-dir DIR]

Performs the controlled refresh needed after staging a replacement local
DriverKit system extension:
  1. lightweight pre-refresh hygiene snapshot
  2. quit ASFWLocal.app
  3. collect ASFW DriverKit user-server PIDs, then terminate those PIDs via one
     admin prompt
  4. restart coreaudiod via the same admin prompt
  5. wait for IORegistry to report the expected CDHash, if provided
  6. lightweight post-refresh hygiene snapshot

If --expected-cdhash is omitted, the script prints the active IORegistry CDHash
without enforcing it.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --expected-cdhash)
      EXPECTED_CDHASH="$2"
      shift 2
      ;;
    --snapshot-dir)
      SNAP_BASE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

active_cdhash() {
  ioreg -p IOService -l -w0 -r -c ASFWDriver 2>/dev/null |
    awk -F'"' '/IOUserServerCDHash/ {print $4; exit}'
}

driver_pids() {
  pgrep -f "/Library/SystemExtensions/.*/${DRIVER_ID}\\.dext/" 2>/dev/null || true
}

snapshot() {
  local name="$1"
  "$ROOT_DIR/tools/debug/hygiene_snapshot.sh" --out "$SNAP_BASE/$name" >/dev/null || true
}

mkdir -p "$SNAP_BASE"
snapshot "before"

echo "Refresh snapshots: $SNAP_BASE"
echo "Active CDHash before: $(active_cdhash || true)"

if [[ -d "$APP_PATH" ]]; then
  pkill -TERM -f "${APP_PATH}/Contents/MacOS/ASFW" 2>/dev/null || true
fi

# macOS still ships Bash 3.2, so avoid mapfile/readarray here.
PIDS=()
while IFS= read -r pid; do
  [[ -n "$pid" ]] && PIDS+=("$pid")
done < <(driver_pids)
KILL_CMD="/usr/bin/true"
if [[ ${#PIDS[@]} -gt 0 ]]; then
  KILL_CMD="/bin/kill -TERM ${PIDS[*]} || true"
fi

/usr/bin/osascript -e "do shell script \"${KILL_CMD}; /usr/bin/killall coreaudiod || true\" with administrator privileges"

deadline=$((SECONDS + TIMEOUT_SECONDS))
observed=""
while (( SECONDS < deadline )); do
  observed="$(active_cdhash || true)"
  if [[ -z "$EXPECTED_CDHASH" || "$observed" == "$EXPECTED_CDHASH" ]]; then
    break
  fi
  sleep 1
done

snapshot "after"

echo "Active CDHash after: ${observed:-$(active_cdhash || true)}"
if [[ -n "$EXPECTED_CDHASH" && "${observed:-}" != "$EXPECTED_CDHASH" ]]; then
  echo "Expected CDHash was not observed: $EXPECTED_CDHASH" >&2
  exit 1
fi
