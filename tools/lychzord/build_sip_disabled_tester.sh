#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONFIGURATION="${CONFIGURATION:-Release}"
DERIVED_DATA="${DERIVED_DATA:-$ROOT_DIR/build/DerivedDataLychzordSIP}"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT_DIR/build/lychzord-sip}"
APP_BUNDLE_ID="${ASFW_APP_BUNDLE_IDENTIFIER:-com.lychzord.ASFWTest}"
DRIVER_BUNDLE_ID="${ASFW_DRIVER_BUNDLE_IDENTIFIER:-$APP_BUNDLE_ID.ASFWDriver}"
APP_NAME="${APP_NAME:-ASFWLychzord.app}"
VERSION="${CURRENT_PROJECT_VERSION:-16}"
MACOS_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-15.5}"
DRIVERKIT_TARGET="${DRIVERKIT_DEPLOYMENT_TARGET:-24.0}"
ARCHS_VALUE="${ARCHS_VALUE:-arm64}"
REQUIRE_CORE_AUDIO_DEVICE="${ASFW_REQUIRE_CORE_AUDIO_DEVICE:-false}"
EXPECTED_CORE_AUDIO_DEVICE_NAME="${ASFW_EXPECTED_CORE_AUDIO_DEVICE_NAME:-}"
CLEAN=true
SETTINGS_ONLY=false

usage() {
  cat <<EOF
Usage: $0 [--settings-only] [--no-clean]

Builds a SIP-disabled Lychzord tester package with no Chris provisioning
profiles and no Apple Developer Team requirement.

Defaults:
  app bundle id:      $APP_BUNDLE_ID
  driver bundle id:   $DRIVER_BUNDLE_ID
  app name:           $APP_NAME
  version:            $VERSION
  macOS deployment:   $MACOS_DEPLOYMENT_TARGET
  DriverKit target:   $DRIVERKIT_TARGET
  archs:              $ARCHS_VALUE

Environment overrides:
  CONFIGURATION=Debug|Release
  DERIVED_DATA=/path/to/DerivedData
  OUTPUT_DIR=/path/to/output
  ASFW_APP_BUNDLE_IDENTIFIER=com.example.App
  ASFW_DRIVER_BUNDLE_IDENTIFIER=com.example.App.ASFWDriver
  APP_NAME=ASFWLychzord.app
  CURRENT_PROJECT_VERSION=16
  MACOSX_DEPLOYMENT_TARGET=15.5
  DRIVERKIT_DEPLOYMENT_TARGET=24.0
  ARCHS_VALUE=arm64
  ASFW_REQUIRE_CORE_AUDIO_DEVICE=false
  ASFW_EXPECTED_CORE_AUDIO_DEVICE_NAME="Midas Venice"
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --settings-only) SETTINGS_ONLY=true; shift ;;
    --no-clean) CLEAN=false; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown argument: $1" >&2; usage; exit 1 ;;
  esac
done

cd "$ROOT_DIR"

COMMON_BUILD_SETTINGS=(
  DEVELOPMENT_TEAM=
  ASFW_APP_BUNDLE_IDENTIFIER="$APP_BUNDLE_ID"
  ASFW_DRIVER_BUNDLE_IDENTIFIER="$DRIVER_BUNDLE_ID"
  ASFW_APP_ENTITLEMENTS=ASFW/App.entitlements
  ASFW_APP_CODE_SIGN_STYLE=Manual
  ASFW_DRIVER_CODE_SIGN_STYLE=Manual
  ASFW_HELPER_CODE_SIGN_STYLE=Manual
  ASFW_APP_PROVISIONING_PROFILE_SPECIFIER=
  ASFW_DRIVER_PROVISIONING_PROFILE_SPECIFIER=
  ASFW_HELPER_PROVISIONING_PROFILE_SPECIFIER=
  CODE_SIGNING_ALLOWED=NO
  CODE_SIGNING_REQUIRED=NO
  AD_HOC_CODE_SIGNING_ALLOWED=YES
  CODE_SIGN_IDENTITY=-
  CURRENT_PROJECT_VERSION="$VERSION"
  MACOSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"
  DRIVERKIT_DEPLOYMENT_TARGET="$DRIVERKIT_TARGET"
  ARCHS="$ARCHS_VALUE"
  ONLY_ACTIVE_ARCH=NO
  RUN_CLANG_STATIC_ANALYZER=NO
)

XCODE_ARGS=(
  -project ASFW.xcodeproj
  -scheme ASFW
  -configuration "$CONFIGURATION"
  -derivedDataPath "$DERIVED_DATA"
  -destination "generic/platform=macOS"
)

