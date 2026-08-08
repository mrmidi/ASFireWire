#!/usr/bin/env bash
#
# bump.sh - Version Bumping and Metadata Generation
#
# Combined script that handles:
# - Version bumping (major/minor/patch) in VERSION.txt
# - Git metadata extraction
# - DriverVersion.hpp generation
#
# Useful for debugging: e.g. latest build is loaded


set -euo pipefail

# Use SRCROOT if set (Xcode build), otherwise use script directory (manual run)
if [[ -n "${SRCROOT:-}" ]]; then
  PROJECT_ROOT="${SRCROOT}"
else
  PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi

VERSION_FILE="${PROJECT_ROOT}/VERSION.txt"
OUTPUT_FILE="${PROJECT_ROOT}/ASFWDriver/Version/DriverVersion.hpp"

# Colors
RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
YELLOW=$'\033[1;33m'
BLUE=$'\033[0;34m'
NC=$'\033[0m'

usage() {
  cat <<EOF
Usage: $0 [major|minor|patch|refresh]

  major     Bump major version (1.2.3 -> 2.0.0)
  minor     Bump minor version (1.2.3 -> 1.3.0)
  patch     Bump patch version (1.2.3 -> 1.2.4)
  refresh   Regenerate version header without bumping VERSION.txt

If no argument provided, defaults to 'refresh' (just regenerate metadata).

Examples:
  $0 patch    # Bump patch version and regenerate
  $0 refresh  # Just regenerate with current VERSION.txt
  $0          # Same as refresh
EOF
}

log() { echo "${BLUE}[INFO]${NC} $*"; }
ok()  { echo "${GREEN}[OK]${NC} $*"; }
warn(){ echo "${YELLOW}[WARN]${NC} $*"; }
err() { echo "${RED}[ERROR]${NC} $*"; exit 1; }

# Read current version from VERSION.txt
read_version() {
  if [[ ! -f "$VERSION_FILE" ]]; then
    err "VERSION.txt not found at $VERSION_FILE"
  fi
  
  local version
  version=$(head -n 1 "$VERSION_FILE" | tr -d '[:space:]')
  
  # Extract version components (strip any -alpha, -beta suffixes for bumping)
  local base_version="${version%%-*}"
  local suffix=""
  [[ "$version" =~ - ]] && suffix="-${version#*-}"
  
  echo "$base_version|$suffix"
}

# Parse version string into components
parse_version() {
  local version="$1"
  local IFS='.'
  read -r major minor patch <<< "$version"
  echo "$major $minor $patch"
}

# Bump version based on type
bump_version() {
  local bump_type="$1"
  local version_data
  version_data=$(read_version)
  
  local base_version="${version_data%%|*}"
  local suffix="${version_data#*|}"
  
  read -r major minor patch <<< "$(parse_version "$base_version")"
  
  case "$bump_type" in
    major)
      major=$((major + 1))
      minor=0
      patch=0
      ;;
    minor)
      minor=$((minor + 1))
      patch=0
      ;;
    patch)
      patch=$((patch + 1))
      ;;
    *)
      err "Invalid bump type: $bump_type"
      ;;
  esac
  
  local new_version="${major}.${minor}.${patch}${suffix}"
  echo "$new_version" > "$VERSION_FILE"
  ok "Bumped version: $base_version$suffix → $new_version"
  echo "$new_version"
}

