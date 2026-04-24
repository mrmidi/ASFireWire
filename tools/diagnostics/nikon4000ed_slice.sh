#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

MCS_ROOT="${MCS_ROOT:-${REPO_ROOT}/../moderncoolscan}"
OUT_DIR=""
NODE=""
SKIP_LOGIN=0
NO_BUILD=0
BUILT_ONCE=0
NIKON_GUID_RE="0x0090b54001ffffff"

usage() {
  cat <<'USAGE'
Usage:
  tools/diagnostics/nikon4000ed_slice.sh [options]

Options:
  --node <id>         Target node. If omitted, the script searches for Nikon 4000ED GUID.
  --mcs-root <path>   Path to moderncoolscan. Default: ../moderncoolscan
  --out-dir <path>    Output directory. Default: build/diagnostics/nikon4000ed-<timestamp>
  --skip-login        Skip SBP-2 login/inquiry commands.
  --no-build          Reuse an existing mcs-cli binary; do not build on the first command.
  -h, --help          Show this help.

The script captures a reproducible Nikon 4000ED bring-up slice through moderncoolscan's
signed mcs-cli runner plus ASFW unified logs. It is safe to run without the scanner;
failed commands are recorded and the script continues where possible.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --node)
      if [[ $# -lt 2 || -z "${2:-}" ]]; then
        echo "error: --node requires a value" >&2
        exit 2
      fi
      NODE="${2:-}"
      shift 2
      ;;
    --mcs-root)
      if [[ $# -lt 2 || -z "${2:-}" ]]; then
        echo "error: --mcs-root requires a value" >&2
        exit 2
      fi
      MCS_ROOT="${2:-}"
      shift 2
      ;;
    --out-dir)
      if [[ $# -lt 2 || -z "${2:-}" ]]; then
        echo "error: --out-dir requires a value" >&2
        exit 2
      fi
      OUT_DIR="${2:-}"
      shift 2
      ;;
    --skip-login)
      SKIP_LOGIN=1
      shift
      ;;
    --no-build)
      NO_BUILD=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "${OUT_DIR}" ]]; then
  OUT_DIR="${REPO_ROOT}/build/diagnostics/nikon4000ed-$(date '+%Y%m%d-%H%M%S')"
fi

RUNNER="${MCS_ROOT}/tools/macos/sign-and-run-cli.sh"
mkdir -p "${OUT_DIR}"

TRANSCRIPT="${OUT_DIR}/transcript.txt"
SUMMARY="${OUT_DIR}/summary.txt"

log() {
  printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "${TRANSCRIPT}"
}

write_summary() {
  printf '%s\n' "$*" >>"${SUMMARY}"
}

if [[ ! -x "${RUNNER}" ]]; then
  log "error: mcs runner not found or not executable: ${RUNNER}"
  log "hint: pass --mcs-root /path/to/moderncoolscan"
  exit 1
fi

run_mcs() {
  local name="$1"
  shift
  local outfile="${OUT_DIR}/${name}.txt"
  local -a prefix=()

  if [[ "${NO_BUILD}" -eq 1 || "${BUILT_ONCE}" -eq 1 ]]; then
    prefix=(--no-build)
  fi

  log "RUN mcs $* -> ${outfile}"
  {
    printf '$ %q' "${RUNNER}"
    for arg in "${prefix[@]}" -- "$@"; do
      printf ' %q' "$arg"
    done
    printf '\n\n'
  } >"${outfile}"

  set +e
  (cd "${MCS_ROOT}" && "${RUNNER}" "${prefix[@]}" -- "$@") >>"${outfile}" 2>&1
  local status=$?
  set +e

  BUILT_ONCE=1
  log "EXIT ${status}: mcs $*"
  return "${status}"
}

run_shell() {
  local name="$1"
  shift
  local outfile="${OUT_DIR}/${name}.txt"
  log "RUN $* -> ${outfile}"
  {
    printf '$'
    printf ' %q' "$@"
    printf '\n\n'
  } >"${outfile}"

  set +e
  "$@" >>"${outfile}" 2>&1
  local status=$?
  set +e

  log "EXIT ${status}: $*"
  return "${status}"
}

extract_nikon_node() {
  local list_file="$1"
  sed -nE "s/^node=([0-9]+) guid=${NIKON_GUID_RE}.*/\\1/p" "${list_file}" | head -n 1
}

