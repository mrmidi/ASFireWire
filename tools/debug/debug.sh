#!/usr/bin/env bash
set -euo pipefail

DRIVER_NAME="net.mrmidi.ASFW.ASFWDriver"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mode="manual"
kill_all=false
use_mcp=true
wait_for_driver=true
driver_pid=""
wait_interval="${ASFW_DEBUG_WAIT_INTERVAL:-0.2}"
wait_timeout="${ASFW_DEBUG_WAIT_TIMEOUT:-0}"

usage() {
  cat <<'EOF'
Usage: tools/debug/debug.sh [mode] [options]

Modes:
  tx        Stop on the second complete TX source/destination matrix snapshot
  logs      Stream TX matrix and verifier logs without attaching LLDB
  packet    RX packet parsing
  buffer    AR buffer inspection
  dequeue   AR dequeue inspection
  ue        UE/PostedWriteError snapshot
  irq       IRQ snapshot
  it        IT descriptor inspection
  manual    Attach LLDB without scripted breakpoints (default)

Options:
  -k, --killall        Kill all ASFWDriver processes and exit
  -p, --pid PID        Attach to a specific PID
      --no-wait        Fail if the driver is not already running
      --interval SEC   Process polling interval (default: 0.2)
      --timeout SEC    Stop waiting after SEC; 0 waits indefinitely
      --no-mcp         Do not start the LLDB MCP protocol server
  -h, --help           Show this help

There is no fixed startup sleep. The polling sleep is used only while the
DriverKit process does not exist, preventing a busy loop.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    tx|logs|packet|buffer|dequeue|ue|irq|it|manual)
      mode="$1"
      shift
      ;;
    --killall|-k)
      kill_all=true
      shift
      ;;
    --pid|-p)
      [[ $# -ge 2 ]] || { echo "Missing PID after $1" >&2; exit 2; }
      driver_pid="$2"
      shift 2
      ;;
    --no-wait)
      wait_for_driver=false
      shift
      ;;
    --interval)
      [[ $# -ge 2 ]] || { echo "Missing seconds after --interval" >&2; exit 2; }
      wait_interval="$2"
      shift 2
      ;;
    --timeout)
      [[ $# -ge 2 ]] || { echo "Missing seconds after --timeout" >&2; exit 2; }
      wait_timeout="$2"
      shift 2
      ;;
    --no-mcp)
      use_mcp=false
      shift
      ;;
    --help|-h)
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

find_driver_pid() {
  pgrep -nx "$DRIVER_NAME" 2>/dev/null ||
    pgrep -n -f "$DRIVER_NAME" 2>/dev/null ||
    true
}

find_all_driver_pids() {
  pgrep -x "$DRIVER_NAME" 2>/dev/null ||
    pgrep -f "$DRIVER_NAME" 2>/dev/null ||
    true
}

if [[ "$kill_all" == true ]]; then
  pids_to_kill="$(find_all_driver_pids)"
  if [[ -z "$pids_to_kill" ]]; then
    echo "No ASFWDriver processes found."
    exit 0
  fi

  echo "Killing ASFWDriver processes: $pids_to_kill"
  for pid in $pids_to_kill; do
    sudo kill -9 "$pid" || true
  done
  exit 0
fi

if [[ "$mode" == "logs" ]]; then
  echo "Streaming TX source/ring/DMA matrix logs. Press Ctrl+C to stop."
  exec sudo /usr/bin/log stream \
    --style compact \
    --level debug \
    --predicate \
    'eventMessage CONTAINS "TX FRAME MATRIX" OR eventMessage CONTAINS "TX FRAME SRC_RING" OR eventMessage CONTAINS "IT TX DMA MATRIX" OR eventMessage CONTAINS "IT TX DMA FRAME" OR eventMessage CONTAINS "TX PAYLOAD UNCOVERED" OR eventMessage CONTAINS "ADK FATAL TX PREP"'
fi

LLDB_BIN=""
if command -v brew >/dev/null 2>&1; then
  homebrew_lldb="$(brew --prefix llvm)/bin/lldb"
  if [[ -x "$homebrew_lldb" ]]; then
    LLDB_BIN="$homebrew_lldb"
  fi
fi
LLDB_BIN="${LLDB_BIN:-/usr/bin/lldb}"
echo "Using LLDB: $LLDB_BIN"

if [[ -z "$driver_pid" ]]; then
  driver_pid="$(find_driver_pid)"
fi

if [[ -z "$driver_pid" && "$wait_for_driver" == false ]]; then
  echo "ASFWDriver is not running." >&2
  exit 1
fi

if [[ -z "$driver_pid" ]]; then
  echo "Waiting for ASFWDriver (poll interval ${wait_interval}s; Ctrl+C to stop)..."
  started_at="$(date +%s)"
  while [[ -z "$driver_pid" ]]; do
    sleep "$wait_interval"
    driver_pid="$(find_driver_pid)"
    if [[ "$wait_timeout" != "0" ]]; then
      now="$(date +%s)"
      if (( now - started_at >= wait_timeout )); then
        echo "Timed out waiting for ASFWDriver after ${wait_timeout}s." >&2
        exit 1
      fi
    fi
  done
fi

if ! ps -p "$driver_pid" >/dev/null 2>&1; then
  echo "PID $driver_pid is not running." >&2
  exit 1
fi

MCP_PORT="${MCP_PORT:-59999}"
MCP_LISTEN_URI="listen://localhost:${MCP_PORT}"
MCP_START_CMD="protocol-server start MCP ${MCP_LISTEN_URI}"

lldb_args=(-p "$driver_pid")
if [[ "$use_mcp" == true ]]; then
  lldb_args+=(-o "$MCP_START_CMD")
fi

case "$mode" in
  tx)
    echo "TX matrix mode: first one-second snapshot is skipped; stopping on the second."
    echo "Requires ASFWEnableIsochTxVerifier=true in the running dext."
    lldb_args+=(-s "$SCRIPT_DIR/lldb_tx_frame_matrix.txt")
    ;;
  packet)
    lldb_args+=(-s "$SCRIPT_DIR/lldb_packet.txt")
    ;;
  buffer)
    lldb_args+=(-s "$SCRIPT_DIR/lldb_ar_buffer.txt")
    ;;
  dequeue)
    lldb_args+=(-s "$SCRIPT_DIR/lldb_dequeue.txt")
    ;;
  ue)
    lldb_args+=(-s "$SCRIPT_DIR/lldb_ue.txt")
    ;;
  irq)
    lldb_args+=(-s "$SCRIPT_DIR/lldb_irq.txt")
    ;;
  it)
    lldb_args+=(-s "$SCRIPT_DIR/lldb_it_descriptors.txt")
    ;;
  manual)
    echo "Manual LLDB mode. TX hook: br set -n ASFWDebugTxFrameMatrixBreakpoint"
    ;;
esac

echo "Attaching to ASFWDriver PID $driver_pid"
if [[ "$use_mcp" == true ]]; then
  echo "LLDB MCP: $MCP_LISTEN_URI"
fi
exec sudo "$LLDB_BIN" "${lldb_args[@]}"