# Generate version header with git metadata
generate_version_header() {
  log "Generating version metadata..."
  
  # Read semantic version. ASFW_VERSION_OVERRIDE lets CI stamp a release version
  # (from the git tag) WITHOUT writing VERSION.txt — writing that tracked file would
  # dirty the working tree and make every release build report kGitDirty = true.
  local SEMANTIC_VERSION
  if [[ -n "${ASFW_VERSION_OVERRIDE:-}" ]]; then
    SEMANTIC_VERSION="${ASFW_VERSION_OVERRIDE}"
  else
    SEMANTIC_VERSION=$(head -n 1 "$VERSION_FILE" | tr -d '[:space:]')
  fi

  # Ensure we're in a git repository
  if ! git rev-parse --git-dir > /dev/null 2>&1; then
    warn "Not in a git repository - using placeholder values"
    GIT_COMMIT_FULL="unknown"
    GIT_COMMIT_SHORT="unknown"
    GIT_BRANCH="unknown"
    GIT_DIRTY=true
  else
    # Extract git metadata
    GIT_COMMIT_FULL=$(git rev-parse HEAD 2>/dev/null || echo "unknown")
    GIT_COMMIT_SHORT=$(git rev-parse --short=7 HEAD 2>/dev/null || echo "unknown")
    GIT_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
    
    # Check for uncommitted changes
    if git diff-index --quiet HEAD -- 2>/dev/null; then
      GIT_DIRTY=false
    else
      GIT_DIRTY=true
    fi
  fi
  
  # Build timestamp (ISO 8601). Deliberately the COMMIT date, not `date -u` now:
  # a wall-clock timestamp changes the header's contents on every single run, which
  # forces every translation unit that includes DriverVersion.hpp to recompile even
  # when nothing changed, and makes builds non-reproducible. The commit date is
  # stable for a given checkout and just as useful for identifying a build.
  # Normalised to UTC so the string keeps the exact shape it has always had
  # (…Z) — it is logged and passed across the user-client boundary.
  BUILD_TIMESTAMP=$(TZ=UTC git log -1 --date=format-local:'%Y-%m-%dT%H:%M:%SZ' --format=%cd 2>/dev/null || true)
  [[ -n "${BUILD_TIMESTAMP}" ]] || BUILD_TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

  # Build host
  BUILD_HOST=$(hostname -s 2>/dev/null || echo "unknown")
  
  # Compiler version (clang)
  COMPILER_VERSION=$(clang --version 2>/dev/null | head -n 1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n 1 || echo "unknown")
  
  # Construct full version string
  local DIRTY_FLAG=""
  $GIT_DIRTY && DIRTY_FLAG=" DIRTY"
  FULL_VERSION_STRING="ASFWDriver v${SEMANTIC_VERSION} (${GIT_COMMIT_SHORT}${DIRTY_FLAG})"
  BUILD_INFO_STRING="Build: ${GIT_COMMIT_SHORT} (${GIT_BRANCH}) @ ${BUILD_TIMESTAMP}"
  
  # Ensure output directory exists
  mkdir -p "$(dirname "$OUTPUT_FILE")"

  # Generate to a temp file first, then replace the real header ONLY if the contents
  # actually changed. Rewriting an identical file still bumps its mtime, which is
  # enough to make Xcode/ninja rebuild every dependent TU on each build.
  # No RETURN trap here: bash traps are global, so it would survive this function
  # and fire on a later return with TMP_OUTPUT out of scope (unbound under `set -u`).
  # Both exit paths below dispose of the temp file explicitly, and `set -e` aborts
  # the run if the heredoc write itself fails.
  local TMP_OUTPUT="${OUTPUT_FILE}.tmp.$$"

  # Generate header file
  cat > "$TMP_OUTPUT" <<EOF
// Auto-generated by scripts/bump.sh
// DO NOT EDIT THIS FILE MANUALLY
// Generated at: ${BUILD_TIMESTAMP}

#pragma once

#include <cstdint>

namespace ASFW::Version {

// Git metadata
inline constexpr const char* kGitCommitFull = "${GIT_COMMIT_FULL}";
inline constexpr const char* kGitCommitShort = "${GIT_COMMIT_SHORT}";
inline constexpr const char* kGitBranch = "${GIT_BRANCH}";
inline constexpr bool kGitDirty = ${GIT_DIRTY};

// Build metadata
inline constexpr const char* kBuildTimestamp = "${BUILD_TIMESTAMP}";
inline constexpr const char* kBuildHost = "${BUILD_HOST}";
inline constexpr const char* kCompilerVersion = "${COMPILER_VERSION}";

// Semantic version
inline constexpr const char* kSemanticVersion = "${SEMANTIC_VERSION}";

// Combined version string for logging
inline constexpr const char* kFullVersionString = 
    "${FULL_VERSION_STRING}";

// Build info string
inline constexpr const char* kBuildInfoString = 
    "${BUILD_INFO_STRING}";

} // namespace ASFW::Version
EOF

  if [[ -f "$OUTPUT_FILE" ]] && cmp -s "$TMP_OUTPUT" "$OUTPUT_FILE"; then
    rm -f "$TMP_OUTPUT"
    ok "Version metadata unchanged: $OUTPUT_FILE (not rewritten)"
  else
    mv -f "$TMP_OUTPUT" "$OUTPUT_FILE"
    ok "Generated version metadata: $OUTPUT_FILE"
  fi
  echo "   Version: ${SEMANTIC_VERSION}"
  echo "   Commit:  ${GIT_COMMIT_SHORT} (${GIT_BRANCH})"
  echo "   Dirty:   ${GIT_DIRTY}"
  echo "   Time:    ${BUILD_TIMESTAMP}"
}

# Main logic
main() {
  local action="${1:-refresh}"
  
  case "$action" in
    -h|--help)
      usage
      exit 0
      ;;
    major|minor|patch)
      log "Bumping $action version..."
      bump_version "$action"
      ;;
    refresh)
      log "Refreshing version metadata (no bump)..."
      ;;
    *)
      err "Unknown action: $action. Use: major, minor, patch, or refresh"
      ;;
  esac
  
  # Always regenerate version header
  generate_version_header
  
  # Show current version
  local current_version
  current_version=$(head -n 1 "$VERSION_FILE" | tr -d '[:space:]')
  echo
  echo "Current version: ${GREEN}${current_version}${NC}"
}

main "$@"
