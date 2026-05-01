#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DRIVER_PROFILE="${ASFW_FULL_DEBUG_DRIVER_PROFILE:-/Users/chrisizatt/Downloads/ASFWDriver_Chris_MaciOS-2.provisionprofile}"

die() {
  echo "error: $*" >&2
  exit 1
}

[[ -f "$DRIVER_PROFILE" ]] || die "full-debug driver profile not found: $DRIVER_PROFILE"

tmp="$(/usr/bin/mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

/usr/bin/security cms -D -i "$DRIVER_PROFILE" > "$tmp/driver-profile.plist"
if ! /usr/libexec/PlistBuddy -c 'Print :Entitlements:com.apple.developer.driverkit.allow-any-userclient-access' "$tmp/driver-profile.plist" >/dev/null 2>&1; then
  die "driver profile does not grant com.apple.developer.driverkit.allow-any-userclient-access: $DRIVER_PROFILE"
fi

echo "Building ASFW full-debug local app/driver."
echo "  driver profile: $DRIVER_PROFILE"
echo "  driver entitlement: com.apple.developer.driverkit.allow-any-userclient-access"
echo "  version: ${ASFW_CURRENT_PROJECT_VERSION:-28}"

export ASFW_CURRENT_PROJECT_VERSION="${ASFW_CURRENT_PROJECT_VERSION:-28}"
export DERIVED_DATA="${DERIVED_DATA:-$ROOT_DIR/build/DerivedDataFullDebugLocal}"
export ASFW_SAFE_DRIVER_PROFILE="$DRIVER_PROFILE"
export ASFW_DRIVER_ENTITLEMENTS_PATH="ASFWDriver/ASFWDriverAllowAnyUserClient.entitlements"
export ASFW_ALLOW_ANY_USERCLIENT_DRIVER="YES"

"$ROOT_DIR/tools/local/build_v16_safe_local.sh" "$@"
