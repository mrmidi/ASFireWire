#!/usr/bin/env bash
# capture_baseline.sh -- record a reproducible measurement baseline on macOS.
#
# Runs on a Mac with the ASFW driver installed, a supported device attached and
# an output->input loopback cable (see documentation/MEASUREMENT_BASELINE.md).
# Nothing here changes the driver; it only observes.
#
#   tools/baseline/capture_baseline.sh -d "Duet" -n 8 -t 20 -f 128
#
# Produces documentation/baselines/<UTC>-<device>-<sha>/ containing:
#   provenance.json          commit, driver version, OS, machine, argv, parameters
#   hal_snapshot.json        hal_geometry declarations (passive)
#   rtl_declared.txt         rtl_loopback declarations (passive)
#   sessions/session-NN.json one rtl_loopback --measure run per session
#   sessions/session-NN.txt  its human output
#   hal_clock.json           hal_geometry --clock (starts IO)
#   driver_log.txt           declared-geometry and heartbeat lines for the run
#   summary.json / .md       cross-session distribution (summarize_baseline.py)
#
# Each session is a separate rtl_loopback process, i.e. a fresh IOProc start:
# the round trip has been seen to differ between stream starts by whole
# 288-frame laps, so N sessions are required, not N trials.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEVICE="ASFW"
SESSIONS=8
TRIALS=20
FRAMES=""
CLOCK_SECONDS=30
PAUSE_SECONDS=3
OUT=""

usage() {
    cat <<EOF
usage: $0 [-d device-substring] [-n sessions] [-t trials] [-f buffer-frames]
          [-c clock-seconds] [-p pause-seconds] [-o output-dir]
  -d  device name substring (default: ASFW)
  -n  measurement sessions, each a fresh stream start (default: 8)
  -t  trials per session (default: 20)
  -f  client buffer size to request (default: leave as is)
  -c  seconds of HAL clock watch (default: 30; 0 disables)
  -p  pause between sessions in seconds (default: 3)
  -o  output directory (default: documentation/baselines/<UTC>-<device>-<sha>)
EOF
}

while getopts "d:n:t:f:c:p:o:h" opt; do
    case "$opt" in
        d) DEVICE="$OPTARG" ;;
        n) SESSIONS="$OPTARG" ;;
        t) TRIALS="$OPTARG" ;;
        f) FRAMES="$OPTARG" ;;
        c) CLOCK_SECONDS="$OPTARG" ;;
        p) PAUSE_SECONDS="$OPTARG" ;;
        o) OUT="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "capture_baseline.sh runs on macOS only" >&2
    exit 2
fi

RTL="$ROOT/build/tools/rtl/rtl_loopback"
HAL="$ROOT/build/tools/halprobe/hal_geometry"
if [[ ! -x "$RTL" || ! -x "$HAL" ]]; then
    echo "building measurement tools..."
    "$ROOT/tools/rtl/build.sh" >/dev/null
fi

echo "self-test..."
"$RTL" --selftest >/dev/null || { echo "rtl_loopback self-test FAILED -- not measuring" >&2; exit 1; }

SHA="$(git -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
DIRTY=""
if [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null)" ]]; then DIRTY="-dirty"; fi
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
SAFE_DEVICE="$(echo "$DEVICE" | tr -c 'A-Za-z0-9._-' '_' | sed 's/_*$//')"
OUT="${OUT:-$ROOT/documentation/baselines/${STAMP}-${SAFE_DEVICE}-${SHA}${DIRTY}}"
mkdir -p "$OUT/sessions"
START_EPOCH="$(date +%s)"

# ---------------------------------------------------------------- provenance
DRIVER_VERSION="unavailable"
for v in "$ROOT/tools/asfw-version/asfw-version" "$(command -v asfw-version 2>/dev/null || true)"; do
    if [[ -n "$v" && -x "$v" ]]; then
        DRIVER_VERSION="$("$v" 2>&1 | tr '\n' ' ' | sed 's/"/\\"/g')"
        break
    fi
