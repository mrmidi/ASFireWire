#!/usr/bin/env bash
# Captures SBP-2/HBA trace while VueScan runs. Start before Preview, stop with Ctrl-C
# or `pkill -f "log stream"` afterwards. Run via "!" prefix.
set -u
OUT=/tmp/vuescan_trace.log
exec log stream --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[SCSIHBA]" OR eventMessage CONTAINS "SBP2" OR eventMessage CONTAINS "FetchAgent" OR eventMessage CONTAINS "AddressSpaceManager" OR eventMessage CONTAINS "LoginSession" OR eventMessage CONTAINS "[ARReq]" OR eventMessage CONTAINS "[ARRsp]"' \
  > "$OUT" 2>&1
