#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

find_clang_tidy() {
  if command -v clang-tidy >/dev/null 2>&1; then
    command -v clang-tidy
    return 0
  fi
  if [[ -x /opt/homebrew/opt/llvm/bin/clang-tidy ]]; then
    echo /opt/homebrew/opt/llvm/bin/clang-tidy
    return 0
  fi
  if [[ -x /usr/local/opt/llvm/bin/clang-tidy ]]; then
    echo /usr/local/opt/llvm/bin/clang-tidy
    return 0
  fi
  return 1
}

clang_tidy="$(find_clang_tidy || true)"
if [[ -z "${clang_tidy}" ]]; then
  echo "clang-tidy not found."
  echo "Install LLVM tools (recommended): brew install llvm"
  echo "Then add to PATH: export PATH=\"/opt/homebrew/opt/llvm/bin:\$PATH\""
  exit 2
fi

has_nonempty_compile_db() {
  local db="$1"
  if [[ ! -f "$db" ]]; then
    return 1
  fi
  python3 - "$db" <<'PY'
import json, sys
path=sys.argv[1]
try:
    with open(path) as f:
        data=json.load(f)
    sys.exit(0 if isinstance(data, list) and len(data) > 0 else 1)
except Exception:
    sys.exit(1)
PY
}

files=()

if [[ -d "${ROOT}/ASFWDriver/ConfigROM" ]]; then
  while IFS= read -r -d '' f; do
    files+=("$f")
  done < <(find "${ROOT}/ASFWDriver/ConfigROM" -type f -name '*.cpp' -print0)
fi

for f in \
  "${ROOT}/ASFWDriver/Controller/ControllerCore.cpp" \
  "${ROOT}/ASFWDriver/UserClient/Handlers/ConfigROMHandler.cpp" \
  "${ROOT}/ASFWDriver/Bus/BusResetCoordinator.cpp"
do
  if [[ -f "$f" ]]; then
    files+=("$f")
  fi
done

if [[ "${#files[@]}" -eq 0 ]]; then
  echo "No files found to run clang-tidy on."
  exit 0
fi

select_first_nonempty_db() {
  local db
  for db in "$@"; do
    if has_nonempty_compile_db "$db"; then
      echo "$db"
      return 0
    fi
  done
  return 1
}

# Only diagnose headers that are physically within `ASFWDriver/ConfigROM/`.
# Clang may report included header paths containing `../..` segments; exclude those by requiring
# that no path segment begins with '.' (i.e. no `..` traversal).
header_filter="^${ROOT}/ASFWDriver/ConfigROM/([^./][^/]*/)*[^./][^/]*$"

sdk_path="$(xcrun --show-sdk-path 2>/dev/null || true)"
driverkit_sdk="$(xcrun --sdk driverkit --show-sdk-path 2>/dev/null || true)"

