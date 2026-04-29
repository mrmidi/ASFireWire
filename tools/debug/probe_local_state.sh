#!/usr/bin/env bash
set -euo pipefail

DRIVER_ID="${ASFW_DRIVER_ID:-com.chrisizatt.ASFWLocal.ASFWDriver}"
APP_PATH="${ASFW_LOCAL_APP:-/Applications/ASFWLocal.app}"
LOG_WINDOW="${ASFW_LOG_WINDOW:-15m}"

section() {
  printf '\n== %s ==\n' "$1"
}

filter() {
  local pattern="$1"
  if command -v rg >/dev/null 2>&1; then
    rg -i -C 5 "$pattern" || true
  else
    grep -E -i -B 5 -A 5 "$pattern" || true
  fi
}

print_bundle_info() {
  local bundle="$1"
  if [[ ! -d "$bundle" ]]; then
    return
  fi

  echo "$bundle"
  local plist="$bundle/Contents/Info.plist"
  if [[ ! -f "$plist" && -f "$bundle/Info.plist" ]]; then
    plist="$bundle/Info.plist"
  fi

  /usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$plist" 2>/dev/null | sed 's/^/  CFBundleIdentifier: /' || true
  /usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist" 2>/dev/null | sed 's/^/  CFBundleShortVersionString: /' || true
  /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$plist" 2>/dev/null | sed 's/^/  CFBundleVersion: /' || true
  /usr/bin/codesign -dv --verbose=4 "$bundle" 2>&1 | filter 'Identifier=|TeamIdentifier=|CDHash=|Signed Time=' | sed 's/^/  /'
}

section "System Extension State"
systemextensionsctl list

section "ASFW Processes"
pgrep -fl "ASFW|ASFWDriver|${DRIVER_ID}" || true

section "Installed DriverKit Bundles"
find /Library/SystemExtensions -maxdepth 2 -type d -name "${DRIVER_ID}.dext" -print 2>/dev/null | while IFS= read -r dext; do
  print_bundle_info "$dext"
done

section "Staged App Bundle"
print_bundle_info "$APP_PATH"
if [[ -d "$APP_PATH/Contents/Library/SystemExtensions/${DRIVER_ID}.dext" ]]; then
  print_bundle_info "$APP_PATH/Contents/Library/SystemExtensions/${DRIVER_ID}.dext"
fi

section "Thunderbolt / FireWire / PCI Topology"
system_profiler SPThunderboltDataType SPFireWireDataType SPPCIDataType -detailLevel full \
  | filter 'Thunderbolt to FireWire Adapter|pci11c1,5901|IEEE 1394|Driver Installed|Tunnel Compatible|Link Status|Link up|Slot:'

section "ASFW IORegistry Tree"
for service in pci11c1,5901 ASFWDriver ASFWAudioNub ASFWAudioDriver; do
  ioreg -p IOService -l -w0 -r -n "$service" 2>/dev/null | filter 'pci11c1,5901|ASFWDriver|ASFWAudioNub|ASFWAudioDriver|ASFWDeviceName|ASFWGUID|ASFWInputChannelCount|ASFWOutputChannelCount|IOUserAudioDriverUserClient|IOUserServerCDHash|IOPCIMatch'
done

section "Core Audio Visibility"
system_profiler SPAudioDataType -detailLevel full \
  | filter 'Alesis MultiMix Firewire|ASFireWire|FWA Firewire Audio|Transport: FireWire'

section "Focused Recent Logs"
/usr/bin/log show --last "$LOG_WINDOW" --style compact --predicate \
  'eventMessage CONTAINS[c] "ASFWDriver::Start" OR eventMessage CONTAINS[c] "start(pci11c1,5901)" OR eventMessage CONTAINS[c] "Device upsert" OR eventMessage CONTAINS[c] "Known device profile" OR eventMessage CONTAINS[c] "DICETcatProtocol" OR eventMessage CONTAINS[c] "DiceAudioBackend" OR eventMessage CONTAINS[c] "ASFWAudioNub ready" OR eventMessage CONTAINS[c] "ASFWAudioDriver: Started" OR eventMessage CONTAINS[c] "HALS_Device::Activate" OR eventMessage CONTAINS[c] "Alesis" OR eventMessage CONTAINS[c] "MultiMix" OR eventMessage CONTAINS[c] "dext was replaced"' \
  | filter 'ASFW|Alesis|MultiMix|DICE|AudioNub|HALS_Device::Activate|pci11c1,5901|dext was replaced'
