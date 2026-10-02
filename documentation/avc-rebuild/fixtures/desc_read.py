#!/usr/bin/env python3
"""Read AV/C descriptors over the ASFW MCP control plane (OPEN / READ chunks / CLOSE).

Frame shapes follow AppleFWAudio's discovery as replayed in the Orpheus fork
(AppleDiscoverySequence.cpp): OPEN 00 sub 08 type 01 FF, READ 00 sub 09 type FF FF len:2 off:2,
CLOSE 00 sub 08 type 00 FF. Reply data starts at frame byte 10.
Usage: desc_read.py <node> <gen> <guid> <subunit-hex> <specifier-hex> [...]
<specifier-hex> is the whole descriptor specifier, e.g. 80, 00, or 101800 (list by ID 0x1800,
TA 2002013 §6.2.2 Figure 25). Reply data starts at 3 + len(specifier) + 6.
"""
import json
import subprocess
import sys
from pathlib import Path

CLIENT = ".claude/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py"
OUT = Path("tmp/avc-desc")


def send(node, gen, guid, frame, log):
    args = {"targetGuid": guid, "nodeId": node, "generation": gen,
            "addressHigh": 0xFFFF, "addressLow": 0xF0000B00,
            "intent": "control", "payload": frame}
    raw = subprocess.run(["python3", CLIENT, "--allow-mutation", "call", "asfw_fcp_send_command_dev",
                          json.dumps(args)], capture_output=True, text=True).stdout
    try:
        d = json.loads(raw)["structuredContent"]
        resp = d["data"].get("response") or []
        status = d["data"].get("status")
    except Exception:
        resp, status = [], "unparsed:" + raw[:200]
    log.append({"command": frame, "response": resp, "status": status})
    h = lambda b: " ".join(f"{x:02X}" for x in b)
    print(f"  > {h(frame)}\n  < {h(resp[:16])}{' …' if len(resp) > 16 else ''} ({len(resp)} B, {status})")
    return resp


def read_descriptor(node, gen, guid, sub, spec, log):
    print(f"== subunit 0x{sub:02X} specifier {spec.hex(' ')}")
    spec = list(spec)
    data_start = 3 + len(spec) + 6
    opened = send(node, gen, guid, [0x00, sub, 0x08, *spec, 0x01, 0xFF], log)
    data = bytearray()
    try:
        if not opened or opened[0] != 0x09:  # 0x09 = ACCEPTED
            print("  OPEN not accepted; nothing to read")
            return bytes(data)
        total = None
        for _ in range(64):
            want = 0 if total is None else max(total - len(data), 0)
            off = len(data)
            r = send(node, gen, guid, [0x00, sub, 0x09, *spec, 0xFF, 0xFF,
                                       want >> 8, want & 0xFF, off >> 8, off & 0xFF], log)
            if len(r) < data_start or r[0] != 0x09:
                print("  READ not accepted; stopping")
                break
            base = 3 + len(spec)
            result_status = r[base]
            chunk_len = (r[base + 2] << 8) | r[base + 3]
            data += bytes(r[data_start:data_start + chunk_len])
            if total is None and len(data) >= 2:
                total = ((data[0] << 8) | data[1]) + 2  # length field excludes itself
            if result_status == 0x10 or (total is not None and len(data) >= total) or chunk_len == 0:
                break
    finally:
        send(node, gen, guid, [0x00, sub, 0x08, *spec, 0x00, 0xFF], log)  # always CLOSE
    print(f"  read {len(data)} bytes")
    return bytes(data)


def main():
    node, gen, guid = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3], 0)
    pairs = [(int(sys.argv[i], 16), bytes.fromhex(sys.argv[i + 1])) for i in range(4, len(sys.argv), 2)]
    OUT.mkdir(parents=True, exist_ok=True)
    result = {"node": node, "generation": gen, "guid": hex(guid), "descriptors": []}
    for sub, spec in pairs:
        log = []
        data = read_descriptor(node, gen, guid, sub, spec, log)
        result["descriptors"].append({"subunit": sub, "specifier": spec.hex(), "type": spec[0],
                                      "bytes": data.hex(), "exchanges": log})
    path = OUT / f"descriptors_{guid:016x}_{'_'.join(f'{s:02x}-{p.hex()}' for s, p in pairs)}.json"
    path.write_text(json.dumps(result, indent=1))
    print(f"saved {path}")


if __name__ == "__main__":
    main()
