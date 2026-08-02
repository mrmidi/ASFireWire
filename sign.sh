#!/usr/bin/env bash
#
# ASFireWire – ad-hoc signing for local install & prebuilt distribution.
#
# build.sh intentionally builds UNSIGNED (CODE_SIGNING_ALLOWED=NO), which yields
# an app with an empty entitlement dump and a fully unsigned .dext. A DriverKit
# extension carrying restricted entitlements will NOT load unless those
# entitlements are actually embedded in its signature — so this script re-signs
# both, ad-hoc, with the correct per-target entitlement files.
#
# Verified on Apple Silicon / macOS 26 (Tahoe): an app+dext signed this way loads
# with `csrutil disable` + `systemextensionsctl developer on` alone — no
# amfi_get_out_of_my_way boot-arg required, because the entitlements are present
# in the signature (AMFI only *validates* entitlements, it does not *inject* them).
#
# Ad-hoc (`--sign -`) has no provisioning profile and no team, so it is not tied
# to any machine: the same signed .app installs on any Mac (with SIP off).
#
# Usage:
#   ./sign.sh [path/to/ASFW.app]
# If no path is given, the app is resolved from the build output:
#   ${DERIVED:-./build/DerivedData}/Build/Products/${CONFIGURATION:-Release}/ASFW.app

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

CONFIGURATION="${CONFIGURATION:-Release}"
DERIVED="${DERIVED:-./build/DerivedData}"

APP_ENTITLEMENTS="ASFW/App.entitlements"
DEXT_ENTITLEMENTS="ASFWDriver/ASFWDriver.entitlements"

APP_PATH="${1:-${DERIVED}/Build/Products/${CONFIGURATION}/ASFW.app}"

err() { echo "[ERROR] $*" >&2; }
log() { echo "[INFO] $*"; }
ok()  { echo "[OK] $*"; }

[[ -d "${APP_PATH}" ]] || { err "App bundle not found: ${APP_PATH} (build it first with ./build.sh --config ${CONFIGURATION})"; exit 1; }
[[ -f "${APP_ENTITLEMENTS}" ]] || { err "Missing ${APP_ENTITLEMENTS}"; exit 1; }
[[ -f "${DEXT_ENTITLEMENTS}" ]] || { err "Missing ${DEXT_ENTITLEMENTS}"; exit 1; }

# The dext is bundled inside the app under Contents/Library/SystemExtensions/.
DEXT_PATH="$(find "${APP_PATH}/Contents/Library/SystemExtensions" -maxdepth 1 -name '*.dext' -print -quit 2>/dev/null || true)"
[[ -n "${DEXT_PATH}" ]] || { err "No .dext found under ${APP_PATH}/Contents/Library/SystemExtensions"; exit 1; }

# The entitlement set must match what the build put in Info.plist: a dext whose
# signature carries the restricted scsicontroller entitlement is killed by AMFI
# on enforcing machines, and the orphaned kernel-side SCSI stub then panics the
# kernel at boot. Detect the SCSI personality (present only when the app was
# built with ASFW_ENABLE_SCSI=YES / ./build.sh --scsi) and pick the matching file
# — never sign the SCSI entitlement onto a build without the personality, or
# vice versa.
if /usr/libexec/PlistBuddy -c "Print :IOKitPersonalities:ASFWSCSIControllerService" \
     "${DEXT_PATH}/Info.plist" >/dev/null 2>&1; then
  DEXT_ENTITLEMENTS="ASFWDriver/ASFWDriver+SCSI.entitlements"
  log "SCSI HBA personality detected — signing with ${DEXT_ENTITLEMENTS}"
  [[ -f "${DEXT_ENTITLEMENTS}" ]] || { err "Missing ${DEXT_ENTITLEMENTS}"; exit 1; }
else
  log "No SCSI HBA personality — signing with ${DEXT_ENTITLEMENTS} (audio only)"
fi

# Sign inner-first: the dext, then the enclosing app.
log "Signing dext: ${DEXT_PATH}"
codesign --force --sign - --timestamp=none \
  --entitlements "${DEXT_ENTITLEMENTS}" \
  "${DEXT_PATH}"

log "Signing app:  ${APP_PATH}"
codesign --force --sign - --timestamp=none \
  --entitlements "${APP_ENTITLEMENTS}" \
  "${APP_PATH}"

# Fail loudly if entitlements did not embed — that is the exact failure mode
# (empty entitlement dump -> "Missing entitlement ..." at install time).
log "Verifying embedded entitlements…"
if ! codesign -d --entitlements - --xml "${APP_PATH}" 2>/dev/null | grep -q 'system-extension.install'; then
  err "app is missing com.apple.developer.system-extension.install after signing"
  exit 1
fi
if ! codesign -d --entitlements - --xml "${DEXT_PATH}" 2>/dev/null | grep -q 'driverkit'; then
  err "dext is missing DriverKit entitlements after signing"
  exit 1
fi

ok "Signed ${APP_PATH} (ad-hoc, entitlements embedded). Install with SIP off + 'systemextensionsctl developer on'."
