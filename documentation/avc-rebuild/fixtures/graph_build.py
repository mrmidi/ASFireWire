#!/usr/bin/env python3
"""Build a device graph from captured AV/C fixtures only (no hardware, no driver code).

Inputs per device:
  - probe matrix (documentation/fixtures/AVC/<dev>.json on refactor/avc-stack): stream-format lists, signal sources;
  - descriptors (<dev>_descriptors.json here): music status descriptor 60/80; audio identifier 08/00 + text DB.
Layouts: info blocks TA 1999045 as encoded by AVCVideoServices VirtualMusicSubunit.cpp:2269-2333;
audio function blocks TA 1999008 Table 8.1; feature controls Table 8.3.
Usage: graph_build.py <probe.json> <descriptors.json> [out.json]
"""
import json
import sys

RATE = {0x00: 22050, 0x01: 24000, 0x02: 32000, 0x03: 44100, 0x04: 48000, 0x05: 96000, 0x06: 176400,
        0x07: 192000, 0x0A: 88200}  # stream-format rate codes (TA 2001002; Linux bebob_stream.c:38-46)
PORT = {0x00: "speaker", 0x03: "line", 0x04: "spdif", 0x09: "digital", 0x0A: "midi"}
FMT = {0x00: "IEC60958", 0x06: "MBLA", 0x0D: "MIDI", 0x40: "sync"}
u16 = lambda b, i: (b[i] << 8) | b[i + 1]


def blocks(buf, start, end):
    """Yield (type, primary, children_start, block_end) for sibling info blocks."""
    pos = start
    while pos + 6 <= end:
        clen, btype, plen = u16(buf, pos), u16(buf, pos + 2), u16(buf, pos + 4)
        yield btype, buf[pos + 6:pos + 6 + plen], pos + 6 + plen, pos + 2 + clen
        pos += 2 + clen


def name_in(buf, start, end):
    """Text of the first Raw Text (000A) found under [start, end), directly or inside Name Info (000B)."""
    for btype, prim, cs, ce in blocks(buf, start, end):
        if btype == 0x000A:
            return prim.split(b"\0")[0].decode("ascii", "replace")
        if btype == 0x000B:
            n = name_in(buf, cs, ce) or name_in(prim, 4, len(prim))  # nested, or inline after 4 primary bytes
            if n:
                return n
    return None


def music_descriptor(buf):
    g = {"destPlugs": [], "sourcePlugs": [], "musicPlugs": []}
    g["plugChannelNames"] = {}  # source plug id -> names, from Output Plug Status 8101/8102/8103 (Phase 88 style)
    for btype, prim, cs, ce in blocks(buf, 2, len(buf)):
        if btype == 0x8101:
            for t, p, pcs, pce in blocks(buf, cs, ce):
                if t != 0x8102:
                    continue
                for at, ap, acs, ace in blocks(buf, pcs, pce):
                    if at == 0x8103:
                        text = name_in(buf, acs, ace) or ""
                        g["plugChannelNames"][p[0]] = [n for n in text.replace("\r", "").split("\n") if n]
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
    if not buf:
        return []
    n, p, out = u16(buf, 6 + u16(buf, 4)), 8 + u16(buf, 4), []
    for _ in range(n):
        e = buf[p:p + 2 + u16(buf, p)]
        runs, cur = [], ""
        for c in e[4:]:
            cur = cur + chr(c) if 32 <= c < 127 else (runs.append(cur) or "")
        out.append(max(runs + [cur], key=len))
        p += len(e)
    return out


def audio_identifier(buf, names):
    if not buf:
        return []
    p = 8 + u16(buf, 6) * buf[3] + 2 + 4
    fbs = []
    for _ in range(buf[p - 1]):  # configurations
        q = p + 4
        q += 2 + u16(buf, q)                   # master cluster
        q += 1 + 2 * buf[q]                    # subunit source plug links
        nfb, q = buf[q], q + 1
        for _ in range(nfb):
            flen, ft, fid, nm, npl = u16(buf, q), buf[q + 2], buf[q + 3], u16(buf, q + 4), buf[q + 6]
            srcs = [f"{buf[q + 7 + 2 * i]:02x}{buf[q + 8 + 2 * i]:02x}" for i in range(npl)]
            r = q + 7 + 2 * npl
            cl = buf[r + 2:r + 2 + u16(buf, r)]
            kind = {0x80: "selector", 0x81: "feature", 0x82: "processing"}.get(ft, hex(ft))
            fbs.append({"kind": kind, "id": fid, "name": names[nm] if nm < len(names) else None,
                        "inputs": srcs, "channels": cl[0] if cl else None})
            q += 2 + flen
        p += 2 + u16(buf, p)
    return fbs


