#!/bin/zsh
#
# ASFireWire – local installer for a release bundle.
#
# The release ZIP deliberately contains an unsigned ASFW.app. This script runs on
# the installer's Mac, ad-hoc signs the dext first and the app second, then installs
# the result in /Applications. No Apple Developer account or provisioning profile
# is required for this experimental local-install path.
#
# A ZIP whose app already carries a team signature and its embedded provisioning
# profiles is installed as it is: re-signing it ad-hoc would replace a signature
# AMFI can validate with one it cannot, so that path keeps SIP enabled and skips
# system-extension developer mode (the supported DriverKit development flow).

emulate -L zsh
setopt ERR_EXIT NO_UNSET PIPE_FAIL NO_NOMATCH

SCRIPT_DIR="${0:A:h}"
APP_PATH="${SCRIPT_DIR}/ASFW.app"
INSTALL_DIR="${ASFW_INSTALL_DIR:-/Applications}"
ASSUME_YES=false
STEP=0

ENTITLEMENT_DIR="${SCRIPT_DIR}/Entitlements"
if [[ ! -d "$ENTITLEMENT_DIR" && -d "${SCRIPT_DIR}/ASFW" && -d "${SCRIPT_DIR}/ASFWDriver" ]]; then
  # Also allow the script to be exercised directly from the source checkout.
  ENTITLEMENT_DIR="${SCRIPT_DIR}"
  APP_ENTITLEMENTS="${SCRIPT_DIR}/ASFW/App.entitlements"
  DEXT_ENTITLEMENTS="${SCRIPT_DIR}/ASFWDriver/ASFWDriver.entitlements"
else
  APP_ENTITLEMENTS="${ENTITLEMENT_DIR}/App.entitlements"
  DEXT_ENTITLEMENTS="${ENTITLEMENT_DIR}/ASFWDriver.entitlements"
fi

usage() {
  cat <<'EOF'
Usage: ./install.sh [--yes]

Install the ASFW.app next to this script:
  1. Check the bundle's signature: a team-signed app with its provisioning
     profiles installs with SIP enabled; a bundle without them is ad-hoc signed
     and needs SIP disabled plus system-extension developer mode.
  2. Remove the download quarantine flag.
  3. Ad-hoc sign the dext first, then the enclosing app (skipped when the
     bundle already carries a team signature).
  4. Verify signatures and embedded entitlements.
  5. Copy the app to /Applications.
  6. Open the installed app.

Options:
  --yes    Replace an existing /Applications/ASFW.app without prompting.

Environment override for advanced users:
  ASFW_INSTALL_DIR        Destination directory (default: /Applications).
EOF
}

info() {
  print -r -- "[ASFW] $*"
}

step() {
  STEP=$((STEP + 1))
  print -r -- ""
  print -r -- "[ASFW] Step ${STEP}: $*"
}

error() {
  print -u2 -r -- "[ASFW] ERROR: $*"
}

fail() {
  error "$*"
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "Required macOS command '$1' was not found. Install or update Xcode, then try again."
}

run_with_privilege() {
  if [[ -w "$INSTALL_DIR" ]]; then
    "$@"
  else
    info "Administrator permission is required for $INSTALL_DIR. macOS will ask for your password."
    sudo "$@"
  fi
}

read_plist_value() {
  local plist_path="$1"
  local plist_key="$2"
  /usr/libexec/PlistBuddy -c "Print :${plist_key}" "$plist_path" 2>/dev/null || true
}

# A bundle signed by an Apple-issued team identity and carrying its own embedded
# provisioning profiles is validated by AMFI on its own: that is the supported
# DriverKit path, and it keeps SIP enabled. Only a bundle without that signature
# has to take the ad-hoc path, which in turn needs SIP disabled and
# system-extension developer mode.
bundle_is_team_signed() {
  [[ -f "${DEXT_PATH}/embedded.provisionprofile" ]] || return 1
  [[ -f "${APP_PATH}/Contents/embedded.provisionprofile" ]] || return 1
  codesign -dv "$DEXT_PATH" 2>&1 | grep -q '^TeamIdentifier=' || return 1
  codesign -dv "$APP_PATH" 2>&1 | grep -q '^TeamIdentifier=' || return 1
  codesign --verify --deep --strict "$APP_PATH" >/dev/null 2>&1
}

