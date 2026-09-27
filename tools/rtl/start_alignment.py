#!/usr/bin/env python3
"""Per-stream-start alignment facts from the driver log ring (TX_OWNERSHIP.md §1g).

For each StartIO after --after (ring sequence), prints the [TxAlign] frame cursor and
projected frame (diff = projected - cursor), the first [TxSyt] sytOffDelayFree, the ZTS
seed age/SYT and the first [TxPlace] offsets. Pair each line with an
`rtl_loopback --measure` run on that stream: RTL_ts = 105.03 + diff on the Pro 24 DSP
while the cursor is rounded to a packet boundary.

Reads the ring over the app-hosted MCP server (skills/asfw-mcp-control-plane).
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLIENT = ROOT / "skills/asfw-mcp-control-plane/scripts/asfw_mcp.py"


def ring(after):
    records = []
    while True:
        args = {"afterSequence": after, "maxRecords": 200, "maxLevel": "debug"}
        out = subprocess.run(["python3", str(CLIENT), "call", "asfw_log_query", json.dumps(args)],
                             capture_output=True, text=True, check=True).stdout
        data = json.loads(out)["structuredContent"]["data"]
        records += data["records"]
        if data["nextSequence"] <= after or data["nextSequence"] >= data["latestSequence"]:
            return records
        after = data["nextSequence"]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--after", type=int, default=0, help="ring sequence to start after")
    args = parser.parse_args()
    starts = []
    for record in ring(args.after):
        message = record["message"]
        if "ASFWAudioDevice: StartIO flags" in message:
            starts.append({"seq": record["sequence"], "place": []})
        if not starts:
            continue
        cur = starts[-1]
        if "[TxAlign]" in message:
            m = re.search(r"cursor -> (\d+) \(projected=(\d+) rxFirstFrame=(\d+) deltaTicks=(-?\d+) rate=(\d+)", message)
            if m:
                cur["align"] = [int(v) for v in m.groups()]
        elif "[TxSyt]" in message and "syt" not in cur:
            m = re.search(r"sytOffDelayFree=(\d+)", message)
            cur["syt"] = m.group(1) if m else None
        elif "[Zts] SEED" in message:
            m = re.search(r"age=(-?\d+).*syt=(0x[0-9a-f]+)", message)
            cur["seed"] = m.groups() if m else None
        elif "[TxPlace]" in message and len(cur["place"]) < 5:
            m = re.search(r"offset=(-?\d+)", message)
            if m:
                cur["place"].append(m.group(1))
    for s in starts:
        a = s.get("align")
        align = (f"cursor={a[0]} projected={a[1]} diff={a[1] - a[0]} rate={a[4]}" if a else "no [TxAlign]")
        print(f"seq {s['seq']}: {align} sytOffDelayFree={s.get('syt')} "
              f"zts_seed={s.get('seed')} place={','.join(s['place'])}")


if __name__ == "__main__":
    main()
