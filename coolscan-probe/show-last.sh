#!/usr/bin/env bash
# Post-hoc dump av SBP-2/HBA-logg for de siste N minutter (default 5).
# log show taper ingenting (leser persistert buffer) — motsatt av log stream
# som dropper under last. Kjøres via «!»-prefiks: ! coolscan-probe/show-last.sh 10
set -u
MIN="${1:-5}"
OUT=/tmp/sbp2_show.log
/usr/bin/log show --last "${MIN}m" --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[SCSIHBA]" OR eventMessage CONTAINS "SBP2" OR eventMessage CONTAINS "FetchAgent" OR eventMessage CONTAINS "AddressSpaceManager" OR eventMessage CONTAINS "LoginSession" OR eventMessage CONTAINS "CommandExecutor" OR eventMessage CONTAINS "AT-resp"' \
  > "$OUT" 2>&1
echo "skrev $(wc -l < "$OUT") linjer til $OUT"
