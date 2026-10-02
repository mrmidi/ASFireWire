#!/usr/bin/env python3
"""Read-only: which FB1 feature controls does the Duet answer, per channel? (STATUS only.)

Frame: 01 08 B8 81 <fb> <attr> 02 <ch> <cs> <len> FF.. (Audio Subunit 1.0, FUNCTION BLOCK feature).
Usage: duet_fb_status.py <nodeId> <generation> <guid-hex> [out.json]
"""
import json
import sys

sys.path.insert(0, "tmp/avc-wt/tools/avc")
from avc_probe import MCPClient  # noqa: E402

CS = {0x01: ("mute", 1), 0x02: ("volume", 2), 0x03: ("lr_balance", 2), 0x04: ("fr_balance", 2),
      0x05: ("bass", 1), 0x06: ("mid", 1), 0x07: ("treble", 1), 0x09: ("agc", 1), 0x0A: ("delay", 2),
      0x0B: ("bass_boost", 1), 0x0C: ("loudness", 1)}
ATTR = {0x10: "current", 0x02: "min", 0x03: "max", 0x01: "resolution"}

node, gen, guid = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3], 16)
c = MCPClient()
c.connect()
records = []


def send(name, payload):
    r = c.call_tool("asfw_fcp_send_command", {"targetGuid": guid, "nodeId": node, "generation": gen,
                                              "addressHigh": 0xFFFF, "addressLow": 0xF0000B00,
                                              "intent": "status", "payload": payload})
    resp = (r.get("data") or {}).get("response")
    records.append({"name": name, "command": payload, "response": resp, "ok": r.get("ok")})
    print(f"{name:34s} {' '.join(f'{b:02X}' for b in payload):40s} -> "
          f"{' '.join(f'{b:02X}' for b in resp) if resp else r.get('errors')}")
    return resp


for fb in (1,):
    for ch in (0, 1, 2, 3):
        for cs, (nm, ln) in CS.items():
            send(f"fb{fb}_ch{ch}_{nm}_current", [0x01, 0x08, 0xB8, 0x81, fb, 0x10, 0x02, ch, cs, ln] + [0xFF] * ln)
    for ch in (1, 2):
        for attr in (0x02, 0x03, 0x01):
            send(f"fb{fb}_ch{ch}_volume_{ATTR[attr]}", [0x01, 0x08, 0xB8, 0x81, fb, attr, 0x02, ch, 0x02, 0x02, 0xFF, 0xFF])

if len(sys.argv) > 4:
    json.dump({"device": "Duet", "nodeId": node, "generation": gen, "records": records},
              open(sys.argv[4], "w"), indent=1)