run_pass() {
  local pass_name="$1"
  local db="$2"
  local mode="$3"
  shift 3
  local extra_args=("$@")

  local tmpdir
  tmpdir="$(mktemp -d)"
  local out
  out="$(mktemp)"
  local filtered_db="${tmpdir}/compile_commands.json"

  local covered_files=()
  while IFS= read -r line; do
    covered_files+=("$line")
  done < <(python3 - "$db" "$filtered_db" "$mode" "${files[@]}" <<'PY'
import json, os, sys

db_path = sys.argv[1]
out_path = sys.argv[2]
mode = sys.argv[3]
targets = set(sys.argv[4:])

with open(db_path) as f:
    data = json.load(f)

selected = {}
out = []

def command_string(entry: dict) -> str:
    cmd = entry.get("command")
    if isinstance(cmd, str):
        return cmd
    args = entry.get("arguments")
    if isinstance(args, list):
        return " ".join(str(a) for a in args)
    return ""

def matches_mode(cmd: str) -> bool:
    if mode == "host":
        return "ASFW_HOST_TEST" in cmd
    if mode == "driver":
        return (
            "DRIVERKIT_DEPLOYMENT_TARGET" in cmd
            or "DriverKit.platform" in cmd
            or "DriverKit.sdk" in cmd
        )
    raise SystemExit(f"unknown mode: {mode}")

for e in data:
    if not isinstance(e, dict):
        continue
    directory = e.get("directory") or ""
    file = e.get("file")
    if not isinstance(file, str):
        continue
    if not os.path.isabs(file):
        file = os.path.normpath(os.path.join(directory, file))

    if file not in targets:
        continue
    if file in selected:
        continue
    cmd = command_string(e)
    if not matches_mode(cmd):
        continue

    entry = dict(e)
    entry["file"] = file
    entry["directory"] = directory
    out.append(entry)
    selected[file] = True

with open(out_path, "w") as f:
    json.dump(out, f)

for f in sorted(selected.keys()):
    print(f)
PY
)

  if [[ "${#covered_files[@]}" -eq 0 ]]; then
    echo "NOTE: Skipping clang-tidy (${pass_name}) - no matching commands in: ${db}"
    rm -rf "${tmpdir}"
    rm -f "${out}"
    return 0
  fi

  echo "Running clang-tidy (${pass_name}) on ${#covered_files[@]} files (compile DB: ${db})"

  set +e
  set +o pipefail
  "${clang_tidy}" \
    -p "${tmpdir}" \
    -header-filter="${header_filter}" \
    ${extra_args[@]+"${extra_args[@]}"} \
    "${covered_files[@]}" | tee "${out}"
  local tidy_status=${PIPESTATUS[0]}
  set -o pipefail
  set -e

  local warn_count
  warn_count="$(grep -E ': warning:' "${out}" | wc -l | tr -d ' ')"
  local err_count
  err_count="$(grep -E ': error:' "${out}" | wc -l | tr -d ' ')"

  rm -rf "${tmpdir}"
  rm -f "${out}"

  if [[ "${tidy_status}" -ne 0 || "${err_count}" -gt 0 || "${warn_count}" -gt 0 ]]; then
    echo "clang-tidy (${pass_name}) reported ${err_count} errors, ${warn_count} warnings."
    return 1
  fi

  echo "clang-tidy (${pass_name}) clean."
  return 0
}

host_db="$(select_first_nonempty_db "${ROOT}/build/tests_build/compile_commands.json" || true)"
driver_db="$(select_first_nonempty_db "${ROOT}/compile_commands.json" "${ROOT}/build/compile_commands.json" || true)"

if [[ -z "${host_db}" && -z "${driver_db}" ]]; then
  echo "No non-empty compile_commands.json found."
  echo "Generate one via: ./build.sh --commands (DriverKit) or ./build.sh --test-only (host)"
  exit 2
fi

status=0

if [[ -n "${host_db}" ]]; then
  host_args=()
  if [[ -n "${sdk_path}" ]]; then
    host_args+=(--extra-arg-before="--sysroot=${sdk_path}")
  fi
  run_pass "host" "${host_db}" "host" "${host_args[@]}" || status=1
else
  echo "NOTE: Host compile DB not found at ${ROOT}/build/tests_build/compile_commands.json"
  echo "      Generate it via: ./build.sh --test-only"
fi

if [[ -n "${driver_db}" ]]; then
  if [[ -z "${driverkit_sdk}" ]]; then
    echo "DriverKit SDK not found via xcrun --sdk driverkit --show-sdk-path."
    exit 2
  fi

  driver_args=(
    --extra-arg-before="-nostdinc++"
    --extra-arg-before="-isystem${driverkit_sdk}/System/DriverKit/usr/include/c++/v1"
  )
  run_pass "driver" "${driver_db}" "driver" "${driver_args[@]}" || status=1
else
  echo "NOTE: Driver compile DB not found (expected ${ROOT}/compile_commands.json or ${ROOT}/build/compile_commands.json)."
  echo "      Generate one via: ./build.sh --commands"
fi

exit "${status}"
