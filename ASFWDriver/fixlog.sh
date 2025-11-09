#!/bin/zsh
# wraps %s to %{public}s in all files in ASFWDriver directory
# to avoid privacy issues in logs
setopt extendedglob

total=0
for f in **/*(.); do
  # Skip this script itself and other shell scripts
  [[ "$f" == *.sh ]] && continue

  count=$(grep -o '%s' "$f" | wc -l)
  if (( count > 0 )); then
    echo "[$count] $f"
    total=$((total + count))
    sed -i '' 's/%s/%{public}s/g' "$f"
  fi
done
echo "Total replacements: ${total}"