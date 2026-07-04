#!/usr/bin/env bash
# Broad trace: discovery + SBP-2 + HBA. Use when the scanner does not show up:
# start, replug/power-cycle scanner, check for "Device Discovered"/unit-publish and
# [SBP2Bridge] session/login. Run via "!" prefix.
set -u
OUT=/tmp/discovery_trace.log
exec log stream --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[SCSIHBA]" OR eventMessage CONTAINS "SBP2" OR eventMessage CONTAINS "Discovered" OR eventMessage CONTAINS "SelfID" OR eventMessage CONTAINS "unit" OR eventMessage CONTAINS "Unit" OR eventMessage CONTAINS "ROMScanner" OR eventMessage CONTAINS "login" OR eventMessage CONTAINS "Login"' \
  > "$OUT" 2>&1
