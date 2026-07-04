#!/usr/bin/env bash
# Post-hoc dump of SBP-2/HBA log for the last N minutes (default 5).
# log show loses nothing (reads persisted buffer) — unlike log stream
# which drops under load. Run via "!" prefix: ! coolscan-probe/show-last.sh 10
set -u
MIN="${1:-5}"
OUT=/tmp/sbp2_show.log
/usr/bin/log show --last "${MIN}m" --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[SCSIHBA]" OR eventMessage CONTAINS "SBP2" OR eventMessage CONTAINS "FetchAgent" OR eventMessage CONTAINS "AddressSpaceManager" OR eventMessage CONTAINS "LoginSession" OR eventMessage CONTAINS "CommandExecutor" OR eventMessage CONTAINS "AT-resp" OR eventMessage CONTAINS "P2_FALLBACK" OR eventMessage CONTAINS "P1_ARM while" OR eventMessage CONTAINS "ResponseSender" OR eventMessage CONTAINS "UNCLAIMED" OR eventMessage CONTAINS "AR readBlock" OR eventMessage CONTAINS "SessionRegistry" OR eventMessage CONTAINS "escalating to bus reset" OR eventMessage CONTAINS "Bus reset" OR eventMessage CONTAINS "software reset"' \
  > "$OUT" 2>&1
echo "wrote $(wc -l < "$OUT") lines to $OUT"
