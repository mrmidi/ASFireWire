#!/usr/bin/env bash
#
# install.sh - link the ASFW MCP control-plane skill into Claude Code.
#
# The skill itself is committed to the repository. What is not committed is the
# wiring: Claude Code discovers skills under `.claude/skills/`, and `.claude/` is
# gitignored, so every checkout has to link it once. That is all this does.
#
set -euo pipefail

# Resolve this script's own directory, following symlinks, so the installer works
# no matter where it is invoked from. macOS has no `readlink -f`.
SCRIPT_PATH="${BASH_SOURCE[0]}"
while [ -L "$SCRIPT_PATH" ]; do
    SCRIPT_DIR="$(cd -P "$(dirname "$SCRIPT_PATH")" && pwd)"
    SCRIPT_PATH="$(readlink "$SCRIPT_PATH")"
    case "$SCRIPT_PATH" in
        /*) ;;
        *) SCRIPT_PATH="$SCRIPT_DIR/$SCRIPT_PATH" ;;
    esac
done
SKILL_DIR="$(cd -P "$(dirname "$SCRIPT_PATH")" && pwd)"
SKILL_NAME="$(basename "$SKILL_DIR")"
REPO_ROOT="$(cd -P "$SKILL_DIR/../.." && pwd)"

SCOPE="project"
FORCE=0

usage() {
    cat <<USAGE
Install the '${SKILL_NAME}' skill for Claude Code.

Usage: ./install.sh [--user] [--force]

  --user    Install for your account (~/.claude/skills) instead of just this
            checkout. Use when you work across several clones; the link is
            absolute, so it breaks if you move or delete this checkout.
  --force   Replace whatever already occupies the destination. Without this the
            installer refuses to touch anything it did not create.
  -h        Show this message.

Default is project scope: the link lives in this repository's .claude/skills/,
which is gitignored, so it affects only your checkout.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --user) SCOPE="user"; shift ;;
        --force) FORCE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "install.sh: unknown option '$1'" >&2; echo >&2; usage >&2; exit 2 ;;
    esac
done

if [ "$SCOPE" = "user" ]; then
    SKILLS_DIR="$HOME/.claude/skills"
else
    SKILLS_DIR="$REPO_ROOT/.claude/skills"
fi
LINK_PATH="$SKILLS_DIR/$SKILL_NAME"

echo "=============================================================="
echo " ASFW MCP control-plane skill installer"
echo "=============================================================="
echo
echo "NOTE: this installs the skill for Claude Code only, for now."
echo "      An OpenAI agent definition ships alongside it in agents/,"
echo "      but this script does not wire that up."
echo
echo "What this script does:"
echo "  1. Creates ${SKILLS_DIR}"
echo "  2. Symlinks ${SKILL_NAME} there, pointing at this checkout"
echo "  3. Verifies the link resolves to a readable SKILL.md"
echo
echo "It does NOT copy any files, modify the repository, install packages,"
echo "or touch the driver, the app, or any hardware."
echo
echo "  skill source : ${SKILL_DIR}"
echo "  repo root    : ${REPO_ROOT}"
echo "  install into : ${LINK_PATH}  (${SCOPE} scope)"
echo

if [ ! -f "$SKILL_DIR/SKILL.md" ]; then
    echo "ERROR: no SKILL.md in ${SKILL_DIR} -- is this script still inside the skill?" >&2
    exit 1
fi

echo "[1/3] Creating ${SKILLS_DIR} ..."
mkdir -p "$SKILLS_DIR"
echo "      ok"

echo "[2/3] Linking ${SKILL_NAME} ..."
if [ -L "$LINK_PATH" ]; then
    EXISTING="$(cd -P "$(dirname "$LINK_PATH")" && cd -P "$(dirname "$(readlink "$LINK_PATH")")" 2>/dev/null && pwd || true)"
    RESOLVED="$(cd -P "$LINK_PATH" 2>/dev/null && pwd || true)"
    if [ -n "$RESOLVED" ] && [ "$RESOLVED" = "$SKILL_DIR" ]; then
        echo "      already linked to this checkout -- nothing to do"
    elif [ "$FORCE" -eq 1 ]; then
        echo "      replacing existing link -> $(readlink "$LINK_PATH")"
        rm -f "$LINK_PATH"
        ln -s "$SKILL_DIR" "$LINK_PATH"
        echo "      ok"
    else
        echo >&2
        echo "ERROR: ${LINK_PATH} already exists and points somewhere else:" >&2
        echo "         -> $(readlink "$LINK_PATH")" >&2
        echo "       Re-run with --force to replace it." >&2
        exit 1
    fi
elif [ -e "$LINK_PATH" ]; then
    if [ "$FORCE" -eq 1 ]; then
        echo "      replacing existing directory/file at destination"
        rm -rf "$LINK_PATH"
        ln -s "$SKILL_DIR" "$LINK_PATH"
        echo "      ok"
    else
        echo >&2
        echo "ERROR: ${LINK_PATH} already exists and is not a symlink." >&2
        echo "       Refusing to delete it. Inspect it, then re-run with --force." >&2
        exit 1
    fi
else
    ln -s "$SKILL_DIR" "$LINK_PATH"
    echo "      ok"
fi

echo "[3/3] Verifying ..."
if [ ! -r "$LINK_PATH/SKILL.md" ]; then
    echo "ERROR: ${LINK_PATH}/SKILL.md is not readable -- the link did not resolve." >&2
    exit 1
fi
echo "      ${LINK_PATH}/SKILL.md is readable"
echo

echo "Installed."
echo
echo "Next steps:"
echo "  1. Restart Claude Code so it re-scans skills. It then loads on its own"
echo "     when a task needs live driver state, or invoke it explicitly with"
echo "     /${SKILL_NAME}"
echo
echo "  2. The skill talks to the app-hosted MCP endpoint, so the ASFW app must"
echo "     be running with the MCP Control Plane enabled in its settings."
echo "     Default endpoint is http://127.0.0.1:8766/mcp; if you changed the"
echo "     port, export ASFW_MCP_ENDPOINT=http://127.0.0.1:<port>/mcp"
echo
echo "  3. Confirm it works (read-only, no hardware action):"
echo "     python3 ${SKILL_DIR#"$REPO_ROOT"/}/scripts/asfw_mcp.py health"
echo
if ! command -v python3 >/dev/null 2>&1; then
    echo "  WARNING: python3 was not found on PATH. The client is stdlib-only"
    echo "           (nothing to pip install) but it does need python3."
    echo
fi
echo "To uninstall:  rm ${LINK_PATH}"