def main():
    probe = json.load(open(sys.argv[1]))
    desc = json.load(open(sys.argv[2]))
    get = lambda sub, spec: next((bytes.fromhex(d["bytes"]) for d in desc["descriptors"]
                                  if d["subunit"] == sub and d["specifier"] == spec), b"")
    rec = {r["name"]: bytes(r["response"] or []) for r in probe["records"]}

    graph = {"device": desc["device"]["modelName"], "rates": [], "streams": {}, "clock": [], "controls": []}
    for i in range(32):  # formations: unit iso input plug 0 list (0x2F answers on both devices)
        r = rec.get(f"stream_format_0x2F_list_unit_iso_in_0_idx_{i}")
        if not r or r[0] != 0x0C:
            break
        graph["rates"].append(RATE.get(r[13], hex(r[13])))

    music = music_descriptor(get("0x60", "80"))
    for d in range(10):  # SIGNAL SOURCE: which music dest plug is fed by unit iso plug 0 = host playback
        r = rec.get(f"signal_source_0xFF_music_0_dest_{d}")
        if r and len(r) >= 8 and r[4:6] == b"\xff\x00":
            graph["streams"]["playback"] = next(p for p in music["destPlugs"] if p["id"] == d)
    # Capture = the music source plug feeding unit iso OUTPUT plug 0 (SIGNAL SOURCE, dest FF 00). Falls back to
    # source plug 0, flagged, when the fixture lacks that query.
    drec = {r["name"]: bytes(r["response"] or []) for r in desc.get("records", [])}
    r = drec.get("signal_source_0xFF_unit_iso_out_0")
    if r and len(r) >= 6 and r[4] == 0x60:
        graph["streams"]["capture"] = next(p for p in music["sourcePlugs"] if p["id"] == r[5])
        graph["captureSource"] = "SIGNAL SOURCE"
    else:
        graph["streams"]["capture"] = music["sourcePlugs"][0] if music["sourcePlugs"] else None
        graph["captureSource"] = "ASSUMED source plug 0"
    print("  capture plug from:", graph["captureSource"])
    for p in music["destPlugs"]:
        if any(c["format"] == "sync" for c in p["clusters"]):
            r = rec.get(f"signal_source_0xFF_music_0_dest_{p['id']}")
            graph["clock"].append({"syncDestPlug": p["id"], "fedBy": r[4:6].hex() if r and len(r) >= 6 else None})
    graph["controls"] = audio_identifier(get("0x08", "00"), text_db(get("0x08", "101801")))
    graph["musicPlugs"] = music["musicPlugs"]

    def show(label, plug):
        if not plug:
            print(f"  {label}: ?")
            return
        print(f"  {label}: subunit plug {plug['id']}, {plug['channels']} slots")
        for c in plug["clusters"]:
            print(f"    {c['name']!r} {c['format']}/{c['port']}: slots {[s['position'] for s in c['signals']]}")

    print(f"== {graph['device']}\n  rates: {graph['rates']}")
    show("playback (host -> device)", graph["streams"].get("playback"))
    show("capture  (device -> host)", graph["streams"]["capture"])
    names = {m["id"]: m["name"] for m in graph["musicPlugs"]}
    cap = graph["streams"]["capture"]
    per_plug = music["plugChannelNames"].get(cap["id"]) if cap else None
    graph["captureChannelNames"] = [names.get(s["musicPlug"]) for c in cap["clusters"] for s in c["signals"]
                                    if c["format"] != "MIDI"] if cap else []
    if not any(graph["captureChannelNames"]) and per_plug:
        graph["captureChannelNames"] = per_plug
    print("  capture channel names:", graph["captureChannelNames"])
    print("  clock:", graph["clock"])
    for fb in graph["controls"]:
        print(f"  {fb['kind']:10s} {fb['id']:2d} {fb['name'] or '(unnamed)'} <- {fb['inputs']}")
    if len(sys.argv) > 3:
        json.dump(graph, open(sys.argv[3], "w"), indent=1)


if __name__ == "__main__":
    main()
