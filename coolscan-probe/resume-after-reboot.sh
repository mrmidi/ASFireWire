#!/usr/bin/env bash
# Resume the CoolScan 9000 work after a reboot.
#
# Context (2026-06-22): SBP-2 session-scheduler deadlock is fixed (dext v17,
# upstream PR mrmidi/ASFireWire#33). OPEN EXPERIMENT: SEND LUT verdict — last
# wall was SEND LUT rejected 5/24 (INVALID FIELD IN CDB); probe fix committed
# (90e72ca): waitReady + scanner_ready before each color LUT + xferDiag.
# BEFORE REBOOT: FW bus was down (0 devices) after a 30x teardown stress test
# left a duplicate IOUserServer behind. Reboot = clean reset.
set -u
cd "$(dirname "$0")"
PROBE=.build/debug/CoolScanProbe

echo "== 1. dext alive? (expect 1.0/17 activated enabled) =="
systemextensionsctl list 2>/dev/null | grep -i asfw || echo "  (dext not listed!)"

echo "== 2. IOUserServer instances (expect 1 after clean reboot) =="
ioreg -w0 2>/dev/null | grep -c "IOUserServer(net.mrmidi.ASFW.ASFWDriver"

echo
echo "== 3. POWER-CYCLE the scanner and wait until it has FULLY finished booting, press Enter =="
read -r _

[ -x "$PROBE" ] || { echo "MISSING $PROBE — build first: ./build.sh"; exit 1; }
echo "== 4. running: CoolScanProbe coolscan nofocus =="
"$PROBE" coolscan nofocus > /tmp/coolscan_run.log 2>&1 &
pid=$!
( sleep 150; kill -9 "$pid" 2>/dev/null ) & wd=$!
wait "$pid" 2>/dev/null
kill "$wd" 2>/dev/null; wait "$wd" 2>/dev/null
cat /tmp/coolscan_run.log

if grep -q "No FireWire devices found" /tmp/coolscan_run.log; then
  echo
  echo "== Bus down again after reboot → capturing Self-ID log to /tmp/selfid.log =="
  log show --last 3m \
    --predicate 'eventMessage CONTAINS "Self-ID" OR eventMessage CONTAINS "BusReset" OR eventMessage CONTAINS "topology"' \
    --info --debug --style compact > /tmp/selfid.log 2>&1
  echo "   done: /tmp/selfid.log ($(wc -l < /tmp/selfid.log) lines)"
fi

echo
echo "NEXT: share /tmp/coolscan_run.log (and /tmp/selfid.log if present) with Claude in the next session."
echo "  SEND LUT ✅      → continue SCAN→READ"
echo "  SEND LUT 5/24 + targetREAD=32768 → try LUT-before-WINDOW (coolscan9k ordering)"
echo "  targetREAD≠32768 → dext 32 KB struct-input path delivers wrong length"
