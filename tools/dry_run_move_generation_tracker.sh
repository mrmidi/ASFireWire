#!/usr/bin/env bash
set -euo pipefail

# Dry-run plan: Move GenerationTracker.hpp from Async/Bus/ -> Bus/
# Prints git mv and sed replacement commands; DOES NOT apply them unless --apply provided

APPLY=false
if [[ ${1:-} == "--apply" ]]; then
  APPLY=true
fi

ROOT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
cd "$ROOT_DIR"

OLD_PATH="ASFWDriver/Async/Bus/GenerationTracker.hpp"
NEW_DIR="ASFWDriver/Bus"
NEW_PATH="$NEW_DIR/GenerationTracker.hpp"

echo "Dry-run: Move GenerationTracker from $OLD_PATH -> $NEW_PATH"

if [[ ! -f "$OLD_PATH" ]]; then
  echo "Error: $OLD_PATH not found" >&2
  exit 1
fi

OLD_PATH_CPP="ASFWDriver/Async/Bus/GenerationTracker.cpp"

# Find includes that reference the header
echo "\nScanning includes that reference GenerationTracker.hpp..."
INCLUDE_FILES=$(rg --hidden --line-number "GenerationTracker.hpp" -g '!build' -g '!bin' -g '!out' -n || true)

if [[ -z "$INCLUDE_FILES" ]]; then
  echo "No references found that include GenerationTracker.hpp (unexpected)."
else
  echo "References:"; echo "$INCLUDE_FILES"
fi

# For each file, produce sed replacement suggestions to update includes
echo "\nSuggested sed replacements (do NOT run these yet):"
while IFS= read -r line; do
  # line format: path:line:content or similar
  filePath=$(echo "$line" | cut -d ':' -f 1)
  # Determine replacement path relative to the file
  # We'll compute relative path from filePath to NEW_PATH
  relPath=$(python3 - <<PY
import os
file=os.path.abspath('$filePath')
root=os.path.abspath('$ROOT_DIR')
new=os.path.abspath('$NEW_PATH')
# compute relative path from the file's directory
print(os.path.relpath(new, start=os.path.dirname(file)))
PY
)
  sedCmd="sed -n '1,200p' $filePath | rg -n 'GenerationTracker.hpp' || true"
  echo "File: $filePath"
  echo "  Replace includes with: #include \"$relPath\""
  echo "  sed -i.bak 's|Async/Bus/GenerationTracker.hpp|$relPath|g' $filePath"
  echo
done <<< "$INCLUDE_FILES"

# Final git commands
echo "\nSuggested git commands (dry-run):"
echo "  git mv $OLD_PATH $NEW_PATH"
if [[ -f "$OLD_PATH_CPP" ]]; then
  echo "  git mv $OLD_PATH_CPP $NEW_DIR/GenerationTracker.cpp"
fi
echo "  git add -A"
echo "  git commit -m 'refactor: Move GenerationTracker to Bus/ and update includes'"

echo "\nNotes:"
echo "- This script prints commands but does not run them by default."
echo "- Carefully review sed replacements; adjust relative paths where necessary." 

echo "- After git mv and include updates, run: ./build.sh && run relevant unit tests."

if [[ "$APPLY" == "true" ]]; then
  echo "\n--apply used: performing the git mv and sed replacements now"
  # Confirm with user
  read -p "Proceed with git mv and include updates? [y/N] " confirm
  if [[ "$confirm" == "y" || "$confirm" == "Y" ]]; then
    git mv "$OLD_PATH" "$NEW_PATH"
    # Perform sed replacements; this applies simple patterns and may need refinement
    while IFS= read -r line; do
      filePath=$(echo "$line" | cut -d ':' -f 1)
      if [[ -f "$OLD_PATH_CPP" ]]; then
        git mv "$OLD_PATH_CPP" "$NEW_DIR/GenerationTracker.cpp"
      fi
      relPath=$(python3 - <<PY
import os
file=os.path.abspath('$filePath')
root=os.path.abspath('$ROOT_DIR')
new=os.path.abspath('$NEW_PATH')
print(os.path.relpath(new, start=os.path.dirname(file)))
PY
)
      sed -i.bak "s|Async/Bus/GenerationTracker.hpp|$relPath|g" "$filePath" || true
    done <<< "$INCLUDE_FILES"
    git add -A
    git commit -m "refactor: Move GenerationTracker to Bus/ and update includes"
  else
    echo "Apply aborted"
  fi
fi

exit 0
