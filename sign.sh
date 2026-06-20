#!/usr/bin/env bash
# sign.sh — ad-hoc re-sign the built ASFW.app + bundled dext WITH entitlements.
#
# Why this exists:
#   build.sh builds with CODE_SIGNING_ALLOWED=NO, which produces a *linker-signed*
#   ad-hoc app whose signature carries NO entitlements, and an unsigned dext.
#   sysextd then rejects "Install" with `Missing entitlement
#   com.apple.developer.system-extension.install` — and AMFI bypass cannot help,
#   because the entitlement is absent from the signature, not merely unvalidated.
#
#   This script re-signs ad-hoc (CODE_SIGN_IDENTITY = "-", the free path) and
#   embeds the entitlements raw. With SIP disabled on the target, AMFI honors them
#   without an Apple-issued provisioning profile.
#
# Run after ./build.sh. Sign innermost (dext) first, then the app.
set -euo pipefail

CONFIGURATION="${CONFIGURATION:-Debug}"
BUILD_DIR="${BUILD_DIR:-build}"
DERIVED="${DERIVED:-${BUILD_DIR}/DerivedData}"
PRODUCTS="${DERIVED}/Build/Products/${CONFIGURATION}"

APP="${1:-${PRODUCTS}/ASFW.app}"
APP_ENTS="ASFW/App.entitlements"
DEXT_ENTS="ASFWDriver/ASFWDriver.entitlements"

[[ -d "$APP" ]] || { echo "error: app not found: $APP (build it first with ./build.sh)" >&2; exit 1; }
[[ -f "$APP_ENTS" ]] || { echo "error: missing $APP_ENTS" >&2; exit 1; }
[[ -f "$DEXT_ENTS" ]] || { echo "error: missing $DEXT_ENTS" >&2; exit 1; }

DEXT="$APP/Contents/Library/SystemExtensions/net.mrmidi.ASFW.ASFWDriver.dext"
[[ -d "$DEXT" ]] || { echo "error: bundled dext not found: $DEXT" >&2; exit 1; }

echo "Signing bundled dext (ad-hoc + DriverKit entitlements)…"
codesign --force --sign - --entitlements "$DEXT_ENTS" --timestamp=none "$DEXT"

echo "Signing app (ad-hoc + system-extension.install + userclient-access)…"
codesign --force --sign - --entitlements "$APP_ENTS" --timestamp=none "$APP"

echo
echo "Verifying embedded entitlements:"
echo "--- app ---"
codesign -d --entitlements - --xml "$APP" 2>/dev/null | grep -o 'com\.apple\.[a-z.-]*' | sort -u
echo "--- dext ---"
codesign -d --entitlements - --xml "$DEXT" 2>/dev/null | grep -o 'com\.apple\.[a-z.-]*' | sort -u
echo
echo "Done. Both objects are now ad-hoc signed with entitlements embedded."
echo "On the target: SIP off (+ systemextensionsctl developer on) → Install should pass."
