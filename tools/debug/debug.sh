#!/bin/bash
set -euo pipefail

# use lldb from homebrew if available
LLDB_BIN=""
if command -v brew &> /dev/null; then
  HOMEBREW_LLVM_LLDB="$(brew --prefix llvm)/bin/lldb"
  if [[ -x "$HOMEBREW_LLVM_LLDB" ]]; then
    LLDB_BIN="$HOMEBREW_LLVM_LLDB"
  fi
fi

LLDB_BIN="${LLDB_BIN:-/usr/bin/lldb}"
echo "🛠️  Using LLDB binary: $LLDB_BIN"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mode="manual"
kill_all=false
force_kill=false
DRIVER_ID="${ASFW_DRIVER_ID:-com.chrisizatt.ASFWLocal.ASFWDriver}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --killall|-k)
      kill_all=true
      shift
      ;;
    --force)
      force_kill=true
      shift
      ;;
    packet|buffer|dequeue|ue|irq|it|manual)
      mode="$1"
      shift
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

driver_pids() {
  local pid args
  pgrep -f '/Library/SystemExtensions/' 2>/dev/null | while read -r pid; do
    [[ -n "$pid" ]] || continue
    args="$(ps -p "$pid" -o args= 2>/dev/null || true)"
    if [[ "$args" == *"/Library/SystemExtensions/"* && "$args" == *"/${DRIVER_ID}.dext/"* ]]; then
      echo "$pid"
    fi
  done
}

if [[ "$kill_all" == true ]]; then
  pids_to_kill="$(driver_pids || true)"
  if [[ -z "$pids_to_kill" ]]; then
    echo "✅ No ASFWDriver processes found to kill."
    exit 0
  fi

  echo "🧹 Killing ASFWDriver processes: $pids_to_kill"
  for pid in $pids_to_kill; do
    if [[ "$force_kill" == true ]]; then
      sudo kill -9 "$pid" || true
    else
      sudo kill -TERM "$pid" || true
    fi
  done

  # After cleanup, exit without starting LLDB
  exit 0
fi
DRIVER_PID=""

# MCP settings
MCP_PORT="${MCP_PORT:-59999}"
MCP_LISTEN_URI="listen://localhost:${MCP_PORT}"
MCP_START_CMD="protocol-server start MCP ${MCP_LISTEN_URI}"

_on_exit() {
  echo -e "\n✋ Interrupted. Exiting."
  exit 130
}
trap _on_exit SIGINT

echo "⏳ Waiting for ASFWDriver process (press Ctrl+C to quit)..."
while [[ -z "$DRIVER_PID" ]]; do
  DRIVER_PID="$(driver_pids | tail -1 || true)"
  [[ -z "$DRIVER_PID" ]] && sleep 1
done

echo "📍 Attaching to ASFWDriver (PID: $DRIVER_PID)..."
echo "🧩 MCP will listen on: ${MCP_LISTEN_URI}"

case "$mode" in
  packet)
    echo "🔍 Packet parsing mode"
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -o "$MCP_START_CMD" -s "$SCRIPT_DIR/lldb_packet.txt"
    ;;
  buffer)
    echo "🔍 AR buffer inspection mode"
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -o "$MCP_START_CMD" -s "$SCRIPT_DIR/lldb_ar_buffer.txt"
    ;;
  dequeue)
    echo "🔬 AR dequeue deep inspection mode (descriptor analysis + VA->PA + binary dump)"
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -o "$MCP_START_CMD" -s "$SCRIPT_DIR/lldb_dequeue.txt"
    ;;
  ue)
    echo "🚨 UE/PostedWriteError snapshot mode"
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -o "$MCP_START_CMD" -s "$SCRIPT_DIR/lldb_ue.txt"
    ;;
  irq)
    echo "🔧 IRQ snapshot mode"
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -o "$MCP_START_CMD" -s "$SCRIPT_DIR/lldb_irq.txt"
    ;;
  it)
    echo "🎯 IT descriptor inspection mode (verify OMI bug)"
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -o "$MCP_START_CMD" -s "$SCRIPT_DIR/lldb_it_descriptors.txt"
    ;;
  manual|*)
    echo "🎮 Manual mode. Handy commands:"
    echo "  br set -n 'ASFW::Async::AsyncSubsystem::OnRxInterrupt(ASFW::Async::AsyncSubsystem::ARContextType)'"
    echo "  br set -n 'ASFW::Async::BufferRing::Dequeue'"
    echo "  br set -p 'const auto& info = \\*bufferInfo' -f AsyncSubsystem.cpp"
    echo "  mem read -c64 -fx <addr>"
    echo "  expr -R -- ((((uint8_t*)<addr>)[0] >> 4) & 0xF)"
    echo ""
    echo "Quick modes available: packet | buffer | dequeue | ue | irq | it"
    echo ""
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -o "$MCP_START_CMD"
    ;;
esac
