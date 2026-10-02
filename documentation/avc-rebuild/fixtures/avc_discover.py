#!/usr/bin/env python3
"""Live AV/C discovery prototype: identify -> gather -> interpret, read-only, over the ASFW MCP control plane.

Every stage records its outcome (ok / not_implemented / rejected / timeout / skipped) and what it fell back to.
Nothing aborts the run except a wedged device (3 transport timeouts in a row) or a bus generation change.
Commands sent: STATUS and INQUIRY only, plus descriptor OPEN/READ/CLOSE (CONTROL, always closed).
Never: rate, clock, routing or any other CONTROL.

Frames come from our own tools, not invented here:
  tools/avc/avc_probe.py (refactor/avc-stack): UNIT/SUBUNIT/PLUG INFO, stream format C0/C1, SIGNAL SOURCE;
  desc_read.py: descriptor OPEN/READ/CLOSE; duet_fb_status.py: feature STATUS.
Interpretation reuses graph_build.py (music descriptor, audio identifier, text DB) plus Apple's slot check
(applefwaudio-graph-rules.md, "Fallback").

Usage: avc_discover.py --mcp [--guid 0x...] [--no-controls] [--force] [--dump FILE] [--out DIR]
           live device over MCP; saves a replayable dump (fixture format: device, records, descriptors)
       avc_discover.py --replay DUMP.json [DUMP.json ...]
           offline: answers from recorded exchanges (our fixtures, or a dump written by --mcp)
Exactly one of --mcp / --replay is required, so hardware is never touched by accident.
Replay: descriptors are served from their saved bytes (OPEN / READ in 142-byte chunks / CLOSE, like the planned
SimulatedAvcUnit); every other command is answered only by an exact byte match with a recorded exchange.
A command that isn't in the dump is reported as not_in_dump, never guessed.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tmp/avc-wt/tools/avc"))
from avc_probe import MCPClient  # noqa: E402
from graph_build import audio_identifier, music_descriptor, text_db, u16  # noqa: E402

SKILL = ROOT / ".claude/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py"
RC = {0x08: "not_implemented", 0x09: "accepted", 0x0A: "rejected", 0x0B: "in_transition",
      0x0C: "stable", 0x0D: "changed", 0x0F: "interim"}
OK = ("stable", "accepted")
UNSAFE_OUI = {0x000D6C: "M-Audio (freezes on unproven frames; catalog-driven only)"}
RATE = {0x00: 22050, 0x01: 24000, 0x02: 32000, 0x03: 44100, 0x04: 48000, 0x05: 96000, 0x06: 176400,
        0x07: 192000, 0x0A: 88200}  # TA 2001002 codes, as graph_build.py
MUSIC, AUDIO = 0x0C, 0x01  # subunit types (General 4.2 Table 11)
FEATURE_CTLS = ((0x01, "mute", 1), (0x02, "volume", 2))  # selector, name, data bytes (ta1394 audio)


class Wedged(Exception):
    pass


class Device:
    def __init__(self, client, node, gen, guid):
        self.c, self.node, self.gen, self.guid = client, node, gen, guid
        self.records, self.timeouts, self.descriptors = [], 0, []

    def send(self, name, frame, intent="status"):
        tool = "asfw_fcp_send_command_dev" if intent == "control" else "asfw_fcp_send_command"
        r = self.c.call_tool(tool, {"targetGuid": self.guid, "nodeId": self.node, "generation": self.gen,
                                    "addressHigh": 0xFFFF, "addressLow": 0xF0000B00,
                                    "intent": intent, "payload": frame})
        data = r.get("data") or {}
        resp = data.get("response") or []
        err = (r.get("errors") or [{}])[0].get("reason") if not r.get("ok") else None
        if resp:
            outcome = RC.get(resp[0], f"code_0x{resp[0]:02x}")
            self.timeouts = 0
        else:
            outcome = "timeout" if "timeout" in str(err or data.get("status", "")).lower() else "no_response"
            if "generation" in str(err).lower():
                raise Wedged(f"bus generation changed ({err}); refresh and rerun")
            self.timeouts += 1
        self.records.append({"name": name, "command": frame, "response": resp, "outcome": outcome,
                             "error": err})
        if self.timeouts >= 3:
            raise Wedged("3 transport timeouts in a row; stopping so the device isn't pushed further")
        return outcome, resp


class ReplayDevice(Device):
    """Answers from dumps: {'records': [{command, response}]} and {'descriptors': [{subunit, specifier, bytes}]}."""
    CHUNK = 142  # the Phase 88's READ chunk size (phase88_descriptors.md)

    def __init__(self, dumps):
        super().__init__(None, None, None, None)
        self.answers, self.desc = {}, {}
        for d in dumps:
            for r in d.get("records", []):
                if r.get("response"):
                    self.answers.setdefault(tuple(r["command"]), r["response"])
            for x in d.get("descriptors", []):
                if x.get("bytes"):
                    self.desc[(int(str(x["subunit"]), 0), x["specifier"])] = bytes.fromhex(x["bytes"])

    def answer(self, frame):
        if frame[0] == 0x00 and frame[2] in (0x08, 0x09):
            sub, op = frame[1], frame[2]
            spec = frame[3:-2] if op == 0x08 else frame[3:-6]
            data = self.desc.get((sub, bytes(spec).hex()))
            if data is not None:
                if op == 0x08:
                    return [0x09, *frame[1:]]
                want, off = u16(frame, len(frame) - 4), u16(frame, len(frame) - 2)
                chunk = data[off:off + min(want or self.CHUNK, self.CHUNK)]
                done = off + len(chunk) >= len(data)
                return [0x09, sub, 0x09, *spec, 0x10 if done else 0x11, 0xFF,
                        len(chunk) >> 8, len(chunk) & 0xFF, off >> 8, off & 0xFF, *chunk]
        return self.answers.get(tuple(frame))

    def send(self, name, frame, intent="status"):
        resp = self.answer(frame) or []
        outcome = RC.get(resp[0], f"code_0x{resp[0]:02x}") if resp else "not_in_dump"
        self.records.append({"name": name, "command": frame, "response": resp, "outcome": outcome, "error": None})
        return outcome, resp


def identify(guid_arg, force):
    raw = subprocess.run(["python3", str(SKILL), "summary"], capture_output=True, text=True).stdout
    s = json.loads(raw)
    gen = s["health"]["expectedGeneration"]
    nodes = [n for n in s["nodes"] if "avc" in n.get("protocolHints", [])]
    if guid_arg:
        nodes = [n for n in nodes if int(n["guid"], 16) == int(guid_arg, 16)]
    if not nodes:
        sys.exit("no AV/C node found (protocol hint 'avc')")
    n = nodes[0]
    guid = int(n["guid"], 16)
    oui = guid >> 40
    if oui in UNSAFE_OUI and not force:
        sys.exit(f"refusing: {UNSAFE_OUI[oui]}. Use --force only if you accept a freeze.")
    return n, gen, guid


def stage(stages, name, outcome, **detail):
    stages[name] = {"outcome": outcome, **detail}
    extra = ", ".join(f"{k}={v}" for k, v in detail.items())
    print(f"  [{outcome:15s}] {name}{' — ' + extra if extra else ''}")


def read_descriptor(dev, sub, spec):
    """OPEN / chunked READ / always CLOSE. Returns (outcome, bytes); records the result for the dump."""
    outcome, data = _read_descriptor(dev, sub, list(spec))
    dev.descriptors.append({"subunit": f"0x{sub:02X}", "specifier": bytes(spec).hex(), "outcome": outcome,
                            "bytes": data.hex()})
    return outcome, data


def _read_descriptor(dev, sub, spec):
    tag = f"desc_{sub:02x}_{bytes(spec).hex()}"
    outcome, _ = dev.send(f"{tag}_open", [0x00, sub, 0x08, *spec, 0x01, 0xFF], "control")
    data = bytearray()
    try:
        if outcome != "accepted":
            return outcome, b""
        base, total = 3 + len(spec), None
        for i in range(64):
            want = 0 if total is None else max(total - len(data), 0)
            off = len(data)
            outcome, r = dev.send(f"{tag}_read_{i}", [0x00, sub, 0x09, *spec, 0xFF, 0xFF,
                                                      want >> 8, want & 0xFF, off >> 8, off & 0xFF], "control")
            if outcome != "accepted" or len(r) < base + 6:
                return (outcome if not data else "partial"), bytes(data)
            n = (r[base + 2] << 8) | r[base + 3]
            data += bytes(r[base + 6:base + 6 + n])
            if total is None and len(data) >= 2:
                total = u16(data, 0) + 2
            if r[base] == 0x10 or (total and len(data) >= total) or n == 0:
                break
        return "ok", bytes(data)
    finally:
        dev.send(f"{tag}_close", [0x00, sub, 0x08, *spec, 0x00, 0xFF], "control")


def source_id(b0, b1):
    """Audio Subunit 1.0 source_ID: F0 n = subunit dest plug n; 80/81/82 n = selector/feature/processing n."""
    kind = {0xF0: "dest", 0x80: "Sel", 0x81: "FB", 0x82: "Proc"}.get(b0)
    if (b0, b1) == (0xFE, 0xFF) or b0 == 0xFE:
        return "not connected"
    return f"{kind} {b1}" if kind else f"{b0:02x}{b1:02x}"


def audio_source_links(buf):
    """Per configuration: which block feeds each audio-subunit source plug (the link list after the master
    cluster; same walk as graph_build.audio_identifier)."""
    if len(buf) < 8:
        return []
    p = 8 + u16(buf, 6) * buf[3] + 2 + 4
    out = []
    for _ in range(buf[p - 1]):
        q = p + 4
        q += 2 + u16(buf, q)
        n = buf[q]
        out.append([source_id(buf[q + 1 + 2 * i], buf[q + 2 + 2 * i]) for i in range(n)])
        p += 2 + u16(buf, p)
    return out


def list_children(buf, size_list_id):
    """Child list IDs of an object list (entries with attribute bit 0x20, as observed on the Phase 88)."""
    if len(buf) < 8:
        return []
    p = 6 + u16(buf, 4)
    count, p, out = u16(buf, p), p + 2, []
    for _ in range(count):
        if p + 4 > len(buf):
            break
        elen = u16(buf, p)
        if buf[p + 3] & 0x20:
            out.append(bytes(buf[p + 4:p + 4 + size_list_id]))
        p += 2 + elen
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--guid")
    ap.add_argument("--no-controls", action="store_true", help="skip STATUS confirmation of feature controls")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--out", default=str(ROOT / "tmp/avc-desc"))
    ap.add_argument("--dump", help="dump file path for --mcp (default: <out>/dump_<model>_<guid>_<time>.json)")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--mcp", action="store_true", help="discover a live device over the ASFW MCP server")
    mode.add_argument("--replay", nargs="+", metavar="DUMP")
    a = ap.parse_args()

    if a.replay:
        dumps = [json.load(open(f)) for f in a.replay]
        info = next(d["device"] for d in dumps if "device" in d)
        node = {"vendorName": info.get("vendorName"), "modelName": info.get("modelName") + " (REPLAY)",
                "nodeId": info.get("nodeId")}
        gen, guid = info.get("generation"), int(info["guid"], 16)
        dev = ReplayDevice(dumps)
    else:
        node, gen, guid = identify(a.guid, a.force)
        c = MCPClient()
        c.connect()
        dev = Device(c, node["nodeId"], gen, guid)
    print(f"== {node.get('vendorName')} {node.get('modelName')}  node {node['nodeId']}  gen {gen}  guid {guid:#018x}")
    stages, facts = {}, {"formations": {}, "signalSources": {}, "descriptors": {}, "controls": []}
    graph = {"device": node.get("modelName"), "streams": {}, "notes": []}

    try:
        # ---- unit ----------------------------------------------------------------------------
        o, r = dev.send("unit_info", [0x01, 0xFF, 0x30, 0x07, 0xFF, 0xFF, 0xFF, 0xFF])  # ta1394 form, as the probe
        stage(stages, "unit_info", "ok" if o in OK else o,
              **({"companyId": bytes(r[5:8]).hex()} if o in OK and len(r) >= 8 else {}))

        subunits = []
        o, r = dev.send("subunit_info", [0x01, 0xFF, 0x31, 0x07, 0xFF, 0xFF, 0xFF, 0xFF])
        if o in OK:
            for b in r[4:8]:
                if b != 0xFF:
                    subunits += [(b >> 3, i) for i in range((b & 7) + 1)]
        stage(stages, "subunit_info", "ok" if o in OK else o,
              subunits=[f"type{t:#04x}/id{i}" for t, i in subunits])

        o, r = dev.send("unit_plug_info", [0x01, 0xFF, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF])
        iso_in, iso_out, ext_in, ext_out = (r[4:8] if o in OK and len(r) >= 8 else [1, 1, 0, 0])
        stage(stages, "unit_plug_info", "ok" if o in OK else o, iso=f"{iso_in} in / {iso_out} out",
              ext=f"{ext_in} in / {ext_out} out", **({} if o in OK else {"fallback": "assume 1 iso in + 1 out"}))

        plugs = {}
        for t, i in subunits:
            addr = (t << 3) | i
            o, r = dev.send(f"plug_info_{addr:02x}", [0x01, addr, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF])
            if o in OK and len(r) >= 6:
                plugs[addr] = (r[4], r[5])
            stage(stages, f"plug_info_{addr:02x}", "ok" if o in OK else o,
                  **({"dest/src": f"{r[4]}/{r[5]}"} if o in OK and len(r) >= 6 else {}))

        # ---- stream formations (unit iso plugs): 0xBF first, 0x2F on NOT IMPLEMENTED, remembered ----
        opcode = 0xBF
        for d, label, count in ((0x00, "iso_in", iso_in), (0x01, "iso_out", iso_out)):
            for p in range(count):
                forms = []
                for idx in range(32):
                    o, r = dev.send(f"sf_{opcode:02x}_{label}_{p}_{idx}",
                                    [0x01, 0xFF, opcode, 0xC1, d, 0x00, 0x00, p, 0xFF, 0xFF, idx])
                    if o == "not_implemented" and idx == 0 and opcode == 0xBF:
                        opcode = 0x2F
                        graph["notes"].append("0xBF NOT IMPLEMENTED -> 0x2F for this unit (Apple's fallback)")
                        o, r = dev.send(f"sf_2f_{label}_{p}_{idx}",
                                        [0x01, 0xFF, 0x2F, 0xC1, d, 0x00, 0x00, p, 0xFF, 0xFF, idx])
                    if o == "stable" and len(r) >= 14 and r[11:14] == [0x90, 0x00, 0x40]:
                        forms.append({"sync": True})  # AM824 single, sync stream (e.g. Phase 88 iso plug 1)
                        break
                    if o != "stable" or len(r) < 16 or r[11:13] != [0x90, 0x40]:
                        break
                    pcm = midi = 0
                    for k in range(r[15]):  # compound AM824: [rate][rate ctl][n] then (count, format) pairs
                        cnt, fmt = r[16 + 2 * k], r[17 + 2 * k]
                        midi, pcm = (midi + cnt, pcm) if fmt == 0x0D else (midi, pcm + cnt)
                    forms.append({"rate": RATE.get(r[13], hex(r[13])), "pcm": pcm, "midi": midi,
                                  "dbs": pcm + (midi + 7) // 8})
                facts["formations"][f"{label}_{p}"] = forms
                stage(stages, f"formations_{label}_{p}", "ok" if forms else o, opcode=f"{opcode:#04x}",
                      formations=["sync stream" if f.get("sync") else f"{f['rate']}:{f['pcm']}+{f['midi']}"
                                  for f in forms])

        # ---- SIGNAL SOURCE: who feeds each destination (read-only) ----
        dests = [(f"unit_iso_out_{p}", [0xFF, p]) for p in range(iso_out)]
        dests += [(f"unit_ext_out_{p}", [0xFF, 0x80 + p]) for p in range(ext_out)]
        for addr, (ndest, _) in plugs.items():
            dests += [(f"sub_{addr:02x}_dest_{p}", [addr, p]) for p in range(ndest)]
        for name, dst in dests:
            o, r = dev.send(f"signal_source_{name}", [0x01, 0xFF, 0x1A, 0xFF, 0xFF, 0xFE, *dst])
            if o in OK and len(r) >= 8:
                facts["signalSources"][name] = bytes(r[4:6]).hex()
        ss = facts["signalSources"]
        stage(stages, "signal_source", "ok" if ss else "not_implemented", answered=f"{len(ss)}/{len(dests)}")

        # ---- descriptors ----
        music = next((((t << 3) | i) for t, i in subunits if t == MUSIC), None)
        audio = next((((t << 3) | i) for t, i in subunits if t == AUDIO), None)
        if music is not None:
            o, b = read_descriptor(dev, music, [0x80])
            facts["descriptors"]["music_80"] = b.hex()
            stage(stages, "music_status_descriptor", o, bytes=len(b))
        else:
            stage(stages, "music_status_descriptor", "skipped", reason="no music subunit")
        names = []
        if audio is not None:
            o, b = read_descriptor(dev, audio, [0x00])
            facts["descriptors"]["audio_00"] = b.hex()
            stage(stages, "audio_identifier_descriptor", o, bytes=len(b))
            if b and len(b) >= 8:
                size_id, nroot = b[3], u16(b, 6)
                roots = [bytes(b[8 + k * size_id:8 + (k + 1) * size_id]) for k in range(nroot)]
                todo, leaves = list(roots), []
                while todo and len(leaves) < 4:
                    lid = todo.pop(0)
                    o, lb = read_descriptor(dev, audio, [0x10, *lid])
                    facts["descriptors"][f"audio_list_{lid.hex()}"] = lb.hex()
                    kids = list_children(lb, size_id)
                    todo += kids
                    if lb and not kids:
                        leaves.append(lb)
                for lb in leaves:
                    names += text_db(lb)
                stage(stages, "audio_text_database", "ok" if names else "not_found", roots=len(roots),
                      names=len(names))
        else:
            stage(stages, "audio_identifier_descriptor", "skipped", reason="no audio subunit")

        # ---- interpret ----
        m = music_descriptor(bytes.fromhex(facts["descriptors"]["music_80"])) \
            if facts["descriptors"].get("music_80") else None
        fbs = audio_identifier(bytes.fromhex(facts["descriptors"]["audio_00"]), names) \
            if facts["descriptors"].get("audio_00") else []

        # ---- confirm feature controls with STATUS (descriptor bitmaps are hints only) ----
        if a.no_controls or audio is None:
            stage(stages, "feature_controls", "skipped", reason="--no-controls" if a.no_controls else "no audio")
        else:
            for fb in (f for f in fbs if f["kind"] == "feature"):
                for ch in range(0, (fb["channels"] or 0) + 1):
                    for sel, cname, width in FEATURE_CTLS:
                        o, r = dev.send(f"fb{fb['id']}_ch{ch}_{cname}",
                                        [0x01, audio, 0xB8, 0x81, fb["id"], 0x10, 0x02, ch, sel, width]
                                        + [0xFF] * width)
                        if o == "stable":
                            facts["controls"].append({"fb": fb["id"], "name": fb["name"], "channel": ch,
                                                      "control": cname, "value": bytes(r[10:10 + width]).hex()})
            asked = [r["outcome"] for r in dev.records if r["name"].startswith("fb")]
            stage(stages, "feature_controls", "ok" if facts["controls"] else "not_implemented",
                  confirmed=len(facts["controls"]), asked=len(asked),
                  **{k: asked.count(k) for k in set(asked) if k != "stable"})

        def pick(direction, unit_plug_name, fallback_index):
            src = ss.get(unit_plug_name)
            plugs_ = (m["sourcePlugs"] if direction == "capture" else m["destPlugs"]) if m else []
            if direction == "capture" and src and music is not None and int(src[:2], 16) == music:
                return next((p for p in plugs_ if p["id"] == int(src[2:], 16)), None), "SIGNAL SOURCE"
            if direction == "playback":
                for name, s in ss.items():
                    if s == "ff00" and music is not None and name.startswith(f"sub_{music:02x}_dest_"):
                        pid = int(name.rsplit("_", 1)[1])
                        return next((p for p in plugs_ if p["id"] == pid), None), "SIGNAL SOURCE"
            return (plugs_[fallback_index] if len(plugs_) > fallback_index else None), "ASSUMED plug 0"

        for direction, unit_name, forms_key in (("playback", None, "iso_in_0"), ("capture", "unit_iso_out_0", "iso_out_0")):
            plug, how = pick(direction, unit_name, 0)
            forms = facts["formations"].get(forms_key, [])
            dbs = max((f["dbs"] for f in forms if "dbs" in f), default=None)
            if plug:
                slots = [s["position"] for c_ in plug["clusters"] for s in c_["signals"]]
                ok = dbs is None or all(x < dbs for x in slots)
                entry = {"plug": plug["id"], "found_by": how, "slot_map": slots if ok else "identity",
                         "clusters": [f"{c_['name']} {c_['format']}/{c_['port']} x{len(c_['signals'])}"
                                      for c_ in plug["clusters"]]}
                if not ok:
                    graph["notes"].append(f"{direction}: descriptor slot >= DBS {dbs}; identity map (Apple rule)")
            else:
                entry = {"plug": None, "found_by": "no music descriptor", "slot_map": "identity",
                         "clusters": [], "formations_only": forms}
            graph["streams"][direction] = entry
        graph["rates"] = sorted({f["rate"] for f in facts["formations"].get("iso_in_0", []) if "rate" in f}, key=lambda x: (isinstance(x, str), str(x).zfill(8)))
        graph["controls"] = [f"{f['kind']} {f['id']} {f['name'] or '(unnamed)'} <- "
                             f"{[source_id(int(x[:2], 16), int(x[2:], 16)) for x in f['inputs']]}" for f in fbs]
        links = audio_source_links(bytes.fromhex(facts["descriptors"]["audio_00"])) \
            if facts["descriptors"].get("audio_00") else []
        graph["audioSourcePlugs"] = [{f"src {i}": who for i, who in enumerate(cfg)} for cfg in links]
        graph["confirmedControls"] = facts["controls"]
    except Wedged as e:
        stage(stages, "ABORTED", "wedged", reason=str(e))

    print("\n== graph")
    print(json.dumps(graph, indent=1))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    path = out / f"discover_{guid:016x}{'_replay' if a.replay else ''}_{time.strftime('%Y%m%d_%H%M%S')}.json"
    path.write_text(json.dumps({"device": {k: node.get(k) for k in ("vendorName", "modelName", "nodeId")}
                                | {"guid": f"{guid:#018x}", "generation": gen},
                                "stages": stages, "facts": facts, "graph": graph, "records": dev.records},
                               indent=1))
    print(f"\nsaved {path} ({len(dev.records)} exchanges)")
    if a.mcp:
        # Replayable dump, same shape as documentation/avc-rebuild/fixtures/*_descriptors.json.
        stamp = time.strftime("%Y%m%d_%H%M%S")
        model = "".join(ch if ch.isalnum() else "_" for ch in (node.get("modelName") or "device")).strip("_")
        dump = Path(a.dump) if a.dump else out / f"dump_{model}_{guid:016x}_{stamp}.json"
        dump.write_text(json.dumps({
            "device": {"modelName": node.get("modelName"), "vendorName": node.get("vendorName"),
                       "guid": f"{guid:#018x}", "nodeId": node.get("nodeId"), "generation": gen,
                       "capturedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "tool": "avc_discover.py --mcp"},
            "records": [{"name": r["name"], "command": r["command"], "response": r["response"],
                         "responseCode": r["response"][0] if r["response"] else None,
                         "outcome": r["outcome"], "error": r["error"]} for r in dev.records],
            "descriptors": dev.descriptors}, indent=1))
        print(f"dump  {dump}  (replay: avc_discover.py --replay {dump})")


if __name__ == "__main__":
    main()
