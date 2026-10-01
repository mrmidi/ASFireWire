#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""AV/C device discovery for ASFireWire: identify -> gather -> interpret, read-only.

Builds a device capability graph (streams, slot map, channel names, rates, routing, clock, controls) from the
device's own answers, the way AppleFWAudio did, and saves a replayable dump you can share with the ASFW
developers. Standalone: Python 3.10+ standard library only.

Requirements (live mode): the ASFireWire app running, with the MCP Control Plane enabled (Settings). Descriptor
reads use the developer FCP tool; if the app's MCP policy doesn't allow it, those stages report "refused" and
the rest still runs.

Usage:
  python3 avc_discover.py --mcp [--guid 0x...] [--no-controls] [--strip-guid] [--out DIR] [--dump FILE]
  python3 avc_discover.py --replay DUMP.json [DUMP.json ...]
      DUMP may also be an ASFW AV/C Report: its snapshot.json (pick a device with --guid when it
      holds several) or a per-device <GUID>-fcp-exchanges.json from "Save Binary Dump Folder".
  uv run avc_discover.py --mcp ...                     (same thing through uv)
Exactly one of --mcp / --replay is required, so hardware is never touched by accident.
Endpoint: --endpoint, else $ASFW_MCP_ENDPOINT, else http://127.0.0.1:8766/mcp.

Safety (live): STATUS and INQUIRY only, plus descriptor OPEN/READ/CLOSE (CONTROL, always closed). Never rate,
clock, routing or any other CONTROL. M-Audio units are refused unless --force (their firmware freezes on
unproven frames). The run stops after 3 transport timeouts in a row or a bus generation change.

Every stage records ok / not_implemented / rejected / refused / timeout / skipped and any fallback used.
Replay serves descriptors from saved bytes (OPEN / READ in 142-byte chunks / CLOSE) and answers every other
command only by an exact byte match with a recorded exchange; anything else is reported as not_in_dump.

Frame and layout sources: ta1394 (Linux userspace) general/ccm/stream-format/audio crates; TA 2002013
(descriptors), TA 1999045 (info blocks), Audio Subunit 1.0 Tables 8.1/8.3; AppleFWAudio graph rules
(slot map = cluster stream positions, rejected if any slot >= data-block size).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

DEFAULT_ENDPOINT = "http://127.0.0.1:8766/mcp"
PROTOCOL_VERSION = "2025-11-25"
RC = {0x08: "not_implemented", 0x09: "accepted", 0x0A: "rejected", 0x0B: "in_transition",
      0x0C: "stable", 0x0D: "changed", 0x0F: "interim"}
OK = ("stable", "accepted")
UNSAFE_OUI = {0x000D6C: "M-Audio (firmware freezes on unproven frames; supported from the catalog only)"}
RATE = {0x00: 22050, 0x01: 24000, 0x02: 32000, 0x03: 44100, 0x04: 48000, 0x05: 96000, 0x06: 176400,
        0x07: 192000, 0x0A: 88200}  # stream-format rate codes (TA 2001002)
PORT = {0x00: "speaker", 0x03: "line", 0x04: "spdif", 0x09: "digital", 0x0A: "midi"}
FMT = {0x00: "IEC60958", 0x06: "MBLA", 0x0D: "MIDI", 0x40: "sync"}
MUSIC, AUDIO = 0x0C, 0x01  # subunit types (AV/C General 4.2 Table 11)
FEATURE_CTLS = ((0x01, "mute", 1), (0x02, "volume", 2))  # control selector, name, data bytes


def u16(b, i):
    return (b[i] << 8) | b[i + 1]


# ---------------------------------------------------------------------------------------------------------
# MCP client (JSON-RPC over HTTP; JSON or SSE bodies)
# ---------------------------------------------------------------------------------------------------------

class MCPError(RuntimeError):
    pass


