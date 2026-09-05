#!/bin/bash
# Capture the driver-side Phase 3 telemetry while a stream is running.
#
# The agent's sandbox returns zero lines from `log stream` silently, so this
# has to be run by a human. It starts the capture, drives ~20 s of audio
# through the RTL tool (four [Ledger]/[TxFill] heartbeats at 5 s), then stops.
#
# No loopback cable is required: every counter here is driver-side. The RTL
# tool is only being used as a well-behaved duplex client.
set -u
cd "$(dirname "$0")/../../.."
OUT="documentation/reports/phase3-hw-validation-2026-09-05/evidence"
LOG="$OUT/driver-telemetry.txt"

echo "capturing to $LOG ..."
log stream --info --debug --predicate '
     eventMessage CONTAINS "[Ledger]"
  OR eventMessage CONTAINS "[TxFill]"
  OR eventMessage CONTAINS "[TxPrep]"
  OR eventMessage CONTAINS "[TxLead]"
  OR eventMessage CONTAINS "[BackendTiming]"
  OR eventMessage CONTAINS "[TxOwnership]"' > "$LOG" 2>&1 &
STREAM=$!
sleep 2

echo "running 60 RTL trials (~20 s of streaming audio) ..."
tools/rtl/rtl_loopback -d Duet --frames 64 --trials 60 --measure \
    > "$OUT/rtl-during-capture.txt" 2>&1

sleep 6          # let one more heartbeat land after IO stops
kill "$STREAM" 2>/dev/null
wait "$STREAM" 2>/dev/null

echo "done. $(grep -c . "$LOG") lines captured:"
grep -c "\[Ledger\]" "$LOG" | sed 's/^/  [Ledger] lines: /'
grep -c "\[TxFill\]" "$LOG" | sed 's/^/  [TxFill] lines: /'
