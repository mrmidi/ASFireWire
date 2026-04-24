#!/usr/bin/env bash
set -euo pipefail

# install-debug-asfw.sh
#
# 唯一的 ASFW 开发安装入口：
#   1) 用经过验证的 xcodebuild 参数产出 ad-hoc signed app/dext
#   2) 可选执行 system extension 卸载/垃圾回收
#   3) 覆盖安装到 /Applications/ASFW.app
#   4) 启动 app，并输出 app 内嵌 dext 与当前活跃 dext 的状态
#
# 用法：
#   ./install-debug-asfw.sh
#   ./install-debug-asfw.sh --fresh
#
# 可选环境变量：
#   ASFW_TEAM_ID   指定 systemextensionsctl uninstall 使用的 Team ID
#                  为空时卸载时用 '-'（开发阶段常用）

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CONFIGURATION="Debug"
DERIVED_DATA_PATH="${REPO_ROOT}/build/DerivedData"
TEAM_ID="${ASFW_TEAM_ID:-}"
SYSTEMEXT_BUNDLE_ID="net.mrmidi.ASFW.ASFWDriver"
DEXT_BINARY_NAME="net.mrmidi.ASFW.ASFWDriver"
APP_SOURCE="${DERIVED_DATA_PATH}/Build/Products/${CONFIGURATION}/ASFW.app"
APP_DEST="/Applications/ASFW.app"
APP_BINARY_REL="Contents/MacOS/ASFW"
APP_DEXT_REL="Contents/Library/SystemExtensions/${SYSTEMEXT_BUNDLE_ID}.dext/${DEXT_BINARY_NAME}"
FRESH_INSTALL=false
REFRESH_DRIVER=false

usage() {
  cat <<'EOF'
Usage:
  ./install-debug-asfw.sh [--fresh] [--refresh]

Options:
  --fresh    Install 前先执行 systemextensionsctl uninstall/gc
  --refresh  安装后提交 dext activation/replacement request
  -h, --help
EOF
}

log() {
  echo "[$(date '+%H:%M:%S')] $*"
}

systemextensions_list() {
  systemextensionsctl list 2>/dev/null || true
}

run_maybe_sudo() {
  if "$@"; then
    return 0
  fi

  local status=$?

  if [[ -t 0 && -t 1 ]]; then
    sudo "$@"
    return $?
  fi

  if command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
    sudo "$@"
    return $?
  fi

  return "${status}"
}

sha256() {
  shasum -a 256 "$1" | awk '{print $1}'
}

asfw_systemextension_count() {
  systemextensions_list | grep -c "net.mrmidi.ASFW.ASFWDriver (" || true
}

has_duplicate_asfw_extensions() {
  local count
  count="$(asfw_systemextension_count)"
  [[ "${count}" -gt 1 ]]
}

print_asfw_systemextension_status() {
  local lines
  lines="$(systemextensions_list | grep "net.mrmidi.ASFW.ASFWDriver (" || true)"
  if [[ -z "${lines}" ]]; then
    log "systemextensionsctl: 当前没有 ASFW 条目。"
    return 0
  fi

  log "systemextensionsctl: 当前 ASFW 条目如下："
  while IFS= read -r line; do
    log "  ${line}"
  done <<< "${lines}"
}

asfw_app_pids() {
  local app_binary_pattern
  app_binary_pattern="${APP_DEST//./\\.}/${APP_BINARY_REL}"
  pgrep -f "^${app_binary_pattern}([[:space:]]|$)" || true
}

signal_asfw_app() {
  local signal="$1"
  local pids
  pids="$(asfw_app_pids)"
  [[ -z "${pids}" ]] && return 0

  kill "-${signal}" ${pids}
}

wait_for_asfw_app_exit() {
  local attempts="${1:-5}"

  while (( attempts > 0 )); do
    if [[ -z "$(asfw_app_pids)" ]]; then
      return 0
    fi

    sleep 1
    ((attempts--))
  done

  return 1
}

close_existing_asfw_app() {
  if [[ -z "$(asfw_app_pids)" ]]; then
    return 0
  fi

  log "Closing existing ASFW.app instances before install..."
  osascript -e 'tell application id "net.mrmidi.ASFW" to quit' >/dev/null 2>&1 \
    || osascript -e 'tell application "ASFW" to quit' >/dev/null 2>&1 \
    || true

  if wait_for_asfw_app_exit 5; then
    log "Existing ASFW.app exited cleanly."
    return 0
  fi

  log "ASFW.app did not exit after quit request; sending SIGTERM to app process."
  signal_asfw_app TERM

  if wait_for_asfw_app_exit 5; then
    log "Existing ASFW.app exited after SIGTERM."
    return 0
  fi

  log "ASFW.app still running; sending SIGKILL to app process."
  signal_asfw_app KILL
  wait_for_asfw_app_exit 3 || true
}

cleanup_asfw_systemextensions() {
  local uninstall_team_id="${TEAM_ID:-"-"}"
  log "Cleaning existing ASFW system extension state..."
  run_maybe_sudo systemextensionsctl uninstall "${uninstall_team_id}" "${SYSTEMEXT_BUNDLE_ID}" || true
  sleep 1
  run_maybe_sudo systemextensionsctl gc || true
  print_asfw_systemextension_status
}

active_dext_path() {
  find /Library/SystemExtensions -maxdepth 2 -path "*/${SYSTEMEXT_BUNDLE_ID}.dext" -type d -print | head -n 1
}

print_active_dext_status() {
  local active_path
  active_path="$(active_dext_path)"

  if [[ -z "${active_path}" ]]; then
    log "当前没有发现活跃的 ASFW dext。"
    return 0
  fi

  local active_binary="${active_path}/${DEXT_BINARY_NAME}"
  log "当前活跃 dext 路径: ${active_path}"
  if [[ -f "${active_binary}" ]]; then
    log "当前活跃 dext hash: $(sha256 "${active_binary}")"
  else
    log "当前活跃 dext 缺少可执行文件: ${active_binary}"
  fi
}

wait_for_active_dext_hash() {
  local expected_hash="$1"
  local attempts="${2:-10}"

  while (( attempts > 0 )); do
    local active_path
    active_path="$(active_dext_path)"
    if [[ -n "${active_path}" ]]; then
      local active_binary="${active_path}/${DEXT_BINARY_NAME}"
      if [[ -f "${active_binary}" ]]; then
        local active_hash
        active_hash="$(sha256 "${active_binary}")"
        if [[ "${active_hash}" == "${expected_hash}" ]]; then
          log "活跃 dext 已切换到新 build: ${active_hash}"
          return 0
        fi
      fi
    fi

    sleep 1
    ((attempts--))
  done

  log "活跃 dext 暂未切换到新 build，当前状态如下："
  print_asfw_systemextension_status
  print_active_dext_status
  return 1
}

launch_app() {
  local -a launch_args=("$@")
  log "启动 ${APP_DEST}..."
  if open "${APP_DEST}" --args "${launch_args[@]}"; then
    return 0
  fi

  log "open 失败，回退为直接启动 app 二进制。"
  "${APP_DEST}/${APP_BINARY_REL}" "${launch_args[@]}" >/tmp/asfw-install-launch.out 2>/tmp/asfw-install-launch.err &
  sleep 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --fresh)
      FRESH_INSTALL=true
      shift
      ;;
    --refresh)
      REFRESH_DRIVER=true
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "❌ Unknown arg: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

