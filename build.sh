#!/usr/bin/env bash
#
# ASFireWire – Quiet Build Script
# Prints only ERRORS and WARNINGS with a clean summary.
# Exit status mirrors xcodebuild; nonzero if build failed or errors detected.

set -Eeuo pipefail

# -------- Config --------
PROJECT_NAME="ASFireWire"
SCHEME_NAME="ASFireWire"
CONFIGURATION="${CONFIGURATION:-Release}"
BUILD_DIR="${BUILD_DIR:-./build}"
DERIVED="${BUILD_DIR}/DerivedData"
LOG_DIR="${BUILD_DIR}/logs"
RAW_LOG="${LOG_DIR}/xcodebuild.raw.log"
ERR_LOG="${LOG_DIR}/errors.log"
WRN_LOG="${LOG_DIR}/warnings.log"
RESULT_BUNDLE="${BUILD_DIR}/Result.xcresult"

# Colors (TTY only)
if [ -t 1 ]; then
  RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'; BLUE=$'\033[0;34m'; NC=$'\033[0m'
else
  RED=""; GREEN=""; YELLOW=""; BLUE=""; NC=""
fi

VERBOSE=false
NO_BUMP=false

usage() {
  cat <<EOF
Usage: $0 [--verbose] [--no-bump] [--scheme NAME] [--config CONFIG] [--derived PATH]
  --verbose          Show full xcodebuild output (disables quiet filtering)
  --no-bump          Skip ./bump.sh
  --scheme NAME      Override scheme (default: ${SCHEME_NAME})
  --config CONFIG    Override configuration (default: ${CONFIGURATION})
  --derived PATH     Set DerivedData path (default: ${DERIVED})
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --verbose) VERBOSE=true; shift;;
    --no-bump) NO_BUMP=true; shift;;
    --scheme) SCHEME_NAME="$2"; shift 2;;
    --config) CONFIGURATION="$2"; shift 2;;
    --derived) DERIVED="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "${RED}[ERROR]${NC} Unknown arg: $1"; usage; exit 1;;
  esac
done

log() { echo "${BLUE}[INFO]${NC} $*"; }
ok()  { echo "${GREEN}[OK]${NC} $*"; }
warn(){ echo "${YELLOW}[WARN]${NC} $*"; }
err() { echo "${RED}[ERROR]${NC} $*"; }

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || { err "Missing required command: $1"; exit 127; }
}

preflight() {
  require_cmd xcodebuild
  [[ -f "${PROJECT_NAME}.xcodeproj/project.pbxproj" ]] || { err "Run from project root (missing ${PROJECT_NAME}.xcodeproj)"; exit 1; }
  mkdir -p "$BUILD_DIR" "$DERIVED" "$LOG_DIR"
}

bump_version() {
  $NO_BUMP && { warn "Skipping version bump (--no-bump)"; return; }
  if [[ -x "./bump.sh" ]]; then
    log "Bumping version…"
    ./bump.sh >/dev/null && ok "Version bumped"
  else
    warn "bump.sh missing or not executable – skipping"
  fi
}

# Strip ANSI (in case tool adds color)
strip_ansi() { sed -E 's/\x1B\[[0-9;]*[mK]//g'; }

# Grep helpers tuned for clang/xcodebuild
extract_errors() {
  # Match typical clang/linker failures & xcodebuild errors, avoid notes
  grep -iE '(^|[[:space:]])error:|^fatal error:|clang: error| ld: (warning|error)|\bfailed with exit code' \
    | grep -viE 'note:' || true
}

extract_warnings() {
  # Classic warnings, plus ld warnings; avoid noisy "note:"
  grep -iE '(^|[[:space:]])warning:' \
    | grep -viE 'note:' || true
}

run_build() {
  log "Building ${PROJECT_NAME} (scheme=${SCHEME_NAME}, config=${CONFIGURATION})…"
  local QUIET_FLAG=()
  $VERBOSE || QUIET_FLAG=(-quiet)

  # Run xcodebuild. We capture everything to RAW_LOG.
  set +e
  xcodebuild \
    -project "${PROJECT_NAME}.xcodeproj" \
    -scheme "${SCHEME_NAME}" \
    -configuration "${CONFIGURATION}" \
    -derivedDataPath "${DERIVED}" \
    -resultBundlePath "${RESULT_BUNDLE}" \
    "${QUIET_FLAG[@]}" \
    build \
    2>&1 | tee "${RAW_LOG}"
  local xb_status=${PIPESTATUS[0]}
  set -e
  return $xb_status
}

summarize() {
  # Clean logs
  local cleaned
  cleaned="$(cat "${RAW_LOG}" | strip_ansi)"

  # Split into errors/warnings
  echo "$cleaned" | extract_errors   > "${ERR_LOG}"
  echo "$cleaned" | extract_warnings > "${WRN_LOG}"

  local err_count wrn_count
  err_count=$(wc -l < "${ERR_LOG}" | tr -d ' ')
  wrn_count=$(wc -l < "${WRN_LOG}" | tr -d ' ')

  # Compact multi-line diagnostics by showing the first line plus the following indented lines.
  # For simplicity, just show the first 25 errors and 25 warnings verbatim (already fairly quiet with -quiet).
  echo
  if (( err_count > 0 )); then
    echo "${RED}=========== ERRORS (${err_count}) ===========${NC}"
    awk 'NR<=200{print}' "${ERR_LOG}"   # ~25 typical clang errors (multi-line) ~200 lines cap
    (( err_count > 200 )) && echo "… and more (truncated)"
    echo
  fi
  if (( wrn_count > 0 )); then
    echo "${YELLOW}=========== WARNINGS (${wrn_count}) ===========${NC}"
    awk 'NR<=200{print}' "${WRN_LOG}"
    (( wrn_count > 200 )) && echo "… and more (truncated)"
    echo
  fi

  if (( err_count == 0 && wrn_count == 0 )); then
    ok "No warnings or errors reported."
  else
    [[ $wrn_count -gt 0 ]] && warn "${wrn_count} warning(s)"
    [[ $err_count -gt 0 ]] && err  "${err_count} error(s)"
  fi
}

main() {
  echo "==============================="
  echo "  ASFireWire – Quiet Build"
  echo "==============================="

  preflight
  bump_version

  # Clean previous logs/bundle (keep Derived to speed up builds)
  rm -rf "${RESULT_BUNDLE}" "${RAW_LOG}" "${ERR_LOG}" "${WRN_LOG}"

  if run_build; then
    summarize
    echo
    ok "Build Succeeded"
    exit 0
  else
    summarize
    echo
    err "Build Failed"
    # Mirror failure to shell; CI can rely on this
    exit 1
  fi
}

main "$@"