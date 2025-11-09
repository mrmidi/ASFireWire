#!/bin/bash
# NOTE: all arguments is obsolete - stack was heavily modified since initial creation
# still usefull for attaching to the driver process
# though could be useful some similar scripts
# i'm only using it with sudo, i have no idea if it works without it
set -euo pipefail

# ASFWDriver LLDB Debugging Helper (updated)
# Usage:
#   ./debug.sh                -> manual mode
#   ./debug.sh packet         -> packet parsing & trailer/tCode dumps
#   ./debug.sh buffer         -> AR buffer loop: bytes & quick hexdump
#   ./debug.sh dequeue        -> AR descriptor + buffer analysis with VA->PA mapping
#   ./debug.sh ue             -> UnrecoverableError / PostedWriteError snapshot
#   ./debug.sh irq            -> IRQ handler snapshots (intEvent/intMask/HC/Link)
#
# Notes:
# - Requires the LLDB command files in the same dir:
#     lldb_packet.txt, lldb_ar_buffer.txt, lldb_dequeue.txt, lldb_ue.txt, lldb_irq.txt
# - Uses sudo to attach (DriverKit processes typically need it).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLDB_BIN="${LLDB_BIN:-/usr/bin/lldb}"

mode="${1:-manual}"
DRIVER_PID=""

_on_exit() {
  echo -e "\n✋ Interrupted. Exiting."
  exit 130
}
trap _on_exit SIGINT

echo "⏳ Waiting for ASFWDriver process (press Ctrl+C to quit)..."
while [[ -z "$DRIVER_PID" ]]; do
  # -n: newest PID; process name equals bundle id
  DRIVER_PID="$(pgrep -n 'net\.mrmidi\.ASFW\.ASFWDriver' || true)"
  [[ -z "$DRIVER_PID" ]] && sleep 1
done

echo "📍 Attaching to ASFWDriver (PID: $DRIVER_PID)..."

case "$mode" in
  packet)
    echo "🔍 Packet parsing mode"
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -s "$SCRIPT_DIR/lldb_packet.txt"
    ;;
  buffer)
    echo "🔍 AR buffer inspection mode"
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -s "$SCRIPT_DIR/lldb_ar_buffer.txt"
    ;;
  dequeue)
    echo "🔬 AR dequeue deep inspection mode (descriptor analysis + VA->PA + binary dump)"
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -s "$SCRIPT_DIR/lldb_dequeue.txt"
    ;;
  ue)
    echo "🚨 UE/PostedWriteError snapshot mode"
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -s "$SCRIPT_DIR/lldb_ue.txt"
    ;;
  irq)
    echo "🔧 IRQ snapshot mode"
    sudo "$LLDB_BIN" -p "$DRIVER_PID" -s "$SCRIPT_DIR/lldb_irq.txt"
    ;;
  manual|*)
    echo "🎮 Manual mode. Handy commands:"
    echo "  br set -n 'ASFW::Async::AsyncSubsystem::OnRxInterrupt(ASFW::Async::AsyncSubsystem::ARContextType)'"
    echo "  br set -n 'ASFW::Async::BufferRing::Dequeue'"
    echo "  br set -p 'const auto& info = \\*bufferInfo' -f AsyncSubsystem.cpp"
    echo "  mem read -c64 -fx <addr>"
    echo "  expr -R -- ((((uint8_t*)<addr>)[0] >> 4) & 0xF)"
    echo ""
    echo "Quick modes available: packet | buffer | dequeue | ue | irq"
    echo ""
    sudo "$LLDB_BIN" -p "$DRIVER_PID"
    ;;
esac

