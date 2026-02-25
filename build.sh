#!/usr/bin/env bash
#
# ASFireWire – Quiet Build Script
# Prints only ERRORS and WARNINGS with a clean summary.
# Exit status mirrors xcodebuild; nonzero if build failed or errors detected.

set -Eeuo pipefail

# -------- Config --------
PROJECT_NAME="ASFW"
SCHEME_NAME="ASFW"
CONFIGURATION="${CONFIGURATION:-Debug}"
ARCH_NAME="${ARCH_NAME:-arm64}"
BUILD_DIR="${BUILD_DIR:-./build}"
DERIVED="${BUILD_DIR}/DerivedData"
LOG_DIR="${BUILD_DIR}/logs"
RAW_LOG="${LOG_DIR}/xcodebuild.raw.log"
ERR_LOG="${LOG_DIR}/errors.log"
WRN_LOG="${LOG_DIR}/warnings.log"
RESULT_BUNDLE="${BUILD_DIR}/Result.xcresult"

# Colors (TTY only)
if [[ -t 1 ]]; then
  RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'; BLUE=$'\033[0;34m'; NC=$'\033[0m'
else
  RED=""; GREEN=""; YELLOW=""; BLUE=""; NC=""
fi

VERBOSE=false
NO_BUMP=false
RUN_TESTS=false
TEST_ONLY=false
# When true, generate `compile_commands.json` by piping xcodebuild to xcpretty
GENERATE_COMMANDS=false
# Path to write compile_commands.json (relative to script working dir)
COMPILE_COMMANDS_PATH="${COMPILE_COMMANDS_PATH:-./compile_commands.json}"
# Optional: pattern to pass to ctest (-R) to select tests. Empty means "all discovered tests".
SELECTED_TESTS_PATTERN=""
# When true, run PVS-Studio static analyzer after successful build
RUN_ANALYZER=false
PVS_LOG="${BUILD_DIR}/PVS-Studio.log"
PVS_JSON="${BUILD_DIR}/PVS-Studio.json"
# When true, run only Swift/XCTest tests
SWIFT_TEST_ONLY=false
# When true, generate Swift code coverage
SWIFT_COVERAGE=false
SWIFT_COVERAGE_LCOV="${BUILD_DIR}/swift_coverage.lcov"

usage() {
  cat <<EOF
Usage: $0 [--verbose] [--no-bump] [--scheme NAME] [--config CONFIG] [--arch ARCH] [--derived PATH]
  --verbose          Show full xcodebuild output (disables quiet filtering)
  --no-bump          Skip ./bump.sh
  --test             Run C++ tests before building
  --test-only        Run C++ tests only (skip xcodebuild)
  --swift-test-only  Run Swift/XCTest tests only (skip main build)
  --swift-coverage   Run Swift tests with coverage and export to LCOV
  --commands         Generate compile_commands.json via xcpretty
  --analyze          Run PVS-Studio static analyzer after build
  --scheme NAME      Override scheme (default: ${SCHEME_NAME})
  --config CONFIG    Override configuration (default: ${CONFIGURATION})
  --arch ARCH        Override architecture passed to xcodebuild (default: ${ARCH_NAME})
  --derived PATH     Set DerivedData path (default: ${DERIVED})
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --verbose) VERBOSE=true; shift;;
    --no-bump) NO_BUMP=true; shift;;
    --test) RUN_TESTS=true; shift;;
    --test-only) TEST_ONLY=true; shift;;
    --swift-test-only) SWIFT_TEST_ONLY=true; shift;;
    --swift-coverage) SWIFT_COVERAGE=true; shift;;
    --test-filter) SELECTED_TESTS_PATTERN="$2"; shift 2;;
    --commands) GENERATE_COMMANDS=true; shift;;
    --analyze) RUN_ANALYZER=true; shift;;
    --scheme) SCHEME_NAME="$2"; shift 2;;
    --config) CONFIGURATION="$2"; shift 2;;
    --arch) ARCH_NAME="$2"; shift 2;;
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
  # When running test-only we don't need xcodebuild or the Xcode project present.
  if ! $TEST_ONLY; then
    require_cmd xcodebuild
    [[ -f "${PROJECT_NAME}.xcodeproj/project.pbxproj" ]] || { err "Run from project root (missing ${PROJECT_NAME}.xcodeproj)"; exit 1; }
  fi
  if $GENERATE_COMMANDS; then
    require_cmd xcpretty
  fi
  if $RUN_ANALYZER; then
    require_cmd pvs-studio-analyzer
    require_cmd plog-converter
  fi
  mkdir -p "$BUILD_DIR" "$DERIVED" "$LOG_DIR"
}