if $SETTINGS_ONLY; then
  /usr/bin/xcodebuild "${XCODE_ARGS[@]}" -showBuildSettings "${COMMON_BUILD_SETTINGS[@]}" |
    /usr/bin/grep -E '^[[:space:]]+(TARGET_NAME|PRODUCT_BUNDLE_IDENTIFIER|CODE_SIGN_ENTITLEMENTS|CODE_SIGN_STYLE|CODE_SIGNING_ALLOWED|PROVISIONING_PROFILE_SPECIFIER|DEVELOPMENT_TEAM|CURRENT_PROJECT_VERSION|MACOSX_DEPLOYMENT_TARGET|DRIVERKIT_DEPLOYMENT_TARGET|ARCHS|ASFW_APP_ENTITLEMENTS|ASFW_APP_BUNDLE_IDENTIFIER|ASFW_DRIVER_BUNDLE_IDENTIFIER) ='
  exit 0
fi

BUILD_ACTIONS=(build)
if $CLEAN; then
  BUILD_ACTIONS=(clean build)
fi

/usr/bin/xcodebuild "${XCODE_ARGS[@]}" "${BUILD_ACTIONS[@]}" "${COMMON_BUILD_SETTINGS[@]}"

APP_PATH="$DERIVED_DATA/Build/Products/$CONFIGURATION/ASFW.app"
SYS_EXT_DIR="$APP_PATH/Contents/Library/SystemExtensions"
HELPER_PATH="$APP_PATH/Contents/Library/Helpers/ASFWPrivilegedHelper"
DAEMON_PLIST="$APP_PATH/Contents/Library/LaunchDaemons/${APP_BUNDLE_ID}.PrivilegedHelper.plist"
EXPECTED_DEXT_PATH="$SYS_EXT_DIR/$DRIVER_BUNDLE_ID.dext"

[[ -d "$APP_PATH" ]] || { echo "error: built app not found: $APP_PATH" >&2; exit 1; }
[[ -x "$HELPER_PATH" ]] || { echo "error: helper missing: $HELPER_PATH" >&2; exit 1; }
[[ -f "$DAEMON_PLIST" ]] || { echo "error: helper launchd plist missing: $DAEMON_PLIST" >&2; exit 1; }