log "Building ASFW.app (${CONFIGURATION}) via xcodebuild..."
log "Repo root   : ${REPO_ROOT}"
log "DerivedData : ${DERIVED_DATA_PATH}"
log "Team ID     : ${TEAM_ID:-'(empty)'}"
log "Fresh mode  : ${FRESH_INSTALL}"
log "Refresh mode: ${REFRESH_DRIVER}"

cd "${REPO_ROOT}"

xcodebuild \
  -project ASFW.xcodeproj \
  -scheme ASFW \
  -configuration "${CONFIGURATION}" \
  clean build \
  -derivedDataPath "${DERIVED_DATA_PATH}" \
  CODE_SIGN_STYLE=Manual \
  DEVELOPMENT_TEAM="${TEAM_ID}" \
  CODE_SIGN_IDENTITY=- \
  PROVISIONING_PROFILE_SPECIFIER= \
  PROVISIONING_PROFILE= \
  CODE_SIGNING_REQUIRED=NO

if [[ ! -d "${APP_SOURCE}" ]]; then
  echo "❌ Built app not found at: ${APP_SOURCE}" >&2
  exit 1
fi

BUILT_DEXT_BINARY="${APP_SOURCE}/${APP_DEXT_REL}"

if ! codesign -dv --verbose=4 "${BUILT_DEXT_BINARY}" >/tmp/asfw-install-codesign.txt 2>&1; then
  echo "❌ Built dext is not signed. Aborting install." >&2
  cat /tmp/asfw-install-codesign.txt >&2
  exit 1
fi

log "Built app dext hash   : $(sha256 "${BUILT_DEXT_BINARY}")"
BUILT_DEXT_HASH="$(sha256 "${BUILT_DEXT_BINARY}")"

close_existing_asfw_app

if $FRESH_INSTALL || has_duplicate_asfw_extensions; then
  if ! $FRESH_INSTALL; then
    log "Detected duplicated ASFW system extension state before install; performing cleanup."
  fi
  cleanup_asfw_systemextensions
  if has_duplicate_asfw_extensions; then
    log "Cleanup did not fully clear duplicates; retrying..."
    sleep 2
    cleanup_asfw_systemextensions
  fi
fi

if [[ -e "${APP_DEST}" ]]; then
  log "Removing existing ${APP_DEST} to avoid stale signed resources..."
  run_maybe_sudo rm -rf "${APP_DEST}"
fi

log "Installing ASFW.app to /Applications..."
run_maybe_sudo ditto "${APP_SOURCE}" "${APP_DEST}"

log "Installed app dext hash: $(sha256 "${APP_DEST}/${APP_DEXT_REL}")"

APP_LAUNCH_ARGS=()
WAIT_ATTEMPTS=10

if $REFRESH_DRIVER; then
  APP_LAUNCH_ARGS+=(--activate-driver)
  WAIT_ATTEMPTS=20
  log "Refresh mode: app 将在启动后自动提交 dext activation request。"
fi

launch_app "${APP_LAUNCH_ARGS[@]}"

if ! wait_for_active_dext_hash "${BUILT_DEXT_HASH}" "${WAIT_ATTEMPTS}"; then
  if $REFRESH_DRIVER && has_duplicate_asfw_extensions; then
    log "Refresh did not switch to the new dext because duplicate ASFW system extension entries are still present."
    log "Attempting one self-healing cleanup + refresh retry..."
    cleanup_asfw_systemextensions
    launch_app "${APP_LAUNCH_ARGS[@]}"
    wait_for_active_dext_hash "${BUILT_DEXT_HASH}" "${WAIT_ATTEMPTS}" || true
  fi
fi

log "如果系统弹出 Driver Extension 审批或重新激活提示，请按提示完成。"
log "ASFW debug install finished."
