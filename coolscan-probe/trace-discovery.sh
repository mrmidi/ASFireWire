#!/usr/bin/env bash
# Bred trace: discovery + SBP-2 + HBA. Brukes når scanneren ikke dukker opp:
# start, replug/power-cycle scanner, se om «Device Discovered»/unit-publish og
# [SBP2Bridge] session/login kommer. Kjøres via «!»-prefiks.
set -u
OUT=/tmp/discovery_trace.log
exec log stream --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[SCSIHBA]" OR eventMessage CONTAINS "SBP2" OR eventMessage CONTAINS "Discovered" OR eventMessage CONTAINS "SelfID" OR eventMessage CONTAINS "unit" OR eventMessage CONTAINS "Unit" OR eventMessage CONTAINS "ROMScanner" OR eventMessage CONTAINS "login" OR eventMessage CONTAINS "Login"' \
  > "$OUT" 2>&1
