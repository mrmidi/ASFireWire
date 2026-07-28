#!/usr/bin/env bash
#
# Build, sign, stage, and optionally activate ASFW.app using the workflow
# documented in README.md.

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

CONFIGURATION="Release"
DO_BUILD=true
ENABLE_SCSI=false
LAUNCH_APP=true
FRESH_REPLACE=false
AUTO_ACTIVATE=false

APP_DEST="/Applications/ASFW.app"
SYSTEM_EXTENSION_ID="net.mrmidi.ASFW.ASFWDriver"
DEXT_REL="Contents/Library/SystemExtensions/${SYSTEM_EXTENSION_ID}.dext"
DEXT_BINARY_REL="${DEXT_REL}/${SYSTEM_EXTENSION_ID}"
SYSTEM_EXTENSION_ROOT="/Library/SystemExtensions"
# Every install keeps the app it replaced so a bad build can be rolled back by
# hand. Without pruning, a day of iterating leaves a pile of app bundles in
# /Applications. Keep the most recent few and drop the rest.
BACKUP_KEEP_COUNT=3

usage() {
  cat <<'EOF'
Usage: ./install-asfw.sh [options]

Options:
  --config Debug|Release  Build configuration (default: Release)
  --scsi                  Include the experimental SCSI HBA
  --no-build              Reuse an existing build product
  --fresh, --replace      Uninstall existing dext state before staging the app
  --activate, --refresh   Submit activation after install and verify the active dext
  --no-launch             Do not open ASFW.app after installation
  -h, --help              Show this help

This script never prompts for a password. Run "sudo -v" first when an existing
app or dext requires administrator access.

Examples:
  ./install-asfw.sh
  ./install-asfw.sh --config Debug --scsi --activate
  ./install-asfw.sh --config Debug --scsi --fresh --activate
  ./install-asfw.sh --config Release --scsi --no-build
EOF
}

log() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"; }
die() { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

run_maybe_sudo() {
  if "$@"; then
    return 0
  else
    local command_status=$?
  fi

  if command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
    sudo -n "$@"
    return $?
  fi
  log 'Administrator access may be required; run "sudo -v" and retry.'
  return "${command_status}"
}

sha256_file() {
  shasum -a 256 "$1" | awk '{print $1}'
}

entitlements_xml() {
  codesign -d --entitlements - --xml "$1" 2>/dev/null
}

has_entitlement() {
  local artifact="$1"
  local key="$2"
  entitlements_xml "${artifact}" | grep -q "<key>${key}</key>"
}

has_scsi_personality() {
  /usr/libexec/PlistBuddy \
    -c "Print :IOKitPersonalities:ASFWSCSIControllerService" \
    "$1/Info.plist" >/dev/null 2>&1
}

asfw_app_pids() {
  local pattern="${APP_DEST//./\\.}/Contents/MacOS/ASFW"
  pgrep -f "^${pattern}([[:space:]]|$)" 2>/dev/null || true
}

close_existing_app() {
  [[ -z "$(asfw_app_pids)" ]] && return 0
  log "Requesting the existing ASFW.app to quit..."
  osascript -e 'tell application id "net.mrmidi.ASFW" to quit' \
    >/dev/null 2>&1 || true

  local attempts=10
  while (( attempts > 0 )); do
    [[ -z "$(asfw_app_pids)" ]] && return 0
    sleep 1
    ((attempts--))
  done
  die "ASFW.app is still running; close it manually and rerun the installer"
}

system_extension_lines() {
  local extension_list
  extension_list="$(systemextensionsctl list 2>/dev/null)" || return 2
  grep -F "${SYSTEM_EXTENSION_ID}" <<<"${extension_list}" || true
}

system_dext_binary_paths() {
  find "${SYSTEM_EXTENSION_ROOT}" -maxdepth 3 -type f \
    -path "*/${SYSTEM_EXTENSION_ID}.dext/${SYSTEM_EXTENSION_ID}" \
    -print 2>/dev/null
}

system_dext_hashes_match() {
  local expected_hash="$1"
  local binary_path
  local installed_hash
  local found_binary=false

  while IFS= read -r binary_path; do
    [[ -n "${binary_path}" ]] || continue
    found_binary=true
    installed_hash="$(sha256_file "${binary_path}")"
    [[ "${installed_hash}" == "${expected_hash}" ]] || return 1
  done < <(system_dext_binary_paths)

  ${found_binary}
}

print_active_dext_status() {
  local extension_lines
  local binary_path

  extension_lines="$(system_extension_lines)" \
    || die "Unable to query the current system-extension state"
  if [[ -n "${extension_lines}" ]]; then
    printf '%s\n' "${extension_lines}"
  else
    log "No ASFW system-extension entry is visible."
  fi

  while IFS= read -r binary_path; do
    [[ -n "${binary_path}" ]] || continue
    log "Installed system dext hash: $(sha256_file "${binary_path}")"
  done < <(system_dext_binary_paths)
}

wait_for_active_dext_hash() {
  local expected_hash="$1"
  local attempts="${2:-30}"
  # macOS parks the extension in "activated waiting for user" until someone
  # approves it in System Settings and authenticates. That is a human step, not
  # machine progress the installer can wait out, so it draws on its own much
  # larger budget instead of consuming the transition budget.
  local approval_attempts="${3:-300}"
  local extension_lines
  local prompted=false

  while (( attempts > 0 && approval_attempts > 0 )); do
    extension_lines="$(system_extension_lines)" \
      || die "Unable to query the current system-extension state"
    if [[ -n "${extension_lines}" ]] \
      && ! grep -Fqv '[activated enabled]' <<<"${extension_lines}" \
      && system_dext_hashes_match "${expected_hash}"; then
      log "Active dext matches the installed build: ${expected_hash}"
      return 0
    fi

    if grep -Eq '\[activated waiting for user' <<<"${extension_lines}"; then
      if ! ${prompted}; then
        prompted=true
        log "macOS is waiting for you to approve the extension."
        log "  System Settings > General > Login Items & Extensions >"
        log "  Driver Extensions > enable ASFW, then authenticate."
        log "Waiting up to ${approval_attempts}s for that approval..."
      fi
      sleep 1
      ((approval_attempts--))
      continue
    fi

    sleep 1
    ((attempts--))
  done

  if ${prompted}; then
    log "The extension was never approved, so activation did not complete."
  else
    log "The active dext did not switch to the installed build."
  fi
  print_active_dext_status
  return 1
}

ensure_replace_mode_for_pending_transition() {
  local extension_lines
  extension_lines="$(system_extension_lines)" \
    || die "Unable to query the current system-extension state"

  if grep -Eq '\[(activated waiting .*reboot|terminating )' \
      <<<"${extension_lines}"; then
    log "ASFW has a pending system-extension transition:"
    printf '%s\n' "${extension_lines}"
    ${FRESH_REPLACE} && return 0
    die "Rerun with --fresh to uninstall the pending dext state before replacement"
  fi
}

uninstall_existing_system_extension() {
  local extension_lines
  extension_lines="$(system_extension_lines)" \
    || die "Unable to query the current system-extension state"
  if [[ -z "${extension_lines}" ]]; then
    log "No existing ASFW system extension needs to be uninstalled."
    return 0
  fi

  close_existing_app
  log "Uninstalling existing ASFW dext state before replacement..."
  run_maybe_sudo systemextensionsctl uninstall - "${SYSTEM_EXTENSION_ID}" \
    || die "Failed to uninstall ${SYSTEM_EXTENSION_ID}"

  local attempts=30
  while (( attempts > 0 )); do
    extension_lines="$(system_extension_lines)" \
      || die "Unable to verify the system-extension uninstall"
    if [[ -z "${extension_lines}" ]]; then
      log "The previous ASFW dext state has been removed."
      return 0
    fi
    sleep 1
    ((attempts--))
  done

  log "ASFW system-extension state still present after uninstall:"
  printf '%s\n' "${extension_lines}"
  die "macOS could not unload the previous dext; restart macOS before retrying"
}

verify_artifact() {
  local app_path="$1"
  local dext_path="${app_path}/${DEXT_REL}"

  [[ -d "${app_path}" ]] || die "App bundle not found: ${app_path}"
  [[ -d "${dext_path}" ]] || die "Bundled dext not found: ${dext_path}"
  codesign --verify --deep --strict "${app_path}" \
    || die "codesign verification failed: ${app_path}"
  has_entitlement "${app_path}" "com.apple.developer.system-extension.install" \
    || die "App is missing com.apple.developer.system-extension.install"
  has_entitlement "${dext_path}" "com.apple.developer.driverkit" \
    || die "Dext is missing com.apple.developer.driverkit"

  if ${ENABLE_SCSI}; then
    has_scsi_personality "${dext_path}" \
      || die "--scsi requested, but ASFWSCSIControllerService is absent"
    has_entitlement "${dext_path}" \
      "com.apple.developer.driverkit.family.scsicontroller" \
      || die "--scsi requested, but the SCSIController entitlement is absent"
  elif has_scsi_personality "${dext_path}" \
    || has_entitlement "${dext_path}" \
      "com.apple.developer.driverkit.family.scsicontroller"; then
    die "Build contains the SCSI HBA; rerun with --scsi to acknowledge it"
  fi
}

reject_test_host_artifacts() {
  local app_path="$1"
  local plugins_path="${app_path}/Contents/PlugIns"
  local test_bundle

  [[ -d "${plugins_path}" ]] || return 0
  test_bundle="$(
    find "${plugins_path}" -maxdepth 1 -type d \
      -name '*.xctest' -print -quit 2>/dev/null
  )"
  [[ -z "${test_bundle}" ]] || die \
    "Build product contains ${test_bundle}; rerun without --no-build after testing"
}

