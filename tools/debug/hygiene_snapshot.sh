#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${ASFW_HYGIENE_DIR:-/tmp/asfw-hygiene-$(date +%Y%m%d-%H%M%S)}"
DRIVER_CLASS="${ASFW_DRIVER_CLASS:-ASFWDriver}"
AUDIO_NUB_CLASS="${ASFW_AUDIO_NUB_CLASS:-ASFWAudioNub}"

usage() {
  cat <<USAGE
Usage: $0 [--out DIR]

Captures a lightweight machine/driver hygiene snapshot before or after ASFW
driver refreshes. This intentionally avoids broad log collection so it is cheap
enough to run between live audio test rounds.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)
      OUT_DIR="$2"
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

mkdir -p "$OUT_DIR"

capture() {
  local name="$1"
  shift
  {
    printf 'Command:'
    printf ' %q' "$@"
    printf '\n\n'
    "$@"
  } >"$OUT_DIR/$name" 2>&1 || true
}

capture_shell() {
  local name="$1"
  shift
  {
    printf 'Command: %s\n\n' "$*"
    "$@"
  } >"$OUT_DIR/$name" 2>&1 || true
}

{
  echo "ASFW hygiene snapshot"
  echo "Created: $(date)"
  echo "Root: $ROOT_DIR"
  echo "Branch: $(git -C "$ROOT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
  echo "Commit: $(git -C "$ROOT_DIR" rev-parse --short=12 HEAD 2>/dev/null || true)"
  echo
  echo "Key files:"
  echo "- top_processes.txt"
  echo "- zombies.txt"
  echo "- asfw_related_processes.txt"
  echo "- systemextensions.txt"
  echo "- ioreg_asfw_driver.txt"
  echo "- ioreg_asfw_audio_nub.txt"
} >"$OUT_DIR/README.txt"

capture "top_processes.txt" ps -axo pid,ppid,stat,%cpu,%mem,etime,comm,args
sort -k4 -nr "$OUT_DIR/top_processes.txt" -o "$OUT_DIR/top_processes.txt" || true
capture_shell "zombies.txt" awk '$3 ~ /Z/ {print}' "$OUT_DIR/top_processes.txt"
capture_shell "asfw_related_processes.txt" sh -c \
  "pgrep -fl 'ASFW|ASFWDriver|coreaudiod|ctest|cmake|xcodebuild|log stream|coreaudio_channel_capture' || true"
capture "systemextensions.txt" systemextensionsctl list
capture "ioreg_asfw_driver.txt" ioreg -p IOService -l -w0 -r -c "$DRIVER_CLASS"
capture "ioreg_asfw_audio_nub.txt" ioreg -p IOService -l -w0 -r -c "$AUDIO_NUB_CLASS"
capture "cpu_count.txt" sysctl -n hw.ncpu

{
  echo "Snapshot: $OUT_DIR"
  echo
  echo "== Active ASFW extension =="
  grep -E 'ASFW|com\.chrisizatt' "$OUT_DIR/systemextensions.txt" || true
  echo
  echo "== ASFW IORegistry =="
  grep -E 'IOUserServerCDHash|ASFWInputChannelCount|ASFWOutputChannelCount|ASFWCurrentSampleRate|ASFWStreamMode|registered|active' \
    "$OUT_DIR/ioreg_asfw_driver.txt" | head -80 || true
  echo
  echo "== Top CPU =="
  head -20 "$OUT_DIR/top_processes.txt"
  echo
  echo "== Zombies =="
  cat "$OUT_DIR/zombies.txt"
} >"$OUT_DIR/summary.txt"

cat "$OUT_DIR/summary.txt"