class MCPClient:
    def __init__(self, endpoint: str, timeout: float = 5.0) -> None:
        self.endpoint, self.timeout, self.session_id, self.request_id = endpoint, timeout, None, 0

    def connect(self) -> None:
        self.request("initialize", {"protocolVersion": PROTOCOL_VERSION, "capabilities": {},
                                    "clientInfo": {"name": "avc_discover", "version": "1"}}, initialize=True)

    def request(self, method: str, params: Any = None, initialize: bool = False) -> dict:
        self.request_id += 1
        payload = {"jsonrpc": "2.0", "id": self.request_id, "method": method}
        if params is not None:
            payload["params"] = params
        headers = {"Accept": "application/json, text/event-stream", "Content-Type": "application/json",
                   "MCP-Protocol-Version": PROTOCOL_VERSION}
        if self.session_id and not initialize:
            headers["MCP-Session-Id"] = self.session_id
        req = urllib.request.Request(self.endpoint, data=json.dumps(payload).encode(), headers=headers,
                                     method="POST")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                if initialize:
                    self.session_id = resp.headers.get("MCP-Session-Id")
                body = resp.read().decode()
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace")
            existing = e.headers.get("MCP-Session-Id")  # ASFW hosts one session; reuse it
            if initialize and e.code == 400 and "Session already initialized" in detail and existing:
                self.session_id = existing
                return {"serverInfo": {}}
            raise MCPError(f"MCP HTTP {e.code}: {detail}") from e
        except urllib.error.URLError as e:
            raise MCPError(f"cannot reach {self.endpoint}: {e.reason} (is the ASFW app running with MCP "
                           f"enabled? set --endpoint if you changed the port)") from e
        if not body.strip():
            return {}
        text = body.strip()
        if not text.startswith("{"):
            text = "\n".join(line[5:].strip() for line in body.splitlines() if line.startswith("data:"))
        msg = json.loads(text)
        if "error" in msg:
            raise MCPError(f"MCP error {msg['error'].get('code')}: {msg['error'].get('message')}")
        return msg.get("result", {})

    def call_tool(self, name: str, arguments: dict) -> dict:
        return self.request("tools/call", {"name": name, "arguments": arguments}).get("structuredContent", {})

    def read_resource(self, uri: str) -> Any:
        contents = self.request("resources/read", {"uri": uri}).get("contents", [])
        text = contents[0].get("text") if contents and isinstance(contents[0], dict) else None
        try:
            value = json.loads(text) if isinstance(text, str) else contents
        except json.JSONDecodeError:
            return text
        return value["data"] if isinstance(value, dict) and isinstance(value.get("data"), dict) else value


# ---------------------------------------------------------------------------------------------------------
# Devices: live (MCP) and replay (dumps)
# ---------------------------------------------------------------------------------------------------------

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
        elif "generation" in str(err).lower():
            raise Wedged(f"bus generation changed ({err}); rerun")
        elif "timeout" in str(err or data.get("status", "")).lower():
            outcome = "timeout"
            self.timeouts += 1
        else:
            outcome = "refused"  # server policy or tool error; not a device fault, doesn't count as a timeout
        self.records.append({"name": name, "command": frame, "response": resp, "outcome": outcome, "error": err})
        if self.timeouts >= 3:
            raise Wedged("3 transport timeouts in a row; stopping so the device isn't pushed further")
        return outcome, resp


def quadlet_padded(frame):
    """The driver pads FCP commands to whole quadlets with zeros; the tool does not."""
    return tuple(frame) + (0,) * (-len(frame) % 4)


def descriptor_chunk(command, response):
    """(subunit, specifier hex, offset, data) from a READ DESCRIPTOR exchange, else None.

    Layout after the opcode: specifier, read result status, reserved, data length (2), address (2),
    then the data. The specifier's length isn't in the frame, so take the shortest one for which the
    reply's length and address fields agree with what follows."""
    if len(command) < 10 or command[2] != 0x09 or not response or response[0] != 0x09:
        return None
    for spec_len in range(1, 13):
        head = 3 + spec_len + 6
        if len(response) < head or list(command[3:3 + spec_len]) != list(response[3:3 + spec_len]):
            continue
        length = u16(response, 3 + spec_len + 2)
        if 0 <= len(response) - head - length < 4 and list(command[3 + spec_len + 4:head]) == list(response[3 + spec_len + 4:head]):
            return (response[1], bytes(response[3:3 + spec_len]).hex(), u16(response, 3 + spec_len + 4),
                    bytes(response[head:head + length]))
    return None