prune_old_app_backups() {
  local keep="${BACKUP_KEEP_COUNT}"
  local backups=()
  local candidate
  local index

  # Match the exact shape install_app_atomically writes (%Y%m%d-%H%M%S), not a
  # bare backup-* glob: an unrelated directory would sort above the timestamps
  # and take a slot that should have kept a real backup. Zero-padded timestamps
  # make a reverse name sort newest-first, and -maxdepth 1 keeps find from ever
  # descending into a bundle.
  local stamp_glob='[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]-[0-9][0-9][0-9][0-9][0-9][0-9]'
  while IFS= read -r candidate; do
    [[ -n "${candidate}" ]] || continue
    backups+=("${candidate}")
  done < <(
    find /Applications -maxdepth 1 -type d -name "ASFW.app.backup-${stamp_glob}" \
      -print 2>/dev/null | sort -r
  )

  (( ${#backups[@]} > keep )) || return 0

  for (( index = keep; index < ${#backups[@]}; index++ )); do
    candidate="${backups[index]}"
    # Re-check the shape immediately before removing. This function only ever
    # deletes paths it just enumerated out of /Applications, and only ones that
    # still match the backup name exactly.
    if [[ "${candidate}" != /Applications/ASFW.app.backup-* || ! -d "${candidate}" ]]; then
      continue
    fi
    log "Pruning old backup ${candidate}..."
    run_maybe_sudo rm -rf "${candidate}" \
      || log "Could not remove ${candidate}; leaving it in place."
  done
}

install_app_atomically() {
  local source_app="$1"
  local timestamp
  local staging
  local backup=""

  timestamp="$(date '+%Y%m%d-%H%M%S')"
  staging="/Applications/.ASFW.app.staging-${timestamp}"
  [[ ! -e "${staging}" ]] || die "Staging path already exists: ${staging}"

  log "Staging the new app at ${staging}..."
  run_maybe_sudo ditto "${source_app}" "${staging}" \
    || die "Failed to stage ASFW.app"
  run_maybe_sudo xattr -dr com.apple.quarantine "${staging}" \
    || die "Failed to clear quarantine from staged app"
  verify_artifact "${staging}"
  close_existing_app

  if [[ -e "${APP_DEST}" ]]; then
    backup="/Applications/ASFW.app.backup-${timestamp}"
    log "Backing up the current app to ${backup}..."
    run_maybe_sudo mv "${APP_DEST}" "${backup}" \
      || die "Failed to back up the existing ASFW.app"
  fi

  log "Activating the staged app at ${APP_DEST}..."
  if ! run_maybe_sudo mv "${staging}" "${APP_DEST}"; then
    if [[ -n "${backup}" && ! -e "${APP_DEST}" ]]; then
      run_maybe_sudo mv "${backup}" "${APP_DEST}" || true
    fi
    die "Failed to install ASFW.app"
  fi

  verify_artifact "${APP_DEST}"
  log "Installed dext hash: $(sha256_file "${APP_DEST}/${DEXT_BINARY_REL}")"
  [[ -z "${backup}" ]] || log "Previous app backup: ${backup}"

  # Only after the freshly installed app verifies. A failed install must keep
  # every backup it might have to roll back to.
  prune_old_app_backups
}

launch_installed_app() {
  if ${AUTO_ACTIVATE}; then
    log "Opening ${APP_DEST} and submitting the activation request..."
    open "${APP_DEST}" --args --activate-driver \
      || die "Failed to open ASFW.app for automatic activation"
  else
    log "Opening ${APP_DEST}..."
    open "${APP_DEST}" || die "Failed to open ASFW.app"
  fi
}

reopen_installed_app_for_debugging() {
  close_existing_app
  log "Reopening ${APP_DEST} for debugging..."
  open "${APP_DEST}" || die "Failed to reopen ASFW.app"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)
      [[ $# -ge 2 ]] || die "Missing value after --config"
      CONFIGURATION="$2"
      shift 2
      ;;
    --scsi) ENABLE_SCSI=true; shift ;;
    --no-build) DO_BUILD=false; shift ;;
    --fresh|--replace) FRESH_REPLACE=true; shift ;;
    --activate|--refresh) AUTO_ACTIVATE=true; shift ;;
    --no-launch) LAUNCH_APP=false; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "Unknown option: $1" ;;
  esac
done

case "${CONFIGURATION}" in
  Debug|Release) ;;
  *) die "--config must be Debug or Release" ;;
esac

if ${AUTO_ACTIVATE} && ! ${LAUNCH_APP}; then
  die "--activate cannot be combined with --no-launch"
fi

# Match the status line only. Under a Custom Configuration csrutil prints a
# per-protection breakdown, and a bare 'disabled' search matches any single
# disabled entry (e.g. "Apple Internal: disabled") on a machine where SIP is
# otherwise on — letting the install proceed and fail later in a confusing way.
csrutil status | head -n 1 | grep -qi 'status: disabled' \
  || die "SIP must be disabled for the README ad-hoc install workflow"
systemextensionsctl developer 2>&1 | grep -qi 'developer mode is on' \
  || die "Enable system-extension developer mode before installing"
ensure_replace_mode_for_pending_transition

if ${DO_BUILD}; then
  build_args=(--no-bump --config "${CONFIGURATION}")
  ${ENABLE_SCSI} && build_args+=(--scsi)
  log "Building ASFW.app (${CONFIGURATION})..."
  ./build.sh "${build_args[@]}"
fi

APP_SOURCE="${SCRIPT_DIR}/build/DerivedData/Build/Products/${CONFIGURATION}/ASFW.app"
[[ -d "${APP_SOURCE}" ]] || die "Build product not found: ${APP_SOURCE}"
reject_test_host_artifacts "${APP_SOURCE}"

log "Signing the app and its bundled dext..."
CONFIGURATION="${CONFIGURATION}" ./sign.sh "${APP_SOURCE}"
verify_artifact "${APP_SOURCE}"

${FRESH_REPLACE} && uninstall_existing_system_extension
install_app_atomically "${APP_SOURCE}"
EXPECTED_DEXT_HASH="$(sha256_file "${APP_DEST}/${DEXT_BINARY_REL}")"

if ${LAUNCH_APP}; then
  launch_installed_app
  if ${AUTO_ACTIVATE}; then
    activation_ok=true
    wait_for_active_dext_hash "${EXPECTED_DEXT_HASH}" || activation_ok=false
    # Reopen either way. The instance launched with --activate-driver stays on
    # the activation path and never connects its debug client, so returning
    # without this leaves the app running but disconnected — including when the
    # approval simply took longer than the wait above.
    reopen_installed_app_for_debugging
    ${activation_ok} \
      || die "Automatic activation did not produce the expected active dext"
  else
    log "Use the visible Install button to submit the extension replacement."
  fi
fi

systemextensionsctl list 2>/dev/null \
  | grep "${SYSTEM_EXTENSION_ID}" \
  || log "No active ASFW system-extension entry is visible yet."

if ${ENABLE_SCSI}; then
  log "SCSI HBA is present in the staged app. Keep an SBP-2 device powered on;"
  log "do not cold boot until runtime enumeration and teardown tests pass."
fi
