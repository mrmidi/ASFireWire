#!/usr/bin/env python3
"""Decode a FireBug / FireSpy IEEE-1394 text trace into a readable choreography.

FireBug's raw output is noisy: every transaction is split across a request line,
a response line somewhere later, and indented hex/annotation continuation lines,
with elision markers ("[402 packets not shown]") in between.  This script
reassembles it into one line per logical operation, pairs requests with their
responses, and decodes the parts that matter for FireWire audio bring-up:

  * AV/C  -- ctype/response, subunit, opcode, and operands for the opcodes used
             during stream setup (signal format, plug info, function block,
             vendor-dependent)
  * CMP   -- oPCR/iPCR lock transactions, with the plug state before and after
  * IRM   -- bandwidth and isochronous channel allocation
  * CSR   -- well-known register names instead of bare offsets
  * bus   -- resets, self-IDs, topology, isoch channel activity

Node IDs are generation-relative: FireBug prints the node number valid at that
moment, so the same physical device changes address across a bus reset.  Read
"ffc0->ffc2" against the nearest preceding Self-ID block, not globally.

Usage:
    python3 firebug_parse.py firebug.txt                  # full choreography
    python3 firebug_parse.py firebug.txt --only avc,cmp   # just the setup path
    python3 firebug_parse.py firebug.txt --payloads       # include hex dumps
    python3 firebug_parse.py firebug.txt --unparsed       # audit coverage
    python3 firebug_parse.py firebug.txt --syt 0          # AMDTP timestamp cadence
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field

# ---------------------------------------------------------------------------
# Timebase
# ---------------------------------------------------------------------------

TICKS_PER_CYCLE = 3072
CYCLES_PER_SECOND = 8000
TICKS_PER_SECOND = TICKS_PER_CYCLE * CYCLES_PER_SECOND
SECONDS_MODULUS = 128


@dataclass(frozen=True)
class Timestamp:
    seconds: int
    cycle: int
    offset: int

    @property
    def ticks(self) -> int:
        return (self.seconds * CYCLES_PER_SECOND + self.cycle) * TICKS_PER_CYCLE + self.offset

    def __str__(self) -> str:
        return f"{self.seconds:03d}:{self.cycle:04d}:{self.offset:04d}"


def elapsed_ms(start: Timestamp, end: Timestamp) -> float:
    """Milliseconds from start to end, unwrapping the 128 s cycle-timer field."""
    delta = end.ticks - start.ticks
    if delta < 0:
        delta += SECONDS_MODULUS * TICKS_PER_SECOND
    return delta * 1000.0 / TICKS_PER_SECOND


# ---------------------------------------------------------------------------
# CSR / address decoding
# ---------------------------------------------------------------------------

CSR_BASE_HI = 0xFFFF
CSR_REGISTERS = {
    0xF0000000: "STATE_CLEAR",
    0xF0000004: "STATE_SET",
    0xF0000008: "NODE_IDS",
    0xF000000C: "RESET_START",
    0xF0000018: "SPLIT_TIMEOUT_HI",
    0xF000001C: "SPLIT_TIMEOUT_LO",
    0xF0000200: "CYCLE_TIME",
    0xF0000204: "BUS_TIME",
    0xF000020C: "PRIORITY_BUDGET",
    0xF0000210: "BUSY_TIMEOUT",
    0xF0000218: "PRIORITY_BUDGET_DIAL",
    0xF000021C: "BUS_MANAGER_ID",
    0xF0000220: "BANDWIDTH_AVAILABLE",
    0xF0000224: "CHANNELS_AVAILABLE_HI",
    0xF0000228: "CHANNELS_AVAILABLE_LO",
    0xF0000230: "BROADCAST_CHANNEL",
    0xF0000900: "oMPR",
    0xF0000980: "iMPR",
    0xF0000B00: "FCP_COMMAND",
    0xF0000D00: "FCP_RESPONSE",
}

# M-Audio "special" firmware vendor window (bebob_maudio.c:51-53).
MAUDIO_ADDR_HI = 0xFFC7
MAUDIO_WINDOWS = {
    0x00600000: "MAUDIO_METER",
    0x00700000: "MAUDIO_PARAM",
}

CONFIG_ROM_BASE = 0xF0000400
CONFIG_ROM_END = 0xF0000800


def decode_address(addr_hi: int, addr_lo: int) -> str:
    """Name a 48-bit target address, falling back to raw hex."""
    if addr_hi == MAUDIO_ADDR_HI:
        for base, name in MAUDIO_WINDOWS.items():
            if base <= addr_lo < base + 0x10000:
                delta = addr_lo - base
                return name if delta == 0 else f"{name}+0x{delta:x}"
        return f"vendor 0x{addr_hi:04x}.{addr_lo:08x}"

    if addr_hi != CSR_BASE_HI:
        return f"0x{addr_hi:04x}.{addr_lo:08x}"

    if addr_lo in CSR_REGISTERS:
        return CSR_REGISTERS[addr_lo]
    # Connection Management Procedures plug registers (IEC 61883-1 sec 6).
    if 0xF0000904 <= addr_lo < 0xF0000980 and addr_lo % 4 == 0:
        return f"oPCR[{(addr_lo - 0xF0000904) // 4}]"
    if 0xF0000984 <= addr_lo < 0xF0000A00 and addr_lo % 4 == 0:
        return f"iPCR[{(addr_lo - 0xF0000984) // 4}]"
    if CONFIG_ROM_BASE <= addr_lo < CONFIG_ROM_END:
        return f"ConfigROM+0x{addr_lo - CONFIG_ROM_BASE:03x}"
    return f"0x{addr_lo:08x}"


def is_fcp_command(addr_hi: int, addr_lo: int) -> bool:
    return addr_hi == CSR_BASE_HI and addr_lo == 0xF0000B00


def is_fcp_response(addr_hi: int, addr_lo: int) -> bool:
    return addr_hi == CSR_BASE_HI and addr_lo == 0xF0000D00


# ---------------------------------------------------------------------------
# AV/C decoding (AV/C General Specification 4.0)
# ---------------------------------------------------------------------------

AVC_CTYPE = {
    0x00: "CONTROL",
    0x01: "STATUS",
    0x02: "SPECIFIC INQUIRY",
    0x03: "NOTIFY",
    0x04: "GENERAL INQUIRY",
}

AVC_RESPONSE = {
    0x08: "NOT IMPLEMENTED",
    0x09: "ACCEPTED",
    0x0A: "REJECTED",
    0x0B: "IN TRANSITION",
    0x0C: "IMPLEMENTED/STABLE",
    0x0D: "CHANGED",
    0x0F: "INTERIM",
}

AVC_SUBUNIT_TYPE = {
    0x00: "monitor",
    0x01: "audio",
    0x02: "printer",
    0x03: "disc",
    0x04: "tape",
    0x05: "tuner",
    0x06: "ca",
    0x07: "camera",
    0x09: "panel",
    0x0A: "bulletin-board",
    0x0C: "vendor-unique",
    0x1C: "extended",
    0x1E: "extended-unit",
    0x1F: "unit",
}

AVC_OPCODE = {
    0x00: "VENDOR-DEPENDENT",
    0x02: "PLUG INFO",
    0x0B: "RESERVE",
    0x18: "OUTPUT PLUG SIGNAL FORMAT",
    0x19: "INPUT PLUG SIGNAL FORMAT",
    0x1F: "GENERAL BUS SETUP",
    0x2F: "STREAM FORMAT INFO",
    0x30: "UNIT INFO",
    0x31: "SUBUNIT INFO",
    0x40: "PASS THROUGH",
    0xB2: "POWER",
    0xB8: "FUNCTION BLOCK",
    0xBF: "EXTENDED PLUG INFO",
}

# AM824 sampling frequency codes, IEC 61883-6 table 6.
SFC_RATE = {
    0x00: 32000,
    0x01: 44100,
    0x02: 48000,
    0x03: 88200,
    0x04: 96000,
    0x05: 176400,
    0x06: 192000,
}

FUNCTION_BLOCK_TYPE = {
    0x80: "Selector",
    0x81: "Feature",
    0x82: "Processing",
    0x83: "Codec",
}

# Vendor-dependent command families seen on M-Audio "special" firmware.
# The three bytes after the opcode are the company ID field.
VENDOR_COMMANDS = {
    (0x04, 0x00, 0x04): "M-Audio clock/format",
    (0x03, 0x00, 0x01): "M-Audio LED",
}

MAUDIO_CLOCK_SOURCE = {
    0x00: "internal+digital-mute",
    0x01: "digital",
    0x02: "word-clock",
    0x03: "internal",
}

MAUDIO_DIGITAL_FORMAT = {0x00: "S/PDIF", 0x01: "ADAT"}

CIP_FMT = {0x00: "DVCR", 0x10: "AM824", 0x20: "MPEG"}


@dataclass
class Cip:
    """Two-quadlet CIP header, IEC 61883-1 section 6.1."""
    sid: int
    dbs: int
    fn: int
    qpc: int
    sph: int
    dbc: int
    fmt: int
    fdf: int
    syt: int

    @property
    def is_no_data(self) -> bool:
        return self.fdf == 0xFF or self.syt == 0xFFFF

    @property
    def syt_ticks(self) -> int | None:
        """SYT as ticks within its 16-cycle window; None for NO-DATA."""
        if self.syt == 0xFFFF:
            return None
        return ((self.syt >> 12) & 0x0F) * TICKS_PER_CYCLE + (self.syt & 0x0FFF)

    def describe(self) -> str:
        fmt_name = CIP_FMT.get(self.fmt, f"fmt 0x{self.fmt:02x}")
        if self.fdf == 0xFF:
            rate = "NO-DATA"
        else:
            rate = SFC_RATE.get(self.fdf & 0x07)
            rate = f"{rate}Hz" if rate else f"SFC{self.fdf & 0x07}"
        text = (f"node{self.sid} DBS={self.dbs} DBC=0x{self.dbc:02x} "
                f"{fmt_name} {rate}")
        if self.syt == 0xFFFF:
            text += " SYT=----"
        else:
            text += f" SYT={self.syt:04x} (cyc {self.syt >> 12:x} off {self.syt & 0xFFF:4d})"
        return text


def decode_cip(payload: bytes) -> Cip | None:
    if len(payload) < 8:
        return None
    q0 = int.from_bytes(payload[0:4], "big")
    q1 = int.from_bytes(payload[4:8], "big")
    if (q0 >> 30) != 0 or (q1 >> 30) != 0b10:
        return None  # not a CIP-headered packet
    return Cip(sid=(q0 >> 24) & 0x3F, dbs=(q0 >> 16) & 0xFF,
               fn=(q0 >> 14) & 0x03, qpc=(q0 >> 11) & 0x07, sph=(q0 >> 10) & 1,
               dbc=q0 & 0xFF,
               fmt=(q1 >> 24) & 0x3F, fdf=(q1 >> 16) & 0xFF, syt=q1 & 0xFFFF)


def decode_subunit(byte: int) -> str:
    subunit_type = (byte >> 3) & 0x1F
    subunit_id = byte & 0x07
    name = AVC_SUBUNIT_TYPE.get(subunit_type, f"type-0x{subunit_type:02x}")
    if subunit_type == 0x1F and subunit_id == 7:
        return "unit"
    return f"{name}[{subunit_id}]"


def decode_signal_format(operands: bytes) -> str:
    """Operands of opcode 0x18/0x19: plug, then the 61883 fmt/fdf block."""
    if not operands:
        return ""
    parts = [f"plug {operands[0]}"]
    if len(operands) >= 2:
        fmt_byte = operands[1]
        eoh = (fmt_byte >> 7) & 1
        fmt = fmt_byte & 0x3F
        if fmt_byte == 0xFF:
            parts.append("no format")
            return " ".join(parts)
        fmt_name = {0x10: "AM824", 0x00: "DVCR", 0x20: "MPEG"}.get(fmt, f"fmt 0x{fmt:02x}")
        parts.append(fmt_name if eoh else f"{fmt_name} (eoh=0)")
    if len(operands) >= 3:
        fdf0 = operands[2]
        if fdf0 == 0xFF:
            parts.append("FDF NO-DATA")
        else:
            sfc = fdf0 & 0x07
            rate = SFC_RATE.get(sfc)
            parts.append(f"{rate} Hz" if rate else f"SFC {sfc}")
    return " ".join(parts)


CONTROL_ATTRIBUTE = {
    0x10: "CURRENT", 0x11: "MINIMUM", 0x12: "MAXIMUM",
    0x13: "RESOLUTION", 0x14: "DEFAULT", 0x18: "MOVE", 0x19: "DELTA",
}


def decode_function_block(operands: bytes) -> str:
    """AV/C Audio Subunit: fb_type, fb_ID, control_attribute, selector_length,
    then selector_length bytes of selector/data."""
    if len(operands) < 3:
        return ""
    fb_type, fb_id, attribute = operands[0], operands[1], operands[2]
    name = FUNCTION_BLOCK_TYPE.get(fb_type, f"fb 0x{fb_type:02x}")
    attr_name = CONTROL_ATTRIBUTE.get(attribute, f"0x{attribute:02x}")
    text = f"{name} id {fb_id} {attr_name}"
    if len(operands) >= 4:
        length = operands[3]
        body = operands[4:4 + max(length, 0)]
        if body:
            text += " sel " + " ".join(f"{b:02x}" for b in body)
    return text


def decode_vendor_dependent(operands: bytes) -> str:
    if len(operands) < 3:
        return ""
    company = (operands[0], operands[1], operands[2])
    name = VENDOR_COMMANDS.get(company, f"company {operands[0]:02x}{operands[1]:02x}{operands[2]:02x}")
    body = operands[3:]
    if company == (0x04, 0x00, 0x04) and len(body) >= 4:
        # bebob_maudio.c:171-198 avc_maudio_set_special_clk()
        src = MAUDIO_CLOCK_SOURCE.get(body[0], f"0x{body[0]:02x}")
        din = MAUDIO_DIGITAL_FORMAT.get(body[1], f"0x{body[1]:02x}")
        dout = MAUDIO_DIGITAL_FORMAT.get(body[2], f"0x{body[2]:02x}")
        lock = "locked" if body[3] else "unlocked"
        return f"{name}: clk={src} dig_in={din} dig_out={dout} {lock}"
    if body:
        return f"{name}: " + " ".join(f"{b:02x}" for b in body[:8])
    return name


def decode_avc(payload: bytes) -> str | None:
    """Render one AV/C frame. Returns None if the payload is too short."""
    if len(payload) < 3:
        return None
    ctype = payload[0] & 0x0F
    subunit = decode_subunit(payload[1])
    opcode = payload[2]
    operands = payload[3:]

    verb = AVC_CTYPE.get(ctype) or AVC_RESPONSE.get(ctype) or f"ctype 0x{ctype:x}"
    opname = AVC_OPCODE.get(opcode, f"opcode 0x{opcode:02x}")

    detail = ""
    if opcode in (0x18, 0x19):
        detail = decode_signal_format(operands)
    elif opcode == 0xB8:
        detail = decode_function_block(operands)
    elif opcode == 0x00:
        detail = decode_vendor_dependent(operands)
    elif opcode == 0x02 and operands:
        detail = f"subfunction 0x{operands[0]:02x}"
    elif operands:
        detail = " ".join(f"{b:02x}" for b in operands[:8])

    text = f"AV/C {verb} {subunit} {opname}"
    if detail:
        text += f"  [{detail}]"
    return text


def avc_is_interim(payload: bytes) -> bool:
    return len(payload) >= 1 and (payload[0] & 0x0F) == 0x0F


def avc_key(payload: bytes) -> tuple[int, int] | None:
    """Identity used to pair an FCP response with its request."""
    if len(payload) < 3:
        return None
    return (payload[1], payload[2])


# ---------------------------------------------------------------------------
# CMP / IRM decoding
# ---------------------------------------------------------------------------

PCR_RATE = {0: "S100", 1: "S200", 2: "S400", 3: "S800"}


def decode_pcr(value: int, output: bool) -> str:
    """IEC 61883-1 section 6.3: oPCR and iPCR bit layout."""
    online = (value >> 31) & 1
    bcast = (value >> 30) & 1
    p2p = (value >> 24) & 0x3F
    channel = (value >> 16) & 0x3F
    text = f"online={online} bcast={bcast} p2p={p2p} ch={channel}"
    if output:
        rate = PCR_RATE.get((value >> 14) & 0x03, "?")
        overhead_id = (value >> 10) & 0x0F
        overhead = 512 if overhead_id == 0 else overhead_id * 32
        payload = value & 0x3FF
        text += f" {rate} overhead={overhead} payload={payload}q"
    return text


def decode_channels_mask(old: int, new: int, high: bool) -> str:
    """CHANNELS_AVAILABLE bits are numbered from the MSB down (channel 0 = bit 31)."""
    changed = old ^ new
    if not changed:
        return "no change"
    base = 0 if high else 32
    freed = []
    taken = []
    for bit in range(32):
        if not (changed >> (31 - bit)) & 1:
            continue
        channel = base + bit
        # A set bit means the channel is free.
        if (new >> (31 - bit)) & 1:
            freed.append(channel)
        else:
            taken.append(channel)
    parts = []
    if taken:
        parts.append("allocate ch " + ",".join(str(c) for c in taken))
    if freed:
        parts.append("release ch " + ",".join(str(c) for c in freed))
    return "; ".join(parts)


def decode_lock(register: str, payload: bytes) -> str:
    """A compare-swap lock request carries [arg, data] as two big-endian quadlets."""
    if len(payload) < 8:
        return ""
    old = int.from_bytes(payload[0:4], "big")
    new = int.from_bytes(payload[4:8], "big")
    if register == "BANDWIDTH_AVAILABLE":
        delta = old - new
        verb = "allocate" if delta > 0 else "release"
        return f"{old} -> {new} ({verb} {abs(delta)} units)"
    if register in ("CHANNELS_AVAILABLE_HI", "CHANNELS_AVAILABLE_LO"):
        return f"{old:08x} -> {new:08x} ({decode_channels_mask(old, new, register.endswith('HI'))})"
    if register == "BUS_MANAGER_ID":
        return f"BUS_MANAGER {old & 0x3F:#04x} -> {new & 0x3F:#04x}"
    if register.startswith("oPCR") or register.startswith("iPCR"):
        output = register.startswith("oPCR")
        return f"{old:08x} -> {new:08x}\n    from: {decode_pcr(old, output)}\n    to:   {decode_pcr(new, output)}"
    return f"{old:08x} -> {new:08x}"


# ---------------------------------------------------------------------------
# Trace model
# ---------------------------------------------------------------------------

@dataclass
class Event:
    ts: Timestamp
    kind: str                     # Qread, Bwrite, LockRq, BUS RESET, ...
    line: str
    src: str | None = None
    dst: str | None = None
    addr_hi: int | None = None
    addr_lo: int | None = None
    tlabel: int | None = None
    value: int | None = None
    rcode: int | None = None
    size: int | None = None
    payload: bytearray = field(default_factory=bytearray)
    notes: list[str] = field(default_factory=list)
    # Filled in during pairing.
    response: "Event | None" = None
    interim: "Event | None" = None
    matched: bool = False
    category: str = "other"


TS_RE = re.compile(r"^(\d{3}):(\d{4}):(\d{4})\s+(.*)$")
ADDR_RE = re.compile(r"([0-9a-f]{4})\.([0-9a-f]{4})\.([0-9a-f]{4})\.([0-9a-f]{4})")
HEX_DUMP_RE = re.compile(r"^\s{10,}([0-9a-f]{4})\s+((?:[0-9a-f]{8}\s*)+)")
ELISION_RE = re.compile(r"^\s*\[(\d+) packets? not shown\]")
IDLE_RE = re.compile(r"^\s*\[no activity logged for ([^\]]+)\]")

RCODE_NAMES = {
    0: "complete",
    1: "conflict_error",
    2: "data_error",
    3: "type_error",
    4: "address_error",
    5: "resp_conflict",
    6: "resp_data_error",
    7: "resp_address_error",
}


def parse(path: str) -> tuple[list[Event], list[str]]:
    events: list[Event] = []
    unparsed: list[str] = []
    current: Event | None = None

    for raw in open(path, errors="replace"):
        line = raw.rstrip("\n")
        if not line.strip():
            continue

        m = TS_RE.match(line)
        if m:
            ts = Timestamp(int(m.group(1)), int(m.group(2)), int(m.group(3)))
            body = m.group(4).strip()
            current = build_event(ts, body, line)
            events.append(current)
            continue

        # Continuation lines belong to the event above them.
        dump = HEX_DUMP_RE.match(line)
        if dump and current is not None:
            for word in dump.group(2).split():
                current.payload.extend(int(word, 16).to_bytes(4, "big"))
            continue

        elision = ELISION_RE.match(line)
        if elision:
            events.append(Event(ts=events[-1].ts if events else Timestamp(0, 0, 0),
                                kind="ELIDED", line=line.strip(),
                                category="elision",
                                size=int(elision.group(1))))
            continue

        idle = IDLE_RE.match(line)
        if idle:
            events.append(Event(ts=events[-1].ts if events else Timestamp(0, 0, 0),
                                kind="IDLE", line=idle.group(1).strip(), category="idle"))
            continue

        stripped = line.strip()
        if stripped.startswith("Isoch channel"):
            events.append(Event(ts=events[-1].ts if events else Timestamp(0, 0, 0),
                                kind="ISOCH", line=stripped, category="isoch"))
            continue
        if stripped.startswith(("Bus Topology:", "[node ", "|", "Packets:", "Smallest:",
                                "Active time", "Previous packet")):
            if current is not None:
                current.notes.append(stripped)
            continue
        if current is not None and stripped:
            # FireBug's own decode lines (FCP Request:, IRM:, CMP:, AVC/UNIT:).
            current.notes.append(stripped)
            continue

        unparsed.append(line)

    return events, unparsed


ISOCH_PKT_RE = re.compile(r"Isoch channel (\d+), tag (\d+), sy (\d+), size (\d+)")


def build_event(ts: Timestamp, body: str, raw: str) -> Event:
    kind = body.split()[0] if body else "?"

    if body.startswith("BUS RESET"):
        return Event(ts=ts, kind="BUS RESET", line="BUS RESET", category="bus")
    if kind in ("Self-ID", "PHY", "CycleStart"):
        return Event(ts=ts, kind=kind, line=body, category="bus")

    pkt = ISOCH_PKT_RE.match(body)
    if pkt:
        ev = Event(ts=ts, kind="ISOCHPKT", line=body, category="isochpkt")
        ev.value = int(pkt.group(1))   # isoch channel number
        ev.size = int(pkt.group(4))
        return ev

    ev = Event(ts=ts, kind=kind, line=body)

    addr = ADDR_RE.search(body)
    if addr:
        ev.dst = addr.group(1)
        ev.addr_hi = int(addr.group(2), 16)
        ev.addr_lo = int(addr.group(3) + addr.group(4), 16)
    else:
        # Responses carry only "from X to Y".
        node = re.search(r"from ([0-9a-f]{4}) to ([0-9a-f]{4})", body)
        if node:
            ev.dst = node.group(2)

    src = re.search(r"(?:from|fr) ([0-9a-f]{4})", body)
    if src:
        ev.src = src.group(1)

    for pattern, attr in ((r"tLabel (\d+)", "tlabel"), (r"tLab (\d+)", "tlabel"),
                          (r"rCode (\d+)", "rcode")):
        m = re.search(pattern, body)
        if m:
            setattr(ev, attr, int(m.group(1)))

    m = re.search(r"value ([0-9a-f]{8})", body)
    if m:
        ev.value = int(m.group(1), 16)
    m = re.search(r"(?:size|sz) (\d+)", body)
    if m:
        ev.size = int(m.group(1))

    ev.category = classify(ev)
    return ev


def classify(ev: Event) -> str:
    if ev.addr_hi is None:
        return "response" if ev.kind in ("QRresp", "BRresp", "WrResp", "LockResp") else "other"
    if is_fcp_command(ev.addr_hi, ev.addr_lo) or is_fcp_response(ev.addr_hi, ev.addr_lo):
        return "avc"
    name = decode_address(ev.addr_hi, ev.addr_lo)
    if name.startswith(("oPCR", "iPCR", "oMPR", "iMPR")):
        return "cmp"
    if name in ("BANDWIDTH_AVAILABLE", "CHANNELS_AVAILABLE_HI", "CHANNELS_AVAILABLE_LO",
                "BUS_MANAGER_ID"):
        return "irm"
    if name.startswith("ConfigROM"):
        return "rom"
    if name.startswith("MAUDIO"):
        return "vendor"
    return "csr"


REQUEST_KINDS = {"Qread", "Qwrite", "Bread", "Bwrite", "LockRq"}
RESPONSE_KINDS = {"QRresp", "WrResp", "BRresp", "LockResp"}


def pair_transactions(events: list[Event]) -> None:
    """Match split transactions by tLabel, and FCP frames by AV/C identity."""
    pending: dict[tuple[str, str, int], Event] = {}
    fcp_pending: dict[tuple[int, int], Event] = {}

    for ev in events:
        if ev.kind in REQUEST_KINDS and ev.tlabel is not None:
            # An FCP write is a request at the transaction layer, but the AV/C
            # answer arrives later as a separate write in the other direction.
            pending[(ev.src or "?", ev.dst or "?", ev.tlabel)] = ev
        elif ev.kind in RESPONSE_KINDS and ev.tlabel is not None:
            key = (ev.dst or "?", ev.src or "?", ev.tlabel)
            request = pending.pop(key, None)
            if request is not None:
                request.response = ev
                ev.matched = True

    # Second pass: AV/C request/response pairing across the FCP register pair.
    for ev in events:
        if ev.addr_hi is None or not ev.payload:
            continue
        key = avc_key(bytes(ev.payload))
        if key is None:
            continue
        if is_fcp_command(ev.addr_hi, ev.addr_lo):
            fcp_pending[key] = ev
        elif is_fcp_response(ev.addr_hi, ev.addr_lo):
            request = fcp_pending.get(key)
            if request is None:
                continue
            if avc_is_interim(bytes(ev.payload)):
                request.interim = ev
            else:
                request.response = ev
                fcp_pending.pop(key, None)
            ev.matched = True


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def render(ev: Event, show_payload: bool) -> list[str]:
    out: list[str] = []
    stamp = str(ev.ts)

    if ev.category == "elision":
        return [f"{'':>14}  ... {ev.size} packets not shown"]
    if ev.category == "idle":
        return [f"{'':>14}  === idle for {ev.line} ==="]
    if ev.category == "isoch":
        return [f"{'':>14}  ** {ev.line}"]
    if ev.category == "isochpkt":
        cip = decode_cip(bytes(ev.payload))
        detail = cip.describe() if cip else "no CIP header"
        out = [f"{stamp}  ch{ev.value}  {detail}  [{ev.size} B]"]
        if show_payload and len(ev.payload) > 8:
            out.append(f"{'':>14}  raw: " + " ".join(f"{b:02x}" for b in ev.payload[8:40]))
        return out
    if ev.kind == "BUS RESET":
        return ["", f"{stamp}  {'='*20} BUS RESET {'='*20}"]
    if ev.kind in ("Self-ID", "PHY", "CycleStart"):
        return [f"{stamp}  {ev.line}"]

    arrow = f"{ev.src or '????'}->{ev.dst or '????'}"

    if ev.addr_hi is not None and ev.payload and is_fcp_command(ev.addr_hi, ev.addr_lo):
        decoded = decode_avc(bytes(ev.payload)) or "AV/C <short>"
        out.append(f"{stamp}  {arrow}  {decoded}")
        if ev.interim is not None:
            dt = elapsed_ms(ev.ts, ev.interim.ts)
            out.append(f"{'':>14}  {'':>10}  -> INTERIM  (+{dt:.1f} ms)")
        if ev.response is not None:
            dt = elapsed_ms(ev.ts, ev.response.ts)
            reply = decode_avc(bytes(ev.response.payload)) or "?"
            reply = reply.replace("AV/C ", "")
            out.append(f"{'':>14}  {'':>10}  -> {reply}  (+{dt:.1f} ms)")
        else:
            out.append(f"{'':>14}  {'':>10}  -> (no response captured)")
        if show_payload and ev.payload:
            out.append(f"{'':>14}  raw: " + " ".join(f"{b:02x}" for b in ev.payload))
        return out

    if ev.addr_hi is not None and is_fcp_response(ev.addr_hi, ev.addr_lo):
        return []  # already rendered beneath its request

    if ev.addr_hi is None:
        if ev.matched:
            return []  # rendered beneath its request
        return [f"{stamp}  {arrow}  {ev.kind} tLabel {ev.tlabel} (unmatched)"]

    register = decode_address(ev.addr_hi, ev.addr_lo)

    if ev.kind == "LockRq":
        detail = decode_lock(register, bytes(ev.payload))
        out.append(f"{stamp}  {arrow}  LOCK  {register}  {detail}")
        if ev.response is not None:
            dt = elapsed_ms(ev.ts, ev.response.ts)
            verdict = next((n for n in ev.response.notes if n.startswith("IRM:") or n.startswith("CMP:")), "")
            got = int.from_bytes(ev.response.payload[0:4], "big") if ev.response.payload else None
            got_text = f"returned {got:08x}" if got is not None else ""
            out.append(f"{'':>14}  {'':>10}  -> {verdict or got_text}  (+{dt:.1f} ms)")
        return out

    if ev.kind == "Qread":
        out.append(f"{stamp}  {arrow}  READ  {register}")
        if ev.response is not None and ev.response.value is not None:
            dt = elapsed_ms(ev.ts, ev.response.ts)
            extra = ""
            if register.startswith(("oPCR", "iPCR")):
                extra = "  " + decode_pcr(ev.response.value, register.startswith("oPCR"))
            out.append(f"{'':>14}  {'':>10}  = {ev.response.value:08x}{extra}  (+{dt:.1f} ms)")
        return out

    if ev.kind == "Qwrite":
        out.append(f"{stamp}  {arrow}  WRITE {register} = {ev.value:08x}")
        if ev.response is not None:
            rc = ev.response.rcode
            name = RCODE_NAMES.get(rc, "?")
            dt = elapsed_ms(ev.ts, ev.response.ts)
            out.append(f"{'':>14}  {'':>10}  -> rCode {rc} ({name})  (+{dt:.1f} ms)")
        return out

    if ev.kind == "Bread":
        out.append(f"{stamp}  {arrow}  BLOCK READ {register} [{ev.size} bytes]")
        if ev.response is not None and show_payload and ev.response.payload:
            out.append(f"{'':>14}  raw: " + " ".join(f"{b:02x}" for b in ev.response.payload[:64]))
        return out

    if ev.kind == "Bwrite":
        out.append(f"{stamp}  {arrow}  BLOCK WRITE {register} [{ev.size} bytes]")
        if show_payload and ev.payload:
            out.append(f"{'':>14}  raw: " + " ".join(f"{b:02x}" for b in ev.payload[:64]))
        return out

    return [f"{stamp}  {arrow}  {ev.kind} {register}"]


SYT_WINDOW_TICKS = 16 * TICKS_PER_CYCLE

# "Isoch channel 1 ACTIVE at 102:5046:0971 (CT 017:6413)" -- FireBug's own
# capture counter next to the real bus cycle time.
CT_ANCHOR_RE = re.compile(r"at (\d{3}):(\d{4}):(\d{4}) \(CT (\d{3}):(\d{4})\)")


def bus_cycle_offset(path: str) -> int | None:
    """Cycles to add to a FireBug timestamp to get the bus cycle count.

    FireBug's leading "seconds:cycle:offset" is its OWN capture counter, not the
    bus cycle timer -- the two differ by a large constant (1367 cycles in one
    1814 trace, 345367 including seconds).  Any SYT lead computed against the
    raw FireBug cycle is therefore wrong by (offset % 16) cycles, which is
    exactly the size of the whole SYT window.  The channel ACTIVE/STOPPED lines
    print both clocks, so the offset is recoverable; it is stable to about one
    cycle per 16 s of capture and jumps only across a bus reset.
    """
    offsets: list[int] = []
    for line in open(path, errors="replace"):
        m = CT_ANCHOR_RE.search(line)
        if m:
            fs, fc, _fo, cs, cc = (int(g) for g in m.groups())
            offsets.append(((cs * CYCLES_PER_SECOND + cc) -
                            (fs * CYCLES_PER_SECOND + fc)) %
                           (SECONDS_MODULUS * CYCLES_PER_SECOND))
    if not offsets:
        return None
    return max(set(offsets), key=offsets.count)


def syt_report(events: list[Event], channel: int, limit: int,
               offset: int | None) -> None:
    """Tabulate the AMDTP timestamp cadence of one isochronous channel.

    The SYT field carries only the low 4 bits of the cycle count, so deltas are
    taken modulo the 16-cycle window.  At 48 kHz with SYT_INTERVAL 8 the
    expected delta between consecutive DATA packets is 4096 ticks
    (24576000 * 8 / 48000); anything else is drift.

    `lead` is the distance from the packet's own position on the bus to the
    presentation time its SYT names.  IEC 61883-6 puts that at the transfer
    delay: Linux computes 12800 ticks at 48 kHz blocking, i.e. 4.17 to 4.83
    cycles ahead of the transmit cycle boundary (amdtp-stream.c:303-307,
    :1019-1028).  It is printed only when the bus clock could be calibrated,
    because an uncalibrated lead is off by a whole multiple of a cycle.
    """
    calibrated = offset is not None
    print(f"{'timestamp':<15} {'bus cyc':>8} {'DBC':>5} {'SYT':>6} {'cyc':>4} "
          f"{'off':>5} {'d ticks':>8} {'lead cyc':>9}  size")
    print("-" * 80)
    previous: int | None = None
    shown = 0
    for ev in events:
        if ev.category != "isochpkt" or ev.value != channel:
            continue
        cip = decode_cip(bytes(ev.payload))
        if cip is None:
            continue
        bus = (ev.ts.seconds * CYCLES_PER_SECOND + ev.ts.cycle +
               (offset or 0)) % (SECONDS_MODULUS * CYCLES_PER_SECOND)
        bus_text = f"{bus % CYCLES_PER_SECOND:>8}" if calibrated else f"{'?':>8}"
        if cip.syt == 0xFFFF:
            print(f"{str(ev.ts):<15} {bus_text} {cip.dbc:>#5x} {'----':>6} "
                  f"{'':>4} {'':>5} {'':>8} {'':>9}  {ev.size}   NO-DATA")
        else:
            ticks = cip.syt_ticks or 0
            delta = "" if previous is None else f"{(ticks - previous) % SYT_WINDOW_TICKS:>8}"
            if calibrated:
                position = (bus % 16) * TICKS_PER_CYCLE + ev.ts.offset
                lead = f"{(ticks - position) % SYT_WINDOW_TICKS / TICKS_PER_CYCLE:>9.2f}"
            else:
                lead = f"{'n/a':>9}"
            print(f"{str(ev.ts):<15} {bus_text} {cip.dbc:>#5x} {cip.syt:>#6x} "
                  f"{cip.syt >> 12:>4x} {cip.syt & 0xFFF:>5} {delta:>8} {lead}  {ev.size}")
            previous = ticks
        shown += 1
        if limit and shown >= limit:
            break
    if not calibrated:
        print("\nNo 'ACTIVE/STOPPED ... (CT s:cccc)' line in this trace, so the "
              "FireBug capture clock could not be tied to the bus cycle count. "
              "Lead is unknowable from this trace -- do not read the SYT cycle "
              "field against the timestamp column.")


CATEGORY_ALIASES = {
    "avc": {"avc"},
    "isochpkt": {"isochpkt"},
    "cmp": {"cmp"},
    "irm": {"irm"},
    "rom": {"rom"},
    "vendor": {"vendor"},
    "isoch": {"isoch"},
    "bus": {"bus"},
    "csr": {"csr"},
    "setup": {"avc", "cmp", "irm", "vendor", "isoch", "bus"},
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--only", default="",
                    help="comma-separated categories: " + ",".join(sorted(CATEGORY_ALIASES)))
    ap.add_argument("--payloads", action="store_true", help="include raw hex payloads")
    ap.add_argument("--unparsed", action="store_true", help="report lines the parser ignored")
    ap.add_argument("--stats", action="store_true", help="print a category histogram and exit")
    ap.add_argument("--syt", type=int, metavar="CH",
                    help="tabulate the AMDTP SYT cadence of one isoch channel")
    ap.add_argument("--limit", type=int, default=40, help="rows for --syt (0 = all)")
    args = ap.parse_args()

    events, unparsed = parse(args.trace)
    pair_transactions(events)

    if args.syt is not None:
        offset = bus_cycle_offset(args.trace)
        if offset is not None:
            print(f"bus cycle = firebug cycle + {offset} "
                  f"(+{offset % CYCLES_PER_SECOND} within the second)")
        syt_report(events, args.syt, args.limit, offset)
        return 0

    if args.stats:
        counts: dict[str, int] = {}
        for ev in events:
            counts[ev.category] = counts.get(ev.category, 0) + 1
        for name, count in sorted(counts.items(), key=lambda kv: -kv[1]):
            print(f"{count:7d}  {name}")
        print(f"{len(unparsed):7d}  <unparsed lines>")
        return 0

    wanted: set[str] | None = None
    if args.only:
        wanted = set()
        for token in args.only.split(","):
            token = token.strip()
            wanted |= CATEGORY_ALIASES.get(token, {token})

    for ev in events:
        if wanted is not None and ev.category not in wanted:
            continue
        for line in render(ev, args.payloads):
            print(line)

    if args.unparsed and unparsed:
        print("\n--- unparsed lines ---", file=sys.stderr)
        for line in unparsed[:200]:
            print(line, file=sys.stderr)
        print(f"({len(unparsed)} total)", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