def expand_report(d, guid_arg):
    """An ASFW AV/C Report snapshot -> one device's dump, in this tool's own format."""
    devices = [x for x in d["devices"] if x.get("exchanges")]
    if guid_arg:
        devices = [x for x in devices if int(x["guid"]) == int(guid_arg, 16)]
    if len(devices) != 1:
        listing = ", ".join(f"{int(x['guid']):#018x} {x.get('modelName')}" for x in d["devices"])
        sys.exit(f"pick one device with --guid: {listing or 'the report has no exchange logs'}")
    x = devices[0]
    return {"device": {"vendorName": x.get("vendorName"), "modelName": x.get("modelName"),
                       "guid": f"{int(x['guid']):#018x}", "nodeId": x.get("nodeID"),
                       "generation": x.get("generation")},
            "records": x["exchanges"]["records"]}


class ReplayDevice(Device):
    CHUNK = 142  # the Phase 88's descriptor READ chunk size

    def __init__(self, dumps):
        super().__init__(None, None, None, None)
        self.answers, self.desc = {}, {}
        for d in dumps:
            for r in d.get("records", []):
                if r.get("response"):
                    self.answers.setdefault(tuple(r["command"]), r["response"])
                    self.answers.setdefault(quadlet_padded(r["command"]), r["response"])
                    chunk = descriptor_chunk(r["command"], r["response"])
                    if chunk:
                        sub, spec, off, data = chunk
                        whole = bytearray(self.desc.get((sub, spec), b""))
                        whole.extend(bytes(max(0, off + len(data) - len(whole))))
                        whole[off:off + len(data)] = data
                        self.desc[(sub, spec)] = bytes(whole)
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
        return self.answers.get(tuple(frame)) or self.answers.get(quadlet_padded(frame))

    def send(self, name, frame, intent="status"):
        resp = self.answer(frame) or []
        outcome = RC.get(resp[0], f"code_0x{resp[0]:02x}") if resp else "not_in_dump"
        self.records.append({"name": name, "command": frame, "response": resp, "outcome": outcome, "error": None})
        return outcome, resp


def identify(client, guid_arg, force):
    """Node list + expected generation from the server's advertised resources (as the ASFW MCP skill does)."""
    resources = client.request("resources/list").get("resources", [])
    uri = lambda frag: next((r["uri"] for r in resources if frag in str(r.get("uri", "")).lower()), None)
    health = client.read_resource(uri("control-plane/health")) if uri("control-plane/health") else {}
    nodes_v = client.read_resource(uri("nodes")) if uri("nodes") else {}
    nodes = nodes_v.get("nodes", nodes_v) if isinstance(nodes_v, dict) else nodes_v
    gen = health.get("expectedGeneration") if isinstance(health, dict) else None
    if gen is None:
        sys.exit("the MCP server did not report a bus generation; is a device connected and the driver running?")
    avc = [n for n in nodes or [] if "avc" in n.get("protocolHints", [])]
    if guid_arg:
        avc = [n for n in avc if int(str(n["guid"]), 16) == int(guid_arg, 16)]
    if not avc:
        sys.exit("no AV/C device found (DICE, MOTU, RME and Fireworks devices are not AV/C)")
    n = avc[0]
    guid = int(str(n["guid"]), 16)
    if (guid >> 40) in UNSAFE_OUI and not force:
        sys.exit(f"refusing: {UNSAFE_OUI[guid >> 40]}. Use --force only if you accept a possible freeze.")
    return n, gen, guid