# Generate compile_commands.json by piping xcodebuild output to xcpretty.
generate_compile_commands() {
  log "Generating compile_commands.json at ${COMPILE_COMMANDS_PATH}..."

  # Build arguments similar to run_build()
  # Use a clean build to ensure compile steps are emitted (avoids empty DB when nothing is rebuilt)
  local XCODEBUILD_ARGS=(
    -project "${PROJECT_NAME}.xcodeproj"
    -scheme "${SCHEME_NAME}"
    -configuration "${CONFIGURATION}"
    -derivedDataPath "${DERIVED}"
    -destination "platform=macOS,arch=${ARCH_NAME}"
    clean
    build
  )

  # Run xcodebuild and pipe to xcpretty. Capture xcodebuild exit status via PIPESTATUS.
  set +e
  xcodebuild "${XCODEBUILD_ARGS[@]}" 2>&1 | xcpretty -r json-compilation-database --output "${COMPILE_COMMANDS_PATH}"
  local xb_status=${PIPESTATUS[0]}
  set -e

  if (( xb_status == 0 )) && [[ -f "${COMPILE_COMMANDS_PATH}" ]]; then
    ok "Generated ${COMPILE_COMMANDS_PATH}"
    return 0
  else
    err "Failed to generate compile_commands.json (xcodebuild exit=${xb_status})"
    return ${xb_status}
  fi
}

# Run selected CMake/CTest tests. Returns non-zero on failure.
run_tests() {
  # Resolve script directory so tests path works when running from repo root
  local SCRIPT_DIR
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  # Test build/run directory (tests live next to this script in the ASFW folder)
  local TESTS_DIR="${SCRIPT_DIR}/tests"
  local TEST_BUILD_DIR="${BUILD_DIR}/tests_build"

  if [[ ! -d "${TESTS_DIR}" ]]; then
    err "Tests directory not found: ${TESTS_DIR} (script dir: ${SCRIPT_DIR})"; return 2
  fi

  require_cmd cmake
  require_cmd ctest

  log "Configuring tests (cmake) in ${TEST_BUILD_DIR}..."
  cmake -S "${TESTS_DIR}" -B "${TEST_BUILD_DIR}" -DCMAKE_BUILD_TYPE="${CONFIGURATION}" >/dev/null

  log "Building tests..."
  # Use cmake --build for portability; forward config for multi-config generators
  cmake --build "${TEST_BUILD_DIR}" --config "${CONFIGURATION}" -- -j$(sysctl -n hw.ncpu) >/dev/null

  log "Running tests (verbose output)..."
  # If user supplied a filter pattern, pass -R to ctest. Use -V for verbose output of all tests.
  if [[ -n "${SELECTED_TESTS_PATTERN}" ]]; then
    ctest --test-dir "${TEST_BUILD_DIR}" -V -R "${SELECTED_TESTS_PATTERN}"
  else
    ctest --test-dir "${TEST_BUILD_DIR}" -V
  fi
}

# Run Swift/XCTest tests via xcodebuild. Returns non-zero on failure.
run_swift_tests() {
  local with_coverage=${1:-false}
  
  if $with_coverage; then
    log "Running Swift/XCTest tests with coverage..."
  else
    log "Running Swift/XCTest tests..."
  fi
  
  # Use -only-testing to avoid launching the full app
  local XCODEBUILD_ARGS=(
    -project "${PROJECT_NAME}.xcodeproj"
    -scheme "${SCHEME_NAME}"
    -configuration "${CONFIGURATION}"
    -derivedDataPath "${DERIVED}"
    -destination "platform=macOS,arch=${ARCH_NAME}"
    -only-testing:ASFWTests
  )
  
  # Add coverage flag if requested
  if $with_coverage; then
    XCODEBUILD_ARGS+=(-enableCodeCoverage YES)
  fi
  
  XCODEBUILD_ARGS+=(test)
  
  set +e
  if $VERBOSE; then
    xcodebuild "${XCODEBUILD_ARGS[@]}" 2>&1
  else
    xcodebuild "${XCODEBUILD_ARGS[@]}" 2>&1 | grep -E '(Test Case|passed|failed|error:)' || true
  fi
  local test_status=${PIPESTATUS[0]}
  set -e
  
  return $test_status
}

# Export Swift coverage to LCOV format for SonarCloud.
export_swift_coverage() {
  log "Exporting Swift coverage to LCOV format..."
  
  # Find the Coverage.profdata from xcodebuild
  local PROFDATA
  PROFDATA=$(find "${DERIVED}" -name 'Coverage.profdata' 2>/dev/null | head -1)
  
  if [[ -z "$PROFDATA" ]]; then
    warn "No Coverage.profdata found - Swift coverage will be empty"
    touch "${SWIFT_COVERAGE_LCOV}"
    return 1
  fi
  
  # Find the main app binary for coverage export
  local APP_BINARY
  APP_BINARY=$(find "${DERIVED}" -name 'ASFW' -type f -perm +111 2>/dev/null | grep -v '.dSYM' | head -1)
  
  if [[ -z "$APP_BINARY" ]]; then
    warn "Could not find app binary for Swift coverage export"
    touch "${SWIFT_COVERAGE_LCOV}"
    return 1
  fi
  
  set +e
  xcrun llvm-cov export \
    -format=lcov \
    -instr-profile="$PROFDATA" \
    "$APP_BINARY" \
    --ignore-filename-regex='.*/DerivedData/.*' \
    > "${SWIFT_COVERAGE_LCOV}"
  local cov_status=$?
  set -e
  
  if (( cov_status == 0 )); then
    ok "Swift coverage exported to ${SWIFT_COVERAGE_LCOV}"
    return 0
  else
    err "Failed to export Swift coverage (exit=${cov_status})"
    return $cov_status
  fi
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
	    -destination "platform=macOS,arch=${ARCH_NAME}" \
	    -resultBundlePath "${RESULT_BUNDLE}" \
	    CODE_SIGNING_ALLOWED=NO \
	    CODE_SIGNING_REQUIRED=NO \
	    CODE_SIGN_IDENTITY="" \
    ${QUIET_FLAG[@]+"${QUIET_FLAG[@]}"} \
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

  return 0
}

