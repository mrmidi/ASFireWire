#!/usr/bin/env bash
set -euo pipefail

APP_PROFILE="${ASFW_SAFE_APP_PROFILE:-/Users/chrisizatt/Downloads/ASFW_Chris_Mac_Studio(1).provisionprofile}"
DRIVER_PROFILE="${ASFW_SAFE_DRIVER_PROFILE:-/Users/chrisizatt/Downloads/ASFWDriver_Chris_MaciOS.provisionprofile}"
DEST_DIR="${HOME}/Library/MobileDevice/Provisioning Profiles"

die() {
  echo "error: $*" >&2
  exit 1
}

decode_profile() {
  local profile="$1"
  local out="$2"
  /usr/bin/security cms -D -i "$profile" > "$out"
}

plist_read() {
  local plist="$1"
  local key="$2"
  /usr/libexec/PlistBuddy -c "Print :${key}" "$plist" 2>/dev/null || true
}

install_profile() {
  local label="$1"
  local profile="$2"
  local expected_name="$3"
  local expected_app_id="$4"
  local tmp

  [[ -f "$profile" ]] || die "$label provisioning profile not found: $profile"

  tmp="$(/usr/bin/mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN

  local plist="$tmp/profile.plist"
  decode_profile "$profile" "$plist"

  local uuid name app_id
  uuid="$(plist_read "$plist" UUID)"
  name="$(plist_read "$plist" Name)"
  app_id="$(plist_read "$plist" Entitlements:com.apple.application-identifier)"

  [[ -n "$uuid" ]] || die "$label profile has no UUID: $profile"
  [[ "$name" == "$expected_name" ]] || die "$label profile name mismatch: expected '$expected_name', got '$name'"
  [[ "$app_id" == "$expected_app_id" ]] || die "$label profile app id mismatch: expected '$expected_app_id', got '$app_id'"

  /bin/mkdir -p "$DEST_DIR"
  /bin/cp -f "$profile" "$DEST_DIR/${uuid}.provisionprofile"

  echo "$label profile installed:"
  echo "  name: $name"
  echo "  app id: $app_id"
  echo "  uuid: $uuid"
  echo "  path: $DEST_DIR/${uuid}.provisionprofile"
}

install_profile \
  "app" \
  "$APP_PROFILE" \
  "ASFW Chris Mac Studio" \
  "8CZML8FK2D.com.chrisizatt.ASFWLocal"

install_profile \
  "driver" \
  "$DRIVER_PROFILE" \
  "ASFWDriver Chris Mac/iOS" \
  "8CZML8FK2D.com.chrisizatt.ASFWLocal.ASFWDriver"

echo "Safe local provisioning profiles are available to Xcode."