if [[ -d "$SYS_EXT_DIR" ]]; then
  for dext in "$SYS_EXT_DIR"/*.dext; do
    [[ -d "$dext" ]] || continue
    info_plist="$dext/Contents/Info.plist"
    [[ -f "$info_plist" ]] || info_plist="$dext/Info.plist"
    [[ -f "$info_plist" ]] || continue
    dext_bundle_id="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$info_plist" 2>/dev/null || true)"
    [[ -n "$dext_bundle_id" ]] || continue
    expected="$SYS_EXT_DIR/$dext_bundle_id.dext"
    if [[ "$dext" != "$expected" ]]; then
      /bin/rm -rf "$expected"
      /bin/mv "$dext" "$expected"
      echo "Renamed embedded dext to match bundle id: $(basename "$expected")"
    fi
  done
fi
[[ -d "$EXPECTED_DEXT_PATH" ]] || { echo "error: embedded dext missing: $EXPECTED_DEXT_PATH" >&2; exit 1; }

DEXT_INFO_PLIST="$EXPECTED_DEXT_PATH/Contents/Info.plist"
[[ -f "$DEXT_INFO_PLIST" ]] || DEXT_INFO_PLIST="$EXPECTED_DEXT_PATH/Info.plist"
[[ -f "$DEXT_INFO_PLIST" ]] || { echo "error: embedded dext Info.plist missing: $EXPECTED_DEXT_PATH" >&2; exit 1; }

# The main local test path originally matched Chris's Agere/Lucent FW800 PCI ID
# exactly. The SIP-disabled tester lane needs to attach to whichever OHCI bridge
# the tester Mac exposes, so match the FireWire OHCI PCI class instead.
/usr/libexec/PlistBuddy -c "Delete :IOKitPersonalities:ASFWDriverService:IOPCIMatch" "$DEXT_INFO_PLIST" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Delete :IOKitPersonalities:ASFWDriverService:IOPCIClassMatch" "$DEXT_INFO_PLIST" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :IOKitPersonalities:ASFWDriverService:IOPCIClassMatch string 0x0c001000" "$DEXT_INFO_PLIST"
echo "Configured embedded dext for FireWire OHCI class match: IOPCIClassMatch=0x0c001000"

/usr/libexec/PlistBuddy -c "Delete :ASFWRequireCoreAudioDevice" "$APP_PATH/Contents/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :ASFWRequireCoreAudioDevice bool $REQUIRE_CORE_AUDIO_DEVICE" "$APP_PATH/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Delete :ASFWExpectedCoreAudioDeviceName" "$APP_PATH/Contents/Info.plist" 2>/dev/null || true
if [[ -n "$EXPECTED_CORE_AUDIO_DEVICE_NAME" ]]; then
  /usr/libexec/PlistBuddy -c "Add :ASFWExpectedCoreAudioDeviceName string $EXPECTED_CORE_AUDIO_DEVICE_NAME" "$APP_PATH/Contents/Info.plist"
else
  /usr/libexec/PlistBuddy -c "Add :ASFWExpectedCoreAudioDeviceName string" "$APP_PATH/Contents/Info.plist"
fi

/usr/libexec/PlistBuddy -c "Delete :EnvironmentVariables:ASFW_REQUIRE_CORE_AUDIO_DEVICE" "$DAEMON_PLIST" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :EnvironmentVariables:ASFW_REQUIRE_CORE_AUDIO_DEVICE string $REQUIRE_CORE_AUDIO_DEVICE" "$DAEMON_PLIST"
/usr/libexec/PlistBuddy -c "Delete :EnvironmentVariables:ASFW_EXPECTED_CORE_AUDIO_DEVICE_NAME" "$DAEMON_PLIST" 2>/dev/null || true
if [[ -n "$EXPECTED_CORE_AUDIO_DEVICE_NAME" ]]; then
  /usr/libexec/PlistBuddy -c "Add :EnvironmentVariables:ASFW_EXPECTED_CORE_AUDIO_DEVICE_NAME string $EXPECTED_CORE_AUDIO_DEVICE_NAME" "$DAEMON_PLIST"
else
  /usr/libexec/PlistBuddy -c "Add :EnvironmentVariables:ASFW_EXPECTED_CORE_AUDIO_DEVICE_NAME string" "$DAEMON_PLIST"
fi

WORK_DIR="$OUTPUT_DIR/work"
PKG_DIR="$OUTPUT_DIR/package"
/bin/rm -rf "$WORK_DIR" "$PKG_DIR"
/bin/mkdir -p "$WORK_DIR" "$PKG_DIR"

APP_ENTITLEMENTS="$WORK_DIR/app-ad-hoc.entitlements"
DRIVER_ENTITLEMENTS="$WORK_DIR/driver-ad-hoc.entitlements"

cat >"$APP_ENTITLEMENTS" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>com.apple.security.app-sandbox</key>
  <false/>
  <key>com.apple.developer.system-extension.install</key>
  <true/>
  <key>com.apple.security.device.audio-input</key>
  <true/>
  <key>com.apple.developer.driverkit.userclient-access</key>
  <array>
    <string>$DRIVER_BUNDLE_ID</string>
    <string>$DRIVER_BUNDLE_ID:ASFWDriverUserClient</string>
  </array>
</dict>
</plist>
EOF

cat >"$DRIVER_ENTITLEMENTS" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>com.apple.developer.driverkit</key>
  <true/>
  <key>com.apple.developer.driverkit.transport.pci</key>
  <array>
    <dict>
      <key>IOPCIPrimaryMatch</key>
      <string>0xFFFFFFFF&amp;0x00000000</string>
    </dict>
  </array>
  <key>com.apple.developer.driverkit.family.audio</key>
  <true/>
</dict>
</plist>
EOF

sign_file_if_present() {
  local path="$1"
  [[ -e "$path" ]] || return 0
  /usr/bin/codesign --force --sign - -o runtime --timestamp=none "$path"
}

while IFS= read -r nested; do
  sign_file_if_present "$nested"
done < <(/usr/bin/find "$APP_PATH/Contents/MacOS" -maxdepth 1 -type f \( -name '*.dylib' -o -name '__preview.dylib' \) -print 2>/dev/null)

if [[ -d "$APP_PATH/Contents/Frameworks" ]]; then
  while IFS= read -r nested; do
    sign_file_if_present "$nested"
  done < <(/usr/bin/find "$APP_PATH/Contents/Frameworks" -type f \( -name '*.dylib' -o -perm -111 \) -print 2>/dev/null)
fi

/usr/bin/codesign --force --sign - -o runtime --timestamp=none "$HELPER_PATH"
/usr/bin/codesign --force --sign - -o runtime --entitlements "$DRIVER_ENTITLEMENTS" --timestamp=none "$EXPECTED_DEXT_PATH"
/usr/bin/codesign --force --sign - -o runtime --entitlements "$APP_ENTITLEMENTS" --timestamp=none "$APP_PATH"

/usr/bin/codesign --verify --strict --verbose=4 "$HELPER_PATH"
/usr/bin/codesign --verify --strict --verbose=4 "$EXPECTED_DEXT_PATH"
/usr/bin/codesign --verify --strict --deep --verbose=4 "$APP_PATH"

PACK_STAMP="$(date +%Y%m%d-%H%M%S)"
PACKAGE_ROOT="$PKG_DIR/ASFWLychzordSIP-v${VERSION}-${PACK_STAMP}"
PACKAGE_APP="$PACKAGE_ROOT/$APP_NAME"
ZIP_PATH="$OUTPUT_DIR/ASFWLychzordSIP-v${VERSION}-${PACK_STAMP}.zip"
/bin/mkdir -p "$PACKAGE_ROOT"
/usr/bin/ditto "$APP_PATH" "$PACKAGE_APP"

cat >"$PACKAGE_ROOT/README-LYCHZORD-SIP.txt" <<EOF
ASFW Lychzord SIP-disabled tester build
======================================

This is a local testing build for macOS $MACOS_DEPLOYMENT_TARGET+ with SIP disabled.
It is not notarised and is not intended for normal end users.
This package uses driver-only maintenance health, so it does not wait for an
Alesis CoreAudio device before considering the driver refresh complete.
This package matches FireWire OHCI PCI controllers by class (pciclass,0c0010),
not only Chris's local Agere pci11c1,5901 controller.

Install on the tester Mac:

1. Confirm SIP is disabled:
   csrutil status

2. Enable system extension developer mode:
   sudo systemextensionsctl developer on

3. Copy $APP_NAME to /Applications.

4. Remove quarantine:
   sudo xattr -dr com.apple.quarantine /Applications/$APP_NAME

5. Verify the bundle has not been re-signed or modified:
   codesign --verify --strict --deep --verbose=4 /Applications/$APP_NAME
   codesign -d --entitlements :- /Applications/$APP_NAME 2>/dev/null
   codesign -d --entitlements :- /Applications/$APP_NAME/Contents/Library/SystemExtensions/$DRIVER_BUNDLE_ID.dext 2>/dev/null

6. Open the app:
   open /Applications/$APP_NAME

7. Use Install / Update Driver once. Approve any System Settings prompt.

8. If the Midas still does not appear, do not keep pressing Repair Driver.
   Capture diagnostics and send the snapshot path/logs back.

After install, the minimum expected driver state is:
  systemextensionsctl list
    -> $DRIVER_BUNDLE_ID ($VERSION) activated enabled
  ps -ax | grep -i ASFW
    -> an ASFWDriver/DriverKit process should be present, not only the app/helper.

If the system extension is active but no ASFWDriver process appears, collect:
  system_profiler SPPCIDataType -detailLevel full | egrep -A12 -B4 'IEEE 1394|FireWire|Open HCI|pci'
  system_profiler SPFireWireDataType
  log show --last 15m --style compact --predicate 'process == "sysextd" OR eventMessage CONTAINS "ASFW" OR eventMessage CONTAINS "$DRIVER_BUNDLE_ID" OR eventMessage CONTAINS "IOPCIClassMatch"'

If the app reports "Repair needed: ASFW driver CDHash does not match the staged
driver" immediately after replacing the app, macOS is probably still running an
older same-version dext. Use a package with a higher CURRENT_PROJECT_VERSION, or
uninstall, reboot, then install once from the new package.

Terminal fallback for a stuck empty-Team-ID tester dext:
  systemextensionsctl list
  sudo systemextensionsctl uninstall - $DRIVER_BUNDLE_ID
  sudo systemextensionsctl gc
  sudo reboot

Do not run:
  sudo codesign --force --deep --sign - /Applications/$APP_NAME

That replaces the entitlements this build intentionally embeds.

If the app crashes at launch with "Taskgated Invalid Signature", send:
  sw_vers
  csrutil status
  systemextensionsctl developer
  codesign --verify --strict --deep --verbose=4 /Applications/$APP_NAME
  codesign -dv --verbose=4 /Applications/$APP_NAME 2>&1
EOF

(cd "$PKG_DIR" && /usr/bin/ditto -c -k --keepParent "$(basename "$PACKAGE_ROOT")" "$ZIP_PATH")
/usr/bin/shasum -a 256 "$ZIP_PATH" >"$ZIP_PATH.sha256"

echo
echo "Built SIP-disabled tester package:"
echo "  package dir: $PACKAGE_ROOT"
echo "  zip:         $ZIP_PATH"
echo "  sha256:      $ZIP_PATH.sha256"
echo
echo "App entitlements:"
/usr/bin/codesign -d --entitlements :- "$PACKAGE_APP" 2>/dev/null
echo
echo "Driver entitlements:"
/usr/bin/codesign -d --entitlements :- "$PACKAGE_APP/Contents/Library/SystemExtensions/$DRIVER_BUNDLE_ID.dext" 2>/dev/null
