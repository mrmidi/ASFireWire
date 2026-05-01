#!/usr/bin/env bash
set -euo pipefail

# Private/local signing example for Chris's Mac Studio. The script name is
# historical; ASFW_CURRENT_PROJECT_VERSION controls the actual bundle version.
# See documentation/LOCAL_SIGNING_EXAMPLE_CHRIS.md.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONFIGURATION="${CONFIGURATION:-Debug}"
ARCH_NAME="${ARCH_NAME:-arm64}"
DERIVED_DATA="${DERIVED_DATA:-$ROOT_DIR/build/DerivedDataSafeLocalV16}"
ASFW_CURRENT_PROJECT_VERSION="${ASFW_CURRENT_PROJECT_VERSION:-28}"
ASFW_CODE_SIGN_IDENTITY="${ASFW_CODE_SIGN_IDENTITY:-Apple Development: boggspa@hotmail.co.uk (QWB2SUQVJ3)}"
ASFW_APP_ENTITLEMENTS_PATH="${ASFW_APP_ENTITLEMENTS_PATH:-ASFW/AppInstallOnly.entitlements}"
ASFW_DRIVER_ENTITLEMENTS_PATH="${ASFW_DRIVER_ENTITLEMENTS_PATH:-ASFWDriver/ASFWDriver.entitlements}"
ASFW_ALLOW_ANY_USERCLIENT_DRIVER="${ASFW_ALLOW_ANY_USERCLIENT_DRIVER:-NO}"
ASFW_APP_PROFILE_SPECIFIER="${ASFW_APP_PROFILE_SPECIFIER:-ASFW Chris Mac Studio}"
ASFW_DRIVER_PROFILE_SPECIFIER="${ASFW_DRIVER_PROFILE_SPECIFIER:-ASFWDriver Chris Mac/iOS}"
INSTALL_PROFILES=true
SETTINGS_ONLY=false
STAGE_APP=false
CLEAN=true

usage() {
  cat <<EOF
Usage: $0 [--settings-only] [--stage-app] [--skip-profile-install] [--no-clean]

Builds ASFW for Chris's Mac Studio using the safe local profiles:
  app:    ASFW Chris Mac Studio
  driver: ASFWDriver Chris Mac/iOS

This lane keeps ASFW/App.entitlements in the repo but builds the app with
ASFW/AppInstallOnly.entitlements, so the pending UserClient entitlement is not
required for this local install/update/repair build.

Environment overrides:
  CONFIGURATION=Debug|Release
  ARCH_NAME=arm64
  ASFW_CURRENT_PROJECT_VERSION=28
  DERIVED_DATA=/path/to/DerivedData
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --settings-only) SETTINGS_ONLY=true; shift ;;
    --stage-app) STAGE_APP=true; shift ;;
    --skip-profile-install) INSTALL_PROFILES=false; shift ;;
    --no-clean) CLEAN=false; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown argument: $1" >&2; usage; exit 1 ;;
  esac
done

cd "$ROOT_DIR"

if $INSTALL_PROFILES; then
  "$ROOT_DIR/tools/local/install_safe_profiles.sh"
fi

COMMON_BUILD_SETTINGS=(
  DEVELOPMENT_TEAM=8CZML8FK2D
  ASFW_APP_BUNDLE_IDENTIFIER=com.chrisizatt.ASFWLocal
  ASFW_DRIVER_BUNDLE_IDENTIFIER=com.chrisizatt.ASFWLocal.ASFWDriver
  ASFW_APP_ENTITLEMENTS="$ASFW_APP_ENTITLEMENTS_PATH"
  ASFW_DRIVER_ENTITLEMENTS="$ASFW_DRIVER_ENTITLEMENTS_PATH"
  ASFW_ALLOW_ANY_USERCLIENT_DRIVER="$ASFW_ALLOW_ANY_USERCLIENT_DRIVER"
  ASFW_APP_CODE_SIGN_STYLE=Manual
  ASFW_DRIVER_CODE_SIGN_STYLE=Manual
  ASFW_HELPER_CODE_SIGN_STYLE=Manual
  "ASFW_APP_PROVISIONING_PROFILE_SPECIFIER=$ASFW_APP_PROFILE_SPECIFIER"
  "ASFW_DRIVER_PROVISIONING_PROFILE_SPECIFIER=$ASFW_DRIVER_PROFILE_SPECIFIER"
  ASFW_HELPER_PROVISIONING_PROFILE_SPECIFIER=
  CURRENT_PROJECT_VERSION="$ASFW_CURRENT_PROJECT_VERSION"
  "CODE_SIGN_IDENTITY=$ASFW_CODE_SIGN_IDENTITY"
  RUN_CLANG_STATIC_ANALYZER=NO
)

XCODE_ARGS=(
  -project ASFW.xcodeproj
  -scheme ASFW
  -configuration "$CONFIGURATION"
  -derivedDataPath "$DERIVED_DATA"
  -destination "platform=macOS,arch=$ARCH_NAME"
)

