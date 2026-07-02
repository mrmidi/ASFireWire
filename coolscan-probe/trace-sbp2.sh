#!/usr/bin/env bash
# Fanger SBP-2 transport-trace fra den kjørende dext-en mens proben gjør
# INQUIRY (cmd1, data-IN) + TEST UNIT READY (cmd2). Svarer på to spørsmål:
#   1) INQUIRY-null: kom det en "remote write ... len=36/96" til buffer-adressen,
#      eller en "write miss"? (= landet data-IN i bufferet vi leser?)
#   2) TUR-timeout: submittet FetchAgent cmd2, og kom det en status-blokk?
# Kjøres via «!»-prefiks slik at log stream faktisk produserer linjer.
set -u
cd "$(dirname "$0")"
PROBE=.build/debug/CoolScanProbe
OUT=/tmp/sbp2_trace.log
[ -x "$PROBE" ] || { echo "FANT IKKE $PROBE — bygg probe først"; exit 1; }

log stream --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "AddressSpaceManager" OR eventMessage CONTAINS "FetchAgent" OR eventMessage CONTAINS "SBP2" OR eventMessage CONTAINS "CommandExecutor"' \
  > "$OUT" 2>&1 &
lspid=$!
sleep 2

echo "== kjører probe (coolscan nofocus) =="
"$PROBE" coolscan nofocus
sleep 1
kill "$lspid" 2>/dev/null; wait "$lspid" 2>/dev/null

echo
echo "== trace skrevet til $OUT ($(wc -l < "$OUT") linjer) =="
echo "   relevante linjer (instrumentert):"
grep -E "\[SBP2(cmd|fa|asm)\]|allocated .*ORB|write miss|UNMATCHED" "$OUT" | tail -80
