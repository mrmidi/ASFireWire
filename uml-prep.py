python3 -c '
import json, re

with open("compile_commands.json", "r") as f:
    db = json.load(f)

clean_db = []
for entry in db:
    cmd = entry["command"]
    
    # 1. Chop off any secondary sub-jobs appended via semicolons
    if ";" in cmd:
        cmd = cmd.split(";")[0]
        
    # 2. Convert internal "-cc1" compilation dumps back into high-level compiler arguments
    cmd = cmd.replace("-cc1", "")
    
    # 3. Strip out the Apple-specific arguments that break LLVM
    flags_to_drop = ["-ivfsstatcache", "-index-store-path", "-index-unit-output-path", "-serialize-diagnostic-file"]
    for flag in flags_to_drop:
        cmd = re.sub(rf"{flag}\s+\S+", "", cmd)
        
    # 4. Remove any references to virtual .sdkstatcache files at the end of the line
    cmd = re.sub(r"\S+\.sdkstatcache", "", cmd)
    
    # 5. Correct the truncated virtual object paths back to their true locations
    cmd = cmd.replace("/ASFW.build", "/Volumes/SDExt/DEV/ASFireWire/build/DerivedData/Build/Intermediates.noindex/ASFW.build")
    
    entry["command"] = cmd.strip()
    clean_db.append(entry)

with open("compile_commands.json", "w") as f:
    json.dump(clean_db, f, indent=2)
print("[OK] compile_commands.json has been thoroughly sanitized!")
'