while (( $# > 0 )); do
  case "$1" in
    --yes)
      ASSUME_YES=true
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      fail "Unknown option: $1"
      ;;
  esac
done

require_command csrutil
require_command systemextensionsctl
require_command codesign
require_command xattr
require_command ditto
require_command find
require_command open
[[ -x /usr/libexec/PlistBuddy ]] || fail "Required macOS command '/usr/libexec/PlistBuddy' was not found."

[[ -d "$APP_PATH" ]] || fail "ASFW.app was not found next to this script. Extract the complete release ZIP and run ./install.sh from the extracted folder."
DEXT_PATH="$(find "$APP_PATH/Contents/Library/SystemExtensions" -maxdepth 1 -type d -name '*.dext' -print -quit 2>/dev/null || true)"
[[ -n "$DEXT_PATH" ]] || fail "The release bundle does not contain a DriverKit extension under ASFW.app/Contents/Library/SystemExtensions."

TEAM_SIGNED=false
if bundle_is_team_signed; then
  TEAM_SIGNED=true
fi
if [[ "$TEAM_SIGNED" != true ]]; then
  [[ -f "$APP_ENTITLEMENTS" ]] || fail "The release bundle is incomplete: missing the app entitlement file."
  [[ -f "$DEXT_ENTITLEMENTS" ]] || fail "The release bundle is incomplete: missing the DriverKit entitlement file."
fi

APP_INFO_PLIST="${APP_PATH}/Contents/Info.plist"
# A .dext is itself the bundle; unlike the enclosing app, its Info.plist is at
# the bundle root rather than under a nested Contents directory.
DEXT_INFO_PLIST="${DEXT_PATH}/Info.plist"
[[ -f "$APP_INFO_PLIST" && -f "$DEXT_INFO_PLIST" ]] || fail "The app or dext is missing its Info.plist; the release bundle may be damaged."

APP_BUNDLE_ID="$(read_plist_value "$APP_INFO_PLIST" "CFBundleIdentifier")"
DEXT_BUNDLE_ID="$(read_plist_value "$DEXT_INFO_PLIST" "CFBundleIdentifier")"
[[ -n "$APP_BUNDLE_ID" && -n "$DEXT_BUNDLE_ID" ]] || fail "Could not read the app and dext bundle identifiers."

step "Check macOS security settings"
if [[ "$TEAM_SIGNED" == true ]]; then
  info "The app and dext carry a team signature and their provisioning profiles, so SIP can stay enabled."
else
  SIP_STATUS="$(csrutil status 2>&1 || true)"
  if ! print -r -- "$SIP_STATUS" | grep -qi 'disabled'; then
    error "System Integrity Protection is not disabled, and this ASFW.app carries no team signature."
    print -u2 -r -- "An ad-hoc signed DriverKit extension can only be activated with SIP disabled:"
    print -u2 -r -- "boot into macOS Recovery, open Terminal, run 'csrutil disable', reboot, and run this installer again."
    print -u2 -r -- "To install without disabling SIP, use a build signed with the team's identity and provisioning profiles."
    exit 1
  fi
  info "SIP is disabled, so this experimental ad-hoc DriverKit installation can continue."
fi

if [[ "$TEAM_SIGNED" == true ]]; then
  step "System-extension developer mode"
  info "Skipped: a team-signed build does not need it."
else
  step "Enable system-extension developer mode"
  if ! systemextensionsctl developer on; then
    fail "Could not enable system-extension developer mode. Run 'systemextensionsctl developer on' in Terminal and try again."
  fi
  info "System-extension developer mode is enabled."
fi

step "Remove the download quarantine flag"
if xattr -lr "$APP_PATH" 2>/dev/null | grep -q 'com.apple.quarantine'; then
  info "Removing com.apple.quarantine from ASFW.app and its embedded dext."
  xattr -dr com.apple.quarantine "$APP_PATH" || fail "Could not remove the quarantine flag from ASFW.app."
else
  info "No download quarantine flag was found."
fi

if [[ "$TEAM_SIGNED" == true ]]; then
  step "Sign the DriverKit extension and enclosing app"
  info "Skipped: ${DEXT_BUNDLE_ID} and ${APP_BUNDLE_ID} keep their team signature."
else
  step "Sign the DriverKit extension and enclosing app"
  info "Ad-hoc signing the dext first: ${DEXT_BUNDLE_ID}"
  codesign --force --timestamp=none --sign - \
    --entitlements "$DEXT_ENTITLEMENTS" "$DEXT_PATH" \
    || fail "The DriverKit extension could not be ad-hoc signed."

  info "Ad-hoc signing the app second: ${APP_BUNDLE_ID}"
  codesign --force --timestamp=none --sign - \
    --entitlements "$APP_ENTITLEMENTS" "$APP_PATH" \
    || fail "The app could not be ad-hoc signed."
fi

step "Verify signatures and embedded entitlements"
codesign --verify --deep --strict --verbose=2 "$APP_PATH" \
  || fail "codesign verification failed for the signed ASFW.app."

APP_ENTITLEMENT_DUMP="$(codesign -d --entitlements - --xml "$APP_PATH" 2>&1 || true)"
DEXT_ENTITLEMENT_DUMP="$(codesign -d --entitlements - --xml "$DEXT_PATH" 2>&1 || true)"
print -r -- "$APP_ENTITLEMENT_DUMP" | grep -q 'system-extension.install' \
  || fail "The signed app is missing com.apple.developer.system-extension.install."
print -r -- "$DEXT_ENTITLEMENT_DUMP" | grep -q 'com.apple.developer.driverkit' \
  || fail "The signed dext is missing its DriverKit entitlements."
info "The app and dext signatures contain the expected entitlements."

step "Copy the signed app to ${INSTALL_DIR}"
[[ -d "$INSTALL_DIR" ]] || fail "Install destination does not exist: ${INSTALL_DIR}"

if [[ -e "${INSTALL_DIR}/ASFW.app" && "$ASSUME_YES" != true ]]; then
  print -n -- "[ASFW] ${INSTALL_DIR}/ASFW.app already exists. Replace it? [y/N] "
  read -r answer
  [[ "$answer" == [yY]* ]] || fail "Installation cancelled; the existing app was left untouched."
fi

if [[ -e "${INSTALL_DIR}/ASFW.app" ]]; then
  info "Replacing the existing ${INSTALL_DIR}/ASFW.app."
  run_with_privilege rm -rf "${INSTALL_DIR}/ASFW.app"
fi

run_with_privilege ditto --rsrc --extattr --acl "$APP_PATH" "${INSTALL_DIR}/ASFW.app" \
  || fail "Could not copy the signed app to ${INSTALL_DIR}."

codesign --verify --deep --strict "${INSTALL_DIR}/ASFW.app" \
  || fail "The installed app failed signature verification after copying."

step "Open ASFW.app"
if open "${INSTALL_DIR}/ASFW.app"; then
  info "ASFW.app is open. Use its Install button to activate the system extension."
else
  info "The app was installed, but macOS could not open it automatically. Open ${INSTALL_DIR}/ASFW.app manually."
fi

print -r -- ""
print -r -- "[ASFW] Installation complete."
print -r -- "[ASFW] Signed app: ${INSTALL_DIR}/ASFW.app"
print -r -- "[ASFW] Press Install in the app to activate the system extension."
if [[ "$TEAM_SIGNED" == true ]]; then
  print -r -- "[ASFW] The build is team-signed, so SIP stays enabled and macOS may ask you to approve the"
  print -r -- "[ASFW] extension in System Settings > General > Login Items & Extensions > Driver Extensions."
else
  print -r -- "[ASFW] Keep SIP disabled while using this experimental ad-hoc DriverKit build."
fi