# ---------------------------------------------------------------------------------------------------------
# Parsers (info blocks TA 1999045; audio identifier descriptor Audio Subunit 1.0 Table 8.1)
# ---------------------------------------------------------------------------------------------------------

def blocks(buf, start, end):
    """Yield (type, primary, children_start, block_end) for sibling info blocks."""
    pos = start
    while pos + 6 <= end:
        clen, btype, plen = u16(buf, pos), u16(buf, pos + 2), u16(buf, pos + 4)
        yield btype, buf[pos + 6:pos + 6 + plen], pos + 6 + plen, pos + 2 + clen
        pos += 2 + clen


def name_in(buf, start, end):
    """First Raw Text (000A) under [start, end), directly or inside Name Info (000B)."""
    for btype, prim, cs, ce in blocks(buf, start, end):
        if btype == 0x000A:
            return prim.split(b"\0")[0].decode("ascii", "replace")
        if btype == 0x000B:
            n = name_in(buf, cs, ce) or name_in(prim, 4, len(prim))
            if n:
                return n
    return None


def music_descriptor(buf):
    """Music subunit status descriptor: dest/source plugs with clusters (8108/8109/810A), music plugs (810B)."""
    g = {"destPlugs": [], "sourcePlugs": [], "musicPlugs": [], "plugChannelNames": {}}
    for btype, prim, cs, ce in blocks(buf, 2, len(buf)):
        if btype == 0x8101:
            for t, p, pcs, pce in blocks(buf, cs, ce):
                if t != 0x8102:
                    continue
                for at, ap, acs, ace in blocks(buf, pcs, pce):
                    if at == 0x8103:
                        text = name_in(buf, acs, ace) or ""
                        g["plugChannelNames"][p[0]] = [x for x in text.replace("\r", "").split("\n") if x]
        if btype != 0x8108:
            continue
        ndest, nsrc = prim[0], prim[1]
        plugs = []
        for t, p, pcs, pce in blocks(buf, cs, ce):
            if t == 0x8109:
                clusters = []
                for ct, cp, ccs, cce in blocks(buf, pcs, pce):
                    if ct != 0x810A:
                        continue
                    sig = [{"musicPlug": u16(cp, 3 + 4 * i), "position": cp[5 + 4 * i], "location": cp[6 + 4 * i]}
                           for i in range(cp[2])]
                    clusters.append({"name": name_in(buf, ccs, cce), "format": FMT.get(cp[0], hex(cp[0])),
                                     "port": PORT.get(cp[1], hex(cp[1])), "signals": sig})
                plugs.append({"id": p[0], "channels": u16(p, 6), "clusters": clusters})
            elif t == 0x810B:  # type, id:2, routing, F0 srcPlug FF pos loc, F1 dstPlug FF pos loc
                g["musicPlugs"].append({"id": u16(p, 1), "type": p[0], "name": name_in(buf, pcs, pce),
                                        "from": {"destPlug": p[5], "position": p[7]},
                                        "to": {"sourcePlug": p[10], "position": p[12]}})
        g["destPlugs"], g["sourcePlugs"] = plugs[:ndest], plugs[ndest:ndest + nsrc]
    return g


def text_db(buf):
    """Strings of a text-database list, in entry order (indexed by function_block_name)."""
    if not buf or len(buf) < 8:
        return []
    n, p, out = u16(buf, 6 + u16(buf, 4)), 8 + u16(buf, 4), []
    for _ in range(n):
        e = buf[p:p + 2 + u16(buf, p)]
        runs, cur = [], ""
        for ch in e[4:]:
            cur = cur + chr(ch) if 32 <= ch < 127 else (runs.append(cur) or "")
        out.append(max(runs + [cur], key=len))
        p += len(e)
    return out


def source_id(b0, b1):
    """source_ID: F0 n = subunit dest plug n; 80/81/82 n = selector/feature/processing block n; FE = none."""
    if b0 == 0xFE:
        return "not connected"
    kind = {0xF0: "dest", 0x80: "Sel", 0x81: "FB", 0x82: "Proc"}.get(b0)
    return f"{kind} {b1}" if kind else f"{b0:02x}{b1:02x}"


