#!/usr/bin/env bash
# Fanger SBP-2/HBA-trace mens VueScan kjører. Start før Preview, stopp med Ctrl-C
# eller `pkill -f "log stream"` etterpå. Kjøres via «!»-prefiks.
set -u
OUT=/tmp/vuescan_trace.log
exec log stream --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[SCSIHBA]" OR eventMessage CONTAINS "SBP2" OR eventMessage CONTAINS "FetchAgent" OR eventMessage CONTAINS "AddressSpaceManager" OR eventMessage CONTAINS "LoginSession"' \
  > "$OUT" 2>&1