collect_unified_log() {
  local minutes="${1:-20}"
  local predicate='eventMessage CONTAINS "SBP2" OR eventMessage CONTAINS "AddressSpaceManager" OR eventMessage CONTAINS "ROMScanSession" OR eventMessage CONTAINS "BusReset" OR eventMessage CONTAINS "cycleMaster"'
  run_shell "asfw-log-last-${minutes}m" /usr/bin/log show --last "${minutes}m" --style syslog --predicate "${predicate}"
}

log "Nikon 4000ED diagnostic slice"
log "ASFireWire=${REPO_ROOT}"
log "moderncoolscan=${MCS_ROOT}"
log "out=${OUT_DIR}"

write_summary "Nikon 4000ED diagnostic slice"
write_summary "Captured: $(date '+%Y-%m-%d %H:%M:%S %z')"
write_summary "ASFireWire: ${REPO_ROOT}"
write_summary "moderncoolscan: ${MCS_ROOT}"
write_summary ""

run_mcs "00-list" list || true
run_mcs "01-bus-state-before" diag bus-state || true
run_mcs "02-raw-discovery" diag raw-discovery || true
run_mcs "03-raw-topology" diag raw-topology || true

if [[ -z "${NODE}" ]]; then
  NODE="$(extract_nikon_node "${OUT_DIR}/00-list.txt" || true)"
fi

if [[ -z "${NODE}" ]]; then
  log "Nikon 4000ED GUID ${NIKON_GUID_RE} not found; skipping node-scoped probes."
  write_summary "Target node: not found"
  write_summary ""
  write_summary "Expected next connected-device signal: list output contains guid=${NIKON_GUID_RE}."
  collect_unified_log 20 || true
  log "Slice written to ${OUT_DIR}"
  exit 0
fi

log "Target node=${NODE}"
write_summary "Target node: ${NODE}"
write_summary "Expected GUID: ${NIKON_GUID_RE}"
write_summary ""

run_mcs "04-info-verbose" info --node "${NODE}" --verbose || true
run_mcs "05-raw-rom-trigger" diag raw-rom --node "${NODE}" --trigger || true
run_mcs "06-raw-csr" diag raw-csr --node "${NODE}" || true

run_mcs "07-read-csr-state-clear-4" tx read --node "${NODE}" --addr 0xf0000000 --len 4 || true
run_mcs "08-read-csr-config-rom-4" tx read --node "${NODE}" --addr 0xf0000400 --len 4 || true
run_mcs "09-read-csr-config-rom-8" tx read --node "${NODE}" --addr 0xf0000400 --len 8 || true
run_mcs "10-read-mgmt-agent-quadlet-shifted" tx read --node "${NODE}" --addr 0xf0030000 --len 4 || true
run_mcs "11-read-mgmt-agent-block-shifted" tx read --node "${NODE}" --addr 0xf0030000 --len 8 || true
run_mcs "12-read-mgmt-agent-byte-offset" tx read --node "${NODE}" --addr 0xf000c000 --len 4 || true

run_mcs "13-sbp2-probe" sbp2 probe --node "${NODE}" || true

if [[ "${SKIP_LOGIN}" -eq 0 ]]; then
  run_mcs "14-sbp2-login" sbp2 login --node "${NODE}" || true
  run_mcs "15-sbp2-inquiry" sbp2 inquiry --node "${NODE}" || true
else
  log "Skipping SBP-2 login/inquiry by request."
fi

run_mcs "16-bus-state-after" diag bus-state || true
collect_unified_log 30 || true

write_summary "Key files:"
write_summary "- ${OUT_DIR}/04-info-verbose.txt"
write_summary "- ${OUT_DIR}/05-raw-rom-trigger.txt"
write_summary "- ${OUT_DIR}/09-read-csr-config-rom-8.txt"
write_summary "- ${OUT_DIR}/11-read-mgmt-agent-block-shifted.txt"
write_summary "- ${OUT_DIR}/13-sbp2-probe.txt"
write_summary "- ${OUT_DIR}/asfw-log-last-30m.txt"
write_summary ""
write_summary "Primary questions for the next connected run:"
write_summary "1. Does Config ROM still parse as 136 bytes with ManagementAgent csr_offset=0x00c000?"
write_summary "2. Does 0xf0000400 len=4 succeed while len=8 fails?"
write_summary "3. Does 0xf0030000 change from status/type/address error to a readable management agent?"
write_summary "4. During login, do ASFW logs show remote read label=sbp2-login-orb?"
write_summary "5. During login, do ASFW logs show remote write label=sbp2-login-response or label=sbp2-status-fifo?"

log "Slice written to ${OUT_DIR}"