def audio_identifier(buf, names):
    """Function blocks of every configuration, plus which block feeds each audio source plug."""
    if not buf or len(buf) < 8:
        return [], []
    p = 8 + u16(buf, 6) * buf[3] + 2 + 4
    fbs, links = [], []
    for _ in range(buf[p - 1]):  # configurations
        q = p + 4
        q += 2 + u16(buf, q)                   # master cluster
        links.append([source_id(buf[q + 1 + 2 * i], buf[q + 2 + 2 * i]) for i in range(buf[q])])
        q += 1 + 2 * buf[q]                    # subunit source plug links
        nfb, q = buf[q], q + 1
        for _ in range(nfb):
            flen, ft, fid, nm, npl = u16(buf, q), buf[q + 2], buf[q + 3], u16(buf, q + 4), buf[q + 6]
            srcs = [source_id(buf[q + 7 + 2 * i], buf[q + 8 + 2 * i]) for i in range(npl)]
            r = q + 7 + 2 * npl
            cl = buf[r + 2:r + 2 + u16(buf, r)]
            kind = {0x80: "selector", 0x81: "feature", 0x82: "processing"}.get(ft, hex(ft))
            fbs.append({"kind": kind, "id": fid, "name": names[nm] if nm < len(names) else None,
                        "inputs": srcs, "channels": cl[0] if cl else None})
            q += 2 + flen
        p += 2 + u16(buf, p)
    return fbs, links


def list_children(buf, size_list_id):
    """Child list IDs of an object list (entry attribute bit 0x20; observed on the Phase 88)."""
    if len(buf) < 8:
        return []
    p = 6 + u16(buf, 4)
    count, p, out = u16(buf, p), p + 2, []
    for _ in range(count):
        if p + 4 > len(buf):
            break
        if buf[p + 3] & 0x20:
            out.append(bytes(buf[p + 4:p + 4 + size_list_id]))
        p += 2 + u16(buf, p)
    return out


# ---------------------------------------------------------------------------------------------------------
# Gather + interpret
# ---------------------------------------------------------------------------------------------------------

def stage(stages, name, outcome, **detail):
    stages[name] = {"outcome": outcome, **detail}
    extra = ", ".join(f"{k}={v}" for k, v in detail.items())
    print(f"  [{outcome:15s}] {name}{' — ' + extra if extra else ''}")


def read_descriptor(dev, sub, spec):
    """OPEN / chunked READ / always CLOSE (TA 2002013). Records the result for the dump."""
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
            n = u16(r, base + 2)
            data += bytes(r[base + 6:base + 6 + n])
            if total is None and len(data) >= 2:
                total = u16(data, 0) + 2
            if r[base] == 0x10 or (total and len(data) >= total) or n == 0:
                break
        return "ok", bytes(data)
    finally:
        dev.send(f"{tag}_close", [0x00, sub, 0x08, *spec, 0x00, 0xFF], "control")


