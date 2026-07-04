#!/usr/bin/env bash
# Captures SBP-2 transport trace from the running dext while the probe does
# INQUIRY (cmd1, data-IN) + TEST UNIT READY (cmd2). Answers two questions:
#   1) INQUIRY-null: did a "remote write ... len=36/96" hit the buffer address,
#      or a "write miss"? (= did data-IN land in the buffer we read?)
#   2) TUR timeout: did FetchAgent submit cmd2, and did a status block arrive?
# Run via the "!" prefix so log stream actually produces lines.
set -u
cd "$(dirname "$0")"
PROBE=.build/debug/CoolScanProbe
OUT=/tmp/sbp2_trace.log
[ -x "$PROBE" ] || { echo "MISSING $PROBE — build probe first"; exit 1; }

log stream --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "AddressSpaceManager" OR eventMessage CONTAINS "FetchAgent" OR eventMessage CONTAINS "SBP2" OR eventMessage CONTAINS "CommandExecutor"' \
  > "$OUT" 2>&1 &
lspid=$!
sleep 2

echo "== running probe (coolscan nofocus) =="
"$PROBE" coolscan nofocus
sleep 1
kill "$lspid" 2>/dev/null; wait "$lspid" 2>/dev/null

echo
echo "== trace written to $OUT ($(wc -l < "$OUT") lines) =="
echo "   relevant lines (instrumented):"
grep -E "\[SBP2(cmd|fa|asm)\]|allocated .*ORB|write miss|UNMATCHED" "$OUT" | tail -80
