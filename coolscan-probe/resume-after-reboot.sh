#!/usr/bin/env bash
# Resume CoolScan 9000-arbeidet etter en reboot.
#
# Kontekst (2026-06-22): SBP-2 session-scheduler-deadlocken er fikset (dext v17,
# upstream PR mrmidi/ASFireWire#33). ÅPENT EKSPERIMENT: SEND LUT-verdikt — siste
# vegg var SEND LUT avvist 5/24 (INVALID FIELD IN CDB); probe-fiks committet
# (90e72ca): waitReady + scanner_ready før hver farge-LUT + xferDiag.
# FØR REBOOT: FW-bussen var nede (0 enheter) etter at en 30x teardown-stresstest
# etterlot en duplikat IOUserServer. Reboot = ren nullstilling.
set -u
cd "$(dirname "$0")"
PROBE=.build/debug/CoolScanProbe

echo "== 1. dext i live? (forvent 1.0/17 activated enabled) =="
systemextensionsctl list 2>/dev/null | grep -i asfw || echo "  (dext ikke listet!)"

echo "== 2. IOUserServer-instanser (forvent 1 etter ren reboot) =="
ioreg -w0 2>/dev/null | grep -c "IOUserServer(net.mrmidi.ASFW.ASFWDriver"

echo
echo "== 3. STRØMSYKLE skanneren og vent til den er HELT ferdig bootet, trykk Enter =="
read -r _

[ -x "$PROBE" ] || { echo "FANT IKKE $PROBE — bygg først: ./build.sh"; exit 1; }
echo "== 4. kjører: CoolScanProbe coolscan nofocus =="
"$PROBE" coolscan nofocus > /tmp/coolscan_run.log 2>&1 &
pid=$!
( sleep 150; kill -9 "$pid" 2>/dev/null ) & wd=$!
wait "$pid" 2>/dev/null
kill "$wd" 2>/dev/null; wait "$wd" 2>/dev/null
cat /tmp/coolscan_run.log

if grep -q "Ingen FireWire-enheter funnet" /tmp/coolscan_run.log; then
  echo
  echo "== Buss nede igjen etter reboot → fanger Self-ID-logg til /tmp/selfid.log =="
  log show --last 3m \
    --predicate 'eventMessage CONTAINS "Self-ID" OR eventMessage CONTAINS "BusReset" OR eventMessage CONTAINS "topology"' \
    --info --debug --style compact > /tmp/selfid.log 2>&1
  echo "   ferdig: /tmp/selfid.log ($(wc -l < /tmp/selfid.log) linjer)"
fi

echo
echo "NESTE: del /tmp/coolscan_run.log (og evt. /tmp/selfid.log) med Claude i neste økt."
echo "  SEND LUT ✅      → fortsett SCAN→READ"
echo "  SEND LUT 5/24 + targetREAD=32768 → prøv LUT-før-WINDOW (coolscan9k-rekkefølge)"
echo "  targetREAD≠32768 → dext 32 KB struct-input-vei leverer feil lengde"
