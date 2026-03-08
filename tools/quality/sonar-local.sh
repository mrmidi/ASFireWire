#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SETTINGS="${ROOT}/sonar-project.properties"
REGEN_COMMANDS=false
WAIT_FOR_GATE=true
BRANCH_NAME=""
EXTRA_SCANNER_ARGS=()

usage() {
  cat <<'EOF'
Usage: tools/quality/sonar-local.sh [options] [-- <extra sonar-scanner args>]

Options:
  --regen-commands    Regenerate compile_commands.json via ./build.sh --commands --no-bump
  --no-wait           Do not wait for quality gate status
  --branch NAME       Override sonar.branch.name (default: current git branch)
  --settings PATH     Use an alternate sonar-project.properties file
  -h, --help          Show this help

Environment:
  SONAR_TOKEN         Preferred SonarCloud token
  SONARQUBE_TOKEN     Accepted fallback; exported to SONAR_TOKEN for the scan
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --regen-commands)
      REGEN_COMMANDS=true
      shift
      ;;
    --no-wait)
      WAIT_FOR_GATE=false
      shift
      ;;
    --branch)
      BRANCH_NAME="$2"
      shift 2
      ;;
    --settings)
      if [[ "$2" != /* ]]; then
        SETTINGS="${ROOT}/$2"
      else
        SETTINGS="$2"
      fi
      shift 2
      ;;
    --)
      shift
      EXTRA_SCANNER_ARGS+=("$@")
      break
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Missing required command: $1" >&2
    exit 127
  }
}

has_nonempty_compile_db() {
  local db="$1"
  [[ -f "$db" ]] || return 1
  python3 - "$db" <<'PY'
import json, sys
path = sys.argv[1]
try:
    with open(path) as f:
        data = json.load(f)
    sys.exit(0 if isinstance(data, list) and len(data) > 0 else 1)
except Exception:
    sys.exit(1)
PY
}

require_cmd sonar-scanner
require_cmd python3

if [[ ! -f "${SETTINGS}" ]]; then
  echo "Sonar settings file not found: ${SETTINGS}" >&2
  exit 2
fi

token="${SONAR_TOKEN:-${SONARQUBE_TOKEN:-}}"
if [[ -z "${token}" ]]; then
  echo "Set SONAR_TOKEN or SONARQUBE_TOKEN before running sonar-local.sh." >&2
  exit 2
fi
export SONAR_TOKEN="${token}"

compile_db="${ROOT}/compile_commands.json"
if $REGEN_COMMANDS || ! has_nonempty_compile_db "${compile_db}"; then
  echo "Generating compile_commands.json via ./build.sh --commands --no-bump..."
  (cd "${ROOT}" && ./build.sh --commands --no-bump)
fi

if ! has_nonempty_compile_db "${compile_db}"; then
  echo "compile_commands.json is missing or empty: ${compile_db}" >&2
  exit 2
fi

if [[ -z "${BRANCH_NAME}" ]]; then
  BRANCH_NAME="$(git -C "${ROOT}" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
fi

scanner_args=("-Dproject.settings=${SETTINGS}")
if [[ -n "${BRANCH_NAME}" && "${BRANCH_NAME}" != "HEAD" ]]; then
  scanner_args+=("-Dsonar.branch.name=${BRANCH_NAME}")
fi
if $WAIT_FOR_GATE; then
  scanner_args+=("-Dsonar.qualitygate.wait=true")
fi

echo "Running sonar-scanner from ${ROOT}"
if [[ -n "${BRANCH_NAME}" && "${BRANCH_NAME}" != "HEAD" ]]; then
  echo "Branch: ${BRANCH_NAME}"
fi
echo "Settings: ${SETTINGS}"

cd "${ROOT}"
exec sonar-scanner "${scanner_args[@]}" "${EXTRA_SCANNER_ARGS[@]}"