done
FRAMES_JSON="null"
if [[ -n "$FRAMES" ]]; then FRAMES_JSON="$FRAMES"; fi
cat > "$OUT/provenance.json" <<EOF
{
  "schema": "asfw.baseline_provenance.v1",
  "timestamp_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "repo_commit": "${SHA}${DIRTY}",
  "driver_version": "${DRIVER_VERSION}",
  "os_version": "$(sw_vers -productVersion 2>/dev/null)",
  "os_build": "$(sw_vers -buildVersion 2>/dev/null)",
  "machine": "$(sysctl -n hw.model 2>/dev/null)",
  "device_filter": "${DEVICE}",
  "sessions": ${SESSIONS},
  "trials_per_session": ${TRIALS},
  "buffer_frames_requested": ${FRAMES_JSON},
  "clock_seconds": ${CLOCK_SECONDS},
  "command": "$(printf '%q ' "$0" "$@" | sed 's/"/\\"/g')"
}
EOF

# ------------------------------------------------------------ declarations
echo "declarations (passive)..."
"$HAL" -d "$DEVICE" --json "$OUT/hal_snapshot.json" > "$OUT/hal_snapshot.txt"
"$RTL" -d "$DEVICE" > "$OUT/rtl_declared.txt"

# ---------------------------------------------------------------- sessions
FRAME_ARGS=()
if [[ -n "$FRAMES" ]]; then FRAME_ARGS=(--frames "$FRAMES"); fi
for ((i = 0; i < SESSIONS; i++)); do
    name="$(printf 'session-%02d' "$i")"
    echo "session $((i + 1))/$SESSIONS..."
    set +e
    "$RTL" -d "$DEVICE" --measure --window auto --trials "$TRIALS" ${FRAME_ARGS[@]+"${FRAME_ARGS[@]}"} \
        --json "$OUT/sessions/$name.json" > "$OUT/sessions/$name.txt" 2>&1
    rc=$?
    set -e
    echo "exit $rc" >> "$OUT/sessions/$name.txt"
    if [[ ! -f "$OUT/sessions/$name.json" ]]; then
        echo "  session $name produced no evidence (exit $rc) -- see $name.txt" >&2
    fi
    sleep "$PAUSE_SECONDS"
done

# ------------------------------------------------------------------ clock
if [[ "$CLOCK_SECONDS" -gt 0 ]]; then
    echo "HAL clock watch (${CLOCK_SECONDS}s)..."
    "$HAL" -d "$DEVICE" --clock "$CLOCK_SECONDS" --json "$OUT/hal_clock.json" > "$OUT/hal_clock.txt"
fi

# -------------------------------------------------------------- driver log
# Declared geometry as the driver applied it, plus the heartbeat lines. Call
# /usr/bin/log by absolute path (zsh has a `log` builtin) and include
# info/debug. The dext logs as process "kernel", so filter on message text.
ELAPSED=$(( $(date +%s) - START_EPOCH + 60 ))
/usr/bin/log show --last "${ELAPSED}s" --info --debug --style compact --predicate \
    'eventMessage CONTAINS "HAL buffer profile" OR eventMessage CONTAINS "Reported HAL latency" OR eventMessage CONTAINS "GetZeroTimestampPeriod" OR eventMessage CONTAINS "txTransferDelay" OR eventMessage CONTAINS "[TxPrep]" OR eventMessage CONTAINS "[Zts]" OR eventMessage CONTAINS "[TxWire]" OR eventMessage CONTAINS "[TxExposure]" OR eventMessage CONTAINS "[TxProducerFatal]"' \
    > "$OUT/driver_log.txt" 2>&1 || true

# ---------------------------------------------------------------- summary
python3 "$ROOT/tools/baseline/summarize_baseline.py" "$OUT"
echo
echo "baseline written to $OUT"