if $SETTINGS_ONLY; then
  /usr/bin/xcodebuild "${XCODE_ARGS[@]}" -showBuildSettings "${COMMON_BUILD_SETTINGS[@]}" |
    /usr/bin/grep -E '^[[:space:]]+(TARGET_NAME|PRODUCT_BUNDLE_IDENTIFIER|CODE_SIGN_ENTITLEMENTS|CODE_SIGN_STYLE|PROVISIONING_PROFILE_SPECIFIER|DEVELOPMENT_TEAM|ASFW_APP_ENTITLEMENTS|ASFW_APP_BUNDLE_IDENTIFIER|ASFW_DRIVER_BUNDLE_IDENTIFIER) ='
  exit 0
fi

BUILD_ACTIONS=(build)
if $CLEAN; then
  BUILD_ACTIONS=(clean build)
fi

/usr/bin/xcodebuild "${XCODE_ARGS[@]}" "${BUILD_ACTIONS[@]}" "${COMMON_BUILD_SETTINGS[@]}"

APP_PATH="$DERIVED_DATA/Build/Products/$CONFIGURATION/ASFW.app"
[[ -d "$APP_PATH" ]] || { echo "error: built app not found: $APP_PATH" >&2; exit 1; }

APP_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP_PATH/Contents/Info.plist")"
HELPER_PATH="$APP_PATH/Contents/Library/Helpers/ASFWPrivilegedHelper"
DAEMON_PLIST="$APP_PATH/Contents/Library/LaunchDaemons/${APP_ID}.PrivilegedHelper.plist"
DEXT_PATH="$APP_PATH/Contents/Library/SystemExtensions/com.chrisizatt.ASFWLocal.ASFWDriver.dext"

renamed_dext=0
needs_resign=0
SYS_EXT_DIR="$APP_PATH/Contents/Library/SystemExtensions"
if [[ -d "$SYS_EXT_DIR" ]]; then
  for dext in "$SYS_EXT_DIR"/*.dext; do
    [[ -d "$dext" ]] || continue
    info_plist="$dext/Contents/Info.plist"
    if [[ ! -f "$info_plist" ]]; then
      info_plist="$dext/Info.plist"
    fi
    [[ -f "$info_plist" ]] || continue
    dext_bundle_id="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$info_plist" 2>/dev/null || true)"
    [[ -n "$dext_bundle_id" ]] || continue
    expected="$SYS_EXT_DIR/$dext_bundle_id.dext"
    if [[ "$dext" != "$expected" ]]; then
      /bin/rm -rf "$expected"
      /bin/mv "$dext" "$expected"
      renamed_dext=1
      needs_resign=1
      echo "Renamed embedded dext to match bundle id: $(basename "$expected")"
    fi
  done
fi

[[ -x "$HELPER_PATH" ]] || { echo "error: helper missing from app bundle: $HELPER_PATH" >&2; exit 1; }
[[ -f "$DAEMON_PLIST" ]] || { echo "error: helper launchd plist missing: $DAEMON_PLIST" >&2; exit 1; }
[[ -d "$DEXT_PATH" ]] || { echo "error: embedded dext missing or has the wrong bundle directory: $DEXT_PATH" >&2; exit 1; }

if ! /usr/bin/codesign --verify --strict --verbose=2 "$HELPER_PATH" >/dev/null 2>&1; then
  /usr/bin/codesign --force --sign "$ASFW_CODE_SIGN_IDENTITY" -o runtime --timestamp=none "$HELPER_PATH"
  needs_resign=1
  echo "Signed embedded maintenance helper."
fi

if [[ "$needs_resign" == "1" ]]; then
  APP_XCENT="$DERIVED_DATA/Build/Intermediates.noindex/ASFW.build/$CONFIGURATION/ASFW.build/ASFW.app.xcent"
  [[ -f "$APP_XCENT" ]] || { echo "error: app xcent not found for re-signing: $APP_XCENT" >&2; exit 1; }
  /usr/bin/codesign --force --sign "$ASFW_CODE_SIGN_IDENTITY" -o runtime --entitlements "$APP_XCENT" --timestamp=none "$APP_PATH"
fi

/usr/bin/codesign --verify --strict --deep --verbose=2 "$APP_PATH"

echo
echo "Built safe local app:"
echo "  app: $APP_PATH"
echo "  app bundle id: $APP_ID"
echo "  current project version: $ASFW_CURRENT_PROJECT_VERSION"
echo "  driver: $DEXT_PATH"
echo "  helper: $HELPER_PATH"
echo "  staged driver CDHash: $(/usr/bin/codesign -dv --verbose=4 "$DEXT_PATH" 2>&1 | /usr/bin/sed -n 's/^CDHash=//p' | /usr/bin/head -n 1)"
echo
echo "App entitlements:"
/usr/bin/codesign -d --entitlements :- "$APP_PATH" 2>/dev/null
echo
echo "Driver entitlements:"
/usr/bin/codesign -d --entitlements :- "$DEXT_PATH" 2>/dev/null

if $STAGE_APP; then
  "$ROOT_DIR/tools/debug/stage_local_app.sh" "$APP_PATH"
else
  echo
  echo "Not staged. To replace /Applications/ASFWLocal.app when ready, rerun with --stage-app."
fi
