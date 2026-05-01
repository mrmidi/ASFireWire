#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ $# -lt 1 ]]; then
  echo "Usage: $0 /path/to/ASFW.app" >&2
  echo "Refusing to stage from a stale DerivedData default. Use tools/local/build_v16_safe_local.sh --stage-app or pass an explicit app path." >&2
  exit 2
fi

SOURCE_APP="$1"
DEST_APP="${ASFW_LOCAL_APP_DEST:-/Applications/ASFWLocal.app}"
APP_ENTITLEMENTS="${ASFW_LOCAL_APP_ENTITLEMENTS:-"$ROOT_DIR/build/DerivedDataLocalSignedInstallOnly/Build/Intermediates.noindex/ASFW.build/Debug/ASFW.build/ASFW.app.xcent"}"

if [[ ! -d "$SOURCE_APP" ]]; then
  echo "Source app not found: $SOURCE_APP" >&2
  echo "Build the local signed bundle first, then rerun this script." >&2
  exit 1
fi

/bin/rm -rf "$DEST_APP"
/usr/bin/ditto "$SOURCE_APP" "$DEST_APP"

renamed=0
sys_ext_dir="$DEST_APP/Contents/Library/SystemExtensions"
if [[ -d "$sys_ext_dir" ]]; then
  for dext in "$sys_ext_dir"/*.dext; do
    [[ -d "$dext" ]] || continue
    bundle_id="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$dext/Info.plist" 2>/dev/null || true)"
    [[ -n "$bundle_id" ]] || continue
    expected="$sys_ext_dir/$bundle_id.dext"
    if [[ "$dext" != "$expected" ]]; then
      rm -rf "$expected"
      mv "$dext" "$expected"
      renamed=1
      echo "Renamed embedded system extension to: $(basename "$expected")"
    fi
  done
fi

if [[ "$renamed" == "1" ]]; then
  if [[ ! -f "$APP_ENTITLEMENTS" ]]; then
    echo "App entitlements not found for re-signing: $APP_ENTITLEMENTS" >&2
    exit 1
  fi

  sign_identity="${ASFW_CODESIGN_IDENTITY:-}"
  if [[ -z "$sign_identity" ]]; then
    sign_identity="$(/usr/bin/codesign -dv --verbose=4 "$SOURCE_APP" 2>&1 | /usr/bin/sed -n 's/^Authority=//p' | /usr/bin/head -n 1)"
  fi
  if [[ -z "$sign_identity" ]]; then
    echo "Unable to determine signing identity. Set ASFW_CODESIGN_IDENTITY." >&2
    exit 1
  fi

  /usr/bin/codesign --force --sign "$sign_identity" -o runtime --entitlements "$APP_ENTITLEMENTS" --timestamp=none "$DEST_APP"
fi

/usr/bin/codesign --verify --strict --deep --verbose=2 "$DEST_APP"
/usr/bin/open -n "$DEST_APP"

echo "Staged and launched: $DEST_APP"
