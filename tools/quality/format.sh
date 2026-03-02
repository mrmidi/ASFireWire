#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLANG_FORMAT=(xcrun clang-format)

targets=()

if [[ -d "${ROOT}/ASFWDriver/ConfigROM" ]]; then
  while IFS= read -r -d '' f; do
    targets+=("$f")
  done < <(find "${ROOT}/ASFWDriver/ConfigROM" \
      -type f \( -name '*.hpp' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \) -print0)
fi

# Integration files (only if present).
for f in \
  "${ROOT}/ASFWDriver/Controller/ControllerCore.cpp" \
  "${ROOT}/ASFWDriver/Controller/ControllerCore.hpp" \
  "${ROOT}/ASFWDriver/UserClient/Handlers/ConfigROMHandler.cpp" \
  "${ROOT}/ASFWDriver/UserClient/Handlers/ConfigROMHandler.hpp" \
  "${ROOT}/ASFWDriver/Bus/BusResetCoordinator.cpp" \
  "${ROOT}/ASFWDriver/Bus/BusResetCoordinator.hpp" \
  "${ROOT}/ASFWDriver/Hardware/RegisterMap.hpp"
do
  if [[ -f "$f" ]]; then
    targets+=("$f")
  fi
done

if [[ "${#targets[@]}" -eq 0 ]]; then
  echo "No files found to format."
  exit 0
fi

echo "Formatting ${#targets[@]} files..."
"${CLANG_FORMAT[@]}" -i "${targets[@]}"
echo "Done."