def discover(dev, no_controls, graph, stages, facts):
    o, r = dev.send("unit_info", [0x01, 0xFF, 0x30, 0x07, 0xFF, 0xFF, 0xFF, 0xFF])
    stage(stages, "unit_info", "ok" if o in OK else o,
          **({"companyId": bytes(r[5:8]).hex()} if o in OK and len(r) >= 8 else {}))

    subunits = []
    o, r = dev.send("subunit_info", [0x01, 0xFF, 0x31, 0x07, 0xFF, 0xFF, 0xFF, 0xFF])
    if o in OK:
        for b in r[4:8]:
            if b != 0xFF:
                subunits += [(b >> 3, i) for i in range((b & 7) + 1)]
    stage(stages, "subunit_info", "ok" if o in OK else o, subunits=[f"type{t:#04x}/id{i}" for t, i in subunits])

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

    # Stream formations on unit iso plugs: 0xBF first, 0x2F on NOT IMPLEMENTED, remembered (Apple's fallback).
    opcode = 0xBF
    for d, label, count in ((0x00, "iso_in", iso_in), (0x01, "iso_out", iso_out)):
        for p in range(count):
            forms = []
            for idx in range(32):
                o, r = dev.send(f"sf_{opcode:02x}_{label}_{p}_{idx}",
                                [0x01, 0xFF, opcode, 0xC1, d, 0x00, 0x00, p, 0xFF, 0xFF, idx])
                if o == "not_implemented" and idx == 0 and opcode == 0xBF:
                    opcode = 0x2F
                    graph["notes"].append("0xBF NOT IMPLEMENTED -> 0x2F for this unit")
                    o, r = dev.send(f"sf_2f_{label}_{p}_{idx}",
                                    [0x01, 0xFF, 0x2F, 0xC1, d, 0x00, 0x00, p, 0xFF, 0xFF, idx])
                if o == "stable" and len(r) >= 14 and r[11:14] == [0x90, 0x00, 0x40]:
                    forms.append({"sync": True})  # AM824 single, sync stream
                    break
                if o != "stable" or len(r) < 16 or r[11:13] != [0x90, 0x40]:
                    break  # end of list (REJECTED) or not compound AM824
                pcm = midi = 0
                for k in range(r[15]):  # compound AM824: rate, rate ctl, n, then (count, format) pairs
                    cnt, fmt = r[16 + 2 * k], r[17 + 2 * k]
                    midi, pcm = (midi + cnt, pcm) if fmt == 0x0D else (midi, pcm + cnt)
                forms.append({"rate": RATE.get(r[13], hex(r[13])), "pcm": pcm, "midi": midi,
                              "dbs": pcm + (midi + 7) // 8})
            facts["formations"][f"{label}_{p}"] = forms
            stage(stages, f"formations_{label}_{p}", "ok" if forms else o, opcode=f"{opcode:#04x}",
                  formations=["sync stream" if f.get("sync") else f"{f['rate']}:{f['pcm']}+{f['midi']}"
                              for f in forms])

    # SIGNAL SOURCE (status): what feeds each destination.
    dests = [(f"unit_iso_out_{p}", [0xFF, p]) for p in range(iso_out)]
    dests += [(f"unit_ext_out_{p}", [0xFF, 0x80 + p]) for p in range(ext_out)]
    for addr, (ndest, _) in plugs.items():
        dests += [(f"sub_{addr:02x}_dest_{p}", [addr, p]) for p in range(ndest)]
    ss = facts["signalSources"]
    for name, dst in dests:
        o, r = dev.send(f"signal_source_{name}", [0x01, 0xFF, 0x1A, 0xFF, 0xFF, 0xFE, *dst])
        if o in OK and len(r) >= 8:
            ss[name] = bytes(r[4:6]).hex()
    stage(stages, "signal_source", "ok" if ss else "not_implemented", answered=f"{len(ss)}/{len(dests)}")

    # Descriptors.
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
            todo = [bytes(b[8 + k * size_id:8 + (k + 1) * size_id]) for k in range(nroot)]
            leaves = []
            while todo and len(leaves) < 4:
                lid = todo.pop(0)
                o, lb = read_descriptor(dev, audio, [0x10, *lid])
                kids = list_children(lb, size_id)
                todo += kids
                if lb and not kids:
                    leaves.append(lb)
            for lb in leaves:
                names += text_db(lb)
            stage(stages, "audio_text_database", "ok" if names else "not_found", roots=nroot, names=len(names))
    else:
        stage(stages, "audio_identifier_descriptor", "skipped", reason="no audio subunit")

    m = music_descriptor(bytes.fromhex(facts["descriptors"]["music_80"])) \
        if facts["descriptors"].get("music_80") else None
    fbs, links = audio_identifier(bytes.fromhex(facts["descriptors"].get("audio_00", "")), names)

    # Confirm feature controls with STATUS: descriptor control bitmaps are hints only.
    if no_controls or audio is None:
        stage(stages, "feature_controls", "skipped", reason="--no-controls" if no_controls else "no audio subunit")
    else:
        for fb in (f for f in fbs if f["kind"] == "feature"):
            for ch in range(0, (fb["channels"] or 0) + 1):
                for sel, cname, width in FEATURE_CTLS:
                    o, r = dev.send(f"fb{fb['id']}_ch{ch}_{cname}",
                                    [0x01, audio, 0xB8, 0x81, fb["id"], 0x10, 0x02, ch, sel, width] + [0xFF] * width)
                    if o == "stable":
                        facts["controls"].append({"fb": fb["id"], "name": fb["name"], "channel": ch,
                                                  "control": cname, "value": bytes(r[10:10 + width]).hex()})
        asked = [x["outcome"] for x in dev.records if x["name"].startswith("fb")]
        stage(stages, "feature_controls", "ok" if facts["controls"] else "not_implemented",
              confirmed=len(facts["controls"]), asked=len(asked),
              **{k: asked.count(k) for k in set(asked) if k != "stable"})

    # Interpret: streams. Playback = the music dest plug fed by unit iso-in 0; capture = what feeds iso-out 0.
    def pick(direction):
        plugs_ = (m["sourcePlugs"] if direction == "capture" else m["destPlugs"]) if m else []
        if direction == "capture":
            src = ss.get("unit_iso_out_0")
            if src and music is not None and int(src[:2], 16) == music:
                return next((p for p in plugs_ if p["id"] == int(src[2:], 16)), None), "SIGNAL SOURCE"
        else:
            for name, s in ss.items():
                if s == "ff00" and music is not None and name.startswith(f"sub_{music:02x}_dest_"):
                    pid = int(name.rsplit("_", 1)[1])
                    return next((p for p in plugs_ if p["id"] == pid), None), "SIGNAL SOURCE"
        return (plugs_[0] if plugs_ else None), "ASSUMED plug 0"

    for direction, forms_key in (("playback", "iso_in_0"), ("capture", "iso_out_0")):
        plug, how = pick(direction)
        forms = facts["formations"].get(forms_key, [])
        dbs = max((f["dbs"] for f in forms if "dbs" in f), default=None)
        if plug:
            slots = [s["position"] for c in plug["clusters"] for s in c["signals"]]
            ok = dbs is None or all(x < dbs for x in slots)
            names_ = {mp["id"]: mp["name"] for mp in m["musicPlugs"]}
            ch_names = [names_.get(s["musicPlug"]) or c["name"] for c in plug["clusters"]
                        for s in c["signals"] if c["format"] != "MIDI"]
            graph["streams"][direction] = {
                "plug": plug["id"], "found_by": how, "slot_map": slots if ok else "identity",
                "clusters": [f"{c['name']} {c['format']}/{c['port']} x{len(c['signals'])}" for c in plug["clusters"]],
                "channelNames": ch_names}
            if not ok:
                graph["notes"].append(f"{direction}: a descriptor slot >= DBS {dbs}; identity map (Apple's rule)")
        else:
            graph["streams"][direction] = {"plug": None, "found_by": "no music descriptor", "slot_map": "identity",
                                           "formations": forms}
    graph["rates"] = sorted({f["rate"] for f in facts["formations"].get("iso_in_0", []) if isinstance(f.get("rate"), int)})
    graph["functionBlocks"] = [f"{f['kind']} {f['id']} {f['name'] or '(unnamed)'} <- {f['inputs']}" for f in fbs]
    graph["audioSourcePlugs"] = [{f"src {i}": who for i, who in enumerate(cfg)} for cfg in links]
    graph["confirmedControls"] = facts["controls"]
    if m:
        graph["syncPlugs"] = {f"music dest {p['id']}": ss.get(f"sub_{music:02x}_dest_{p['id']}")
                              for p in m["destPlugs"] if any(c["format"] == "sync" for c in p["clusters"])}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--mcp", action="store_true", help="discover a live device over the ASFW MCP server")
    mode.add_argument("--replay", nargs="+", metavar="DUMP", help="replay one or more dump/fixture files")
    ap.add_argument("--endpoint", default=os.environ.get("ASFW_MCP_ENDPOINT", DEFAULT_ENDPOINT))
    ap.add_argument("--guid", help="pick this device when several AV/C devices are connected")
    ap.add_argument("--no-controls", action="store_true", help="skip STATUS confirmation of feature controls")
    ap.add_argument("--strip-guid", action="store_true", help="keep only the vendor ID part of the GUID in files")
    ap.add_argument("--force", action="store_true", help="allow devices known to freeze (M-Audio)")
    ap.add_argument("--out", default="avc-dumps", help="output directory (default ./avc-dumps)")
    ap.add_argument("--dump", help="dump file path for --mcp")
    a = ap.parse_args()

    if a.replay:
        dumps = [json.load(open(f)) for f in a.replay]
        dumps = [expand_report(d, a.guid) if "devices" in d else d for d in dumps]
        info = next(d["device"] for d in dumps if "device" in d)
        node = {"vendorName": info.get("vendorName"), "modelName": f"{info.get('modelName')} (REPLAY)",
                "nodeId": info.get("nodeId")}
        gen, guid = info.get("generation"), int(str(info["guid"]), 16)
        dev = ReplayDevice(dumps)
    else:
        client = MCPClient(a.endpoint)
        try:
            client.connect()
            node, gen, guid = identify(client, a.guid, a.force)
        except MCPError as e:
            sys.exit(str(e))
        dev = Device(client, node["nodeId"], gen, guid)
    shown = (guid & ~0xFFFFFFFFFF) if a.strip_guid else guid
    print(f"== {node.get('vendorName')} {node.get('modelName')}  node {node.get('nodeId')}  gen {gen}  "
          f"guid {shown:#018x}")

    stages, facts = {}, {"formations": {}, "signalSources": {}, "descriptors": {}, "controls": []}
    graph = {"device": node.get("modelName"), "vendor": node.get("vendorName"), "streams": {}, "notes": []}
    try:
        discover(dev, a.no_controls, graph, stages, facts)
    except Wedged as e:
        stage(stages, "ABORTED", "wedged", reason=str(e))
    except MCPError as e:
        stage(stages, "ABORTED", "mcp_error", reason=str(e))

    print("\n== graph")
    print(json.dumps(graph, indent=1))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    model = "".join(ch if ch.isalnum() else "_" for ch in (info.get("modelName") if a.replay else node.get("modelName")) or "device").strip("_")
    device = {"modelName": node.get("modelName"), "vendorName": node.get("vendorName"),
              "guid": f"{shown:#018x}", "guidStripped": bool(a.strip_guid), "nodeId": node.get("nodeId"),
              "generation": gen}
    path = out / f"discover_{model}_{shown:016x}{'_replay' if a.replay else ''}_{stamp}.json"
    path.write_text(json.dumps({"device": device, "stages": stages, "facts": facts, "graph": graph,
                                "records": dev.records}, indent=1))
    print(f"\nsaved {path} ({len(dev.records)} exchanges)")
    if a.mcp:
        dump = Path(a.dump) if a.dump else out / f"dump_{model}_{shown:016x}_{stamp}.json"
        dump.write_text(json.dumps({
            "device": device | {"capturedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "tool": "avc_discover.py --mcp"},
            "records": [{"name": r["name"], "command": r["command"], "response": r["response"],
                         "responseCode": r["response"][0] if r["response"] else None,
                         "outcome": r["outcome"], "error": r["error"]} for r in dev.records],
            "descriptors": dev.descriptors}, indent=1))
        print(f"dump  {dump}\n      share this file; replay with: python3 avc_discover.py --replay {dump}")


if __name__ == "__main__":
    main()
