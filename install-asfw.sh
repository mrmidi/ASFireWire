#!/usr/bin/env bash
#
# Build, sign, and stage ASFW.app using the workflow documented in README.md.
# System-extension activation remains an explicit action in ASFW.app.

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

CONFIGURATION="Release"
DO_BUILD=true
ENABLE_SCSI=false
INSTALL_MCP_SKILL=false
LAUNCH_APP=true
FRESH_REPLACE=false

APP_DEST="/Applications/ASFW.app"
SYSTEM_EXTENSION_ID="net.mrmidi.ASFW.ASFWDriver"
DEXT_REL="Contents/Library/SystemExtensions/${SYSTEM_EXTENSION_ID}.dext"
DEXT_BINARY_REL="${DEXT_REL}/${SYSTEM_EXTENSION_ID}"
MCP_SKILL_SOURCE="${SCRIPT_DIR}/skills/asfw-mcp-control-plane"
CODEX_ROOT="${CODEX_HOME:-${HOME}/.codex}"

usage() {
  cat <<'EOF'
Usage: ./install-asfw.sh [options]

Options:
  --config Debug|Release  Build configuration (default: Release)
  --scsi                  Include the experimental SCSI HBA
  --no-build              Reuse an existing build product
  --install-mcp-skill     Install or refresh the bundled Codex MCP skill
  --fresh, --replace      Uninstall the active dext before staging the new app
  --no-launch             Do not open ASFW.app after installation
  -h, --help              Show this help

This script never prompts for a password. Run "sudo -v" first when an existing
app or dext requires administrator access.

Examples:
  ./install-asfw.sh
  ./install-asfw.sh --scsi --install-mcp-skill
  ./install-asfw.sh --config Debug --scsi --fresh
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

has_active_system_extension() {
  local extension_list
  extension_list="$(systemextensionsctl list 2>/dev/null)" || return 2
  grep -F "${SYSTEM_EXTENSION_ID}" <<<"${extension_list}" \
    | grep -qF '[activated enabled]'
}

uninstall_active_system_extension() {
  local query_status
  if has_active_system_extension; then
    :
  else
    query_status=$?
    [[ ${query_status} -eq 1 ]] \
      || die "Unable to query the current system-extension state"
    log "No active ASFW system extension needs to be uninstalled."
    return 0
  fi

  close_existing_app
  log "Uninstalling the active ASFW dext before replacement..."
  run_maybe_sudo systemextensionsctl uninstall - "${SYSTEM_EXTENSION_ID}" \
    || die "Failed to uninstall ${SYSTEM_EXTENSION_ID}"

  local attempts=30
  while (( attempts > 0 )); do
    if has_active_system_extension; then
      :
    else
      query_status=$?
      [[ ${query_status} -eq 1 ]] \
        || die "Unable to verify the system-extension uninstall"
      log "The previous ASFW dext is no longer active."
      return 0
    fi
    sleep 1
    ((attempts--))
  done
  die "The previous ASFW dext is still active; close its clients and retry"
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
}

install_mcp_skill() {
  [[ -f "${MCP_SKILL_SOURCE}/SKILL.md" ]] \
    || die "Bundled MCP skill not found: ${MCP_SKILL_SOURCE}"

  local skill_parent="${CODEX_ROOT}/skills"
  local skill_target="${skill_parent}/asfw-mcp-control-plane"
  local timestamp
  local backup=""
  timestamp="$(date '+%Y%m%d-%H%M%S')"

  mkdir -p "${skill_parent}" \
    || die "Failed to create Codex skills directory: ${skill_parent}"
  if [[ -e "${skill_target}" ]]; then
    backup="${skill_target}.backup-${timestamp}"
    mv "${skill_target}" "${backup}" \
      || die "Failed to back up the existing Codex MCP skill"
  fi

  if ! cp -R "${MCP_SKILL_SOURCE}" "${skill_target}" \
    || ! diff -qr "${MCP_SKILL_SOURCE}" "${skill_target}" >/dev/null; then
    [[ ! -e "${skill_target}" ]] \
      || mv "${skill_target}" "${skill_target}.failed-${timestamp}"
    [[ -z "${backup}" ]] || mv "${backup}" "${skill_target}"
    die "Failed to install the Codex MCP skill"
  fi
  log "Installed MCP skill: ${skill_target}"
  [[ -z "${backup}" ]] || log "Previous MCP skill backup: ${backup}"
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
    --install-mcp-skill) INSTALL_MCP_SKILL=true; shift ;;
    --fresh|--replace) FRESH_REPLACE=true; shift ;;
    --no-launch) LAUNCH_APP=false; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "Unknown option: $1" ;;
  esac
done

case "${CONFIGURATION}" in
  Debug|Release) ;;
  *) die "--config must be Debug or Release" ;;
esac

csrutil status | grep -qi 'disabled' \
  || die "SIP must be disabled for the README ad-hoc install workflow"
systemextensionsctl developer 2>&1 | grep -qi 'developer mode is on' \
  || die "Enable system-extension developer mode before installing"

if ${DO_BUILD}; then
  build_args=(--no-bump --config "${CONFIGURATION}")
  ${ENABLE_SCSI} && build_args+=(--scsi)
  log "Building ASFW.app (${CONFIGURATION})..."
  ./build.sh "${build_args[@]}"
fi

APP_SOURCE="${SCRIPT_DIR}/build/DerivedData/Build/Products/${CONFIGURATION}/ASFW.app"
[[ -d "${APP_SOURCE}" ]] || die "Build product not found: ${APP_SOURCE}"

log "Signing the app and its bundled dext..."
CONFIGURATION="${CONFIGURATION}" ./sign.sh "${APP_SOURCE}"
verify_artifact "${APP_SOURCE}"

${INSTALL_MCP_SKILL} && install_mcp_skill
${FRESH_REPLACE} && uninstall_active_system_extension
install_app_atomically "${APP_SOURCE}"

if ${LAUNCH_APP}; then
  log "Opening ${APP_DEST}..."
  open "${APP_DEST}"
  log "Use the visible Install button to submit the extension replacement."
fi

systemextensionsctl list 2>/dev/null \
  | grep "${SYSTEM_EXTENSION_ID}" \
  || log "No active ASFW system-extension entry is visible yet."

if ${ENABLE_SCSI}; then
  log "SCSI HBA is present in the staged app. Keep an SBP-2 device powered on;"
  log "do not cold boot until runtime enumeration and teardown tests pass."
fi