# Run PVS-Studio static analyzer on the project.
# Requires compile_commands.json to be present.
run_static_analysis() {
  log "Running PVS-Studio static analyzer..."

  if [[ ! -f "${COMPILE_COMMANDS_PATH}" ]]; then
    err "compile_commands.json not found at ${COMPILE_COMMANDS_PATH}. Generate it first with --commands."
    return 1
  fi

  # Ensure pvs-studio-analyzer can find compile_commands.json
  local analysis_dir
  analysis_dir="$(dirname "${COMPILE_COMMANDS_PATH}")"
  
  # Run analyzer with compile_commands.json in the current directory or specified path
  # -o specifies output log path
  # -j uses multiple cores (default to system CPU count)
  local jobs
  jobs=$(sysctl -n hw.ncpu)
  
  set +e
  pvs-studio-analyzer analyze \
    -f "${COMPILE_COMMANDS_PATH}" \
    -o "${PVS_LOG}" \
    -j "${jobs}"
  local pvs_status=$?
  set -e

  if (( pvs_status == 0 )); then
    ok "Static analysis complete. Log: ${PVS_LOG}"
    
    # Convert to JSON format
    log "Converting analysis results to JSON..."
    set +e
    plog-converter -a GA:1,2,3 \
                   -t json \
                   -o "${PVS_JSON}" \
                   "${PVS_LOG}"
    local convert_status=$?
    set -e
    
    if (( convert_status == 0 )); then
      ok "Analysis results converted to JSON: ${PVS_JSON}"
      return 0
    else
      warn "Failed to convert to JSON (exit=${convert_status}), but raw log is available"
      return 0
    fi
  else
    err "Static analysis failed (exit=${pvs_status})"
    return ${pvs_status}
  fi
}

main() {
  echo "==============================="
  echo "  ASFireWire – Quiet Build"
  echo "==============================="

  preflight
  bump_version

  # If --commands was requested, generate compile_commands.json and exit.
  if $GENERATE_COMMANDS; then
    if generate_compile_commands; then
      ok "compile_commands.json generated successfully."
      exit 0
    else
      err "Failed to generate compile_commands.json."; exit 1
    fi
  fi

  # If --test-only was requested, run tests and exit according to their result.
  if $TEST_ONLY; then
    run_tests
    local test_status=$?
    if (( test_status == 0 )); then
      ok "Tests passed (test-only)."
      exit 0
    else
      err "Tests failed (test-only)."; exit $test_status
    fi
  fi

  # If --swift-test-only was requested, run Swift/XCTest tests and exit.
  if $SWIFT_TEST_ONLY; then
    run_swift_tests false
    local test_status=$?
    if (( test_status == 0 )); then
      ok "Swift tests passed."
      exit 0
    else
      err "Swift tests failed."; exit $test_status
    fi
  fi

  # If --swift-coverage was requested, run Swift tests with coverage and export.
  if $SWIFT_COVERAGE; then
    run_swift_tests true
    local test_status=$?
    if (( test_status == 0 )); then
      ok "Swift tests passed."
      export_swift_coverage
      exit 0
    else
      err "Swift tests failed."; exit $test_status
    fi
  fi

  # If --test was requested, run tests before doing the xcodebuild. If they fail, abort.
  if $RUN_TESTS; then
    run_tests
    local test_status=$?
    if (( test_status != 0 )); then
      err "Pre-build tests failed; aborting xcodebuild."; exit $test_status
    else
      ok "Pre-build tests passed. Proceeding to xcodebuild."
    fi
  fi

  # Clean previous logs/bundle (keep Derived to speed up builds)
  rm -rf "${RESULT_BUNDLE}" "${RAW_LOG}" "${ERR_LOG}" "${WRN_LOG}"

  if run_build; then
    summarize
    echo
    ok "Build Succeeded"
    
    # If --analyze was requested, run static analysis after successful build
    if $RUN_ANALYZER; then
      # Ensure compile_commands.json exists (generate if needed)
      if [[ ! -f "${COMPILE_COMMANDS_PATH}" ]]; then
        log "compile_commands.json not found. Generating it for static analysis..."
        if ! generate_compile_commands; then
          err "Failed to generate compile_commands.json for analysis."
          exit 1
        fi
      fi
      
      if run_static_analysis; then
        ok "Static analysis completed successfully"
      else
        err "Static analysis failed"
        exit 1
      fi
    fi
    
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
