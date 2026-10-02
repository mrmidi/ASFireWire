#!/usr/bin/env python3
"""Print the info-block tree of a descriptor saved by desc_read.py.

Info block layout (TA 1999045, as parsed by AVCVideoServices MusicSubunitController.cpp
and the Orpheus fork's AVCInfoBlock): [compound_length:2][type:2][primary_fields_length:2]
[primary fields][nested info blocks]. compound_length excludes its own 2 bytes.
Usage: infoblocks.py <json> [subunit-hex type-hex]
"""
import json
import sys

NAMES = {
    0x8100: "General Music Subunit Status", 0x8101: "Music Output Plug Status", 0x8102: "Source Plug Status",
    0x8103: "Audio Info", 0x8104: "MIDI Info", 0x8105: "SMPTE Time Code Info", 0x8106: "Sample Count Info",
    0x8107: "Audio SYNC Info", 0x8108: "Routing Status", 0x8109: "Subunit Plug Info", 0x810A: "Cluster Info",
    0x810B: "Music Plug Info", 0x000B: "Name Info", 0x000A: "Raw Text",
}


def text(b):
    return "".join(chr(c) if 32 <= c < 127 else "." for c in b)


def walk(buf, start, end, depth):
    pos = start
    while pos + 6 <= end:
        clen = (buf[pos] << 8) | buf[pos + 1]
        btype = (buf[pos + 2] << 8) | buf[pos + 3]
        plen = (buf[pos + 4] << 8) | buf[pos + 5]
        block_end = pos + 2 + clen
        prim = buf[pos + 6:pos + 6 + plen]
        name = NAMES.get(btype, "?")
        shown = f'"{text(prim)}"' if btype == 0x000A else prim.hex(" ")
        print(f"{'  ' * depth}{btype:04X} {name} (len {clen}, primary {plen}): {shown[:100]}")
        if block_end > end or clen < 4:
            print(f"{'  ' * depth}!! block overruns parent; stopping")
            return
        walk(buf, pos + 6 + plen, block_end, depth + 1)
        pos = block_end


def main():
    d = json.load(open(sys.argv[1]))
    want = (int(sys.argv[2], 16), int(sys.argv[3], 16)) if len(sys.argv) > 3 else None
    for desc in d["descriptors"]:
        if want and (desc["subunit"], desc["type"]) != want:
            continue
        buf = bytes.fromhex(desc["bytes"])
        print(f"### subunit 0x{desc['subunit']:02X} type 0x{desc['type']:02X}: {len(buf)} bytes, "
              f"declared {((buf[0] << 8) | buf[1]) + 2 if len(buf) > 1 else 0}")
        walk(buf, 2, len(buf), 0)


if __name__ == "__main__":
    main()
