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
# The meter window is polled continuously by the host for level display, so it
# swamps the trace; it is classified as its own category and suppressed from the
# default output (see classify() and main()).
MAUDIO_ADDR_HI = 0xFFC7
MAUDIO_METER_WINDOW = 0x00600000
MAUDIO_PARAM_WINDOW = 0x00700000
MAUDIO_WINDOWS = {
    MAUDIO_METER_WINDOW: "MAUDIO_METER",
    MAUDIO_PARAM_WINDOW: "MAUDIO_PARAM",
}

CONFIG_ROM_BASE = 0xF0000400
CONFIG_ROM_END = 0xF0000800

# BridgeCo DM1000/DM1100/DM1500 register file, Linux bebob/bebob.h:38-39.
# Quadlets in this window are LITTLE-endian, unlike everything else on the bus:
# Linux reads them raw and uses them as host u32 (bebob.c:91-115), and the GUID
# only reads back as the M-Audio OUI 0x000d6c that way.
BEBOB_REG_INFO = 0xC8020000
BEBOB_REG_REQ = 0xC8021000

# Field offsets inside the info register block (bebob.c:35-38,
# bebob_maudio.c:38).  Anything not named here is undocumented by any reference.
BEBOB_INFO_FIELDS: list[tuple[int, int, str]] = [
    (0x00, 8, "magic"),
    (0x08, 4, "BeBoB version"),
    (0x10, 8, "GUID"),
    (0x18, 4, "HW model ID"),
    (0x1C, 4, "HW model revision"),
    (0x20, 16, "SW build date"),
]

# The three cue quadlets that tell a BeBoB bootloader to load firmware from
# flash and reset configuration to factory settings (bebob_maudio.c:41-49,
# written little-endian at :119-126).
MAUDIO_BOOTLOADER_CUE = bytes.fromhex("010000000000110100000000")


def decode_bebob_info(payload: bytes) -> list[str]:
    """Name the fields of the BeBoB info register block."""
    out: list[str] = []
    for offset, size, label in BEBOB_INFO_FIELDS:
        chunk = payload[offset:offset + size]
        if len(chunk) < size:
            continue
        if label in ("magic", "SW build date"):
            text = chunk.split(b"\x00")[0].decode("ascii", "replace")
            # Linux gates firmware support on this date being >= 20070401,
            # i.e. version 5058 or later (bebob_maudio.c:105-113).
            if label == "SW build date" and text[:8].isdigit():
                era = "fw 5058+" if text[:8] >= "20070401" else "PRE-5058, unsupported"
                text = f"{text} ({era})"
        elif size == 8:
            text = (f"{int.from_bytes(chunk[0:4], 'little'):08x}"
                    f"{int.from_bytes(chunk[4:8], 'little'):08x}")
        else:
            text = f"0x{int.from_bytes(chunk, 'little'):08x}"
        out.append(f"+0x{offset:02x} {label}: {text}")
    return out

# The M-Audio parameter window is a write-only register file -- the firmware
# answers no AV/C for any of it, so the register map is the only documentation
# there is.  Transcribed from FFADO, which names both the offsets
# (bebob/maudio/special_avdevice.h:55-134) and what each one drives
# (bebob/maudio/special_mixer.cpp:109-116, :202-237, :307-319, :375-380).
#
# Every entry is one stereo pair packed into a quadlet: upper 16 bits are the
# first channel of the pair, lower 16 the second.
_MAUDIO_GAIN_LABELS = [
    "Stream 1/2 in", "Stream 3/4 in", "Analog 1/2 out", "Analog 3/4 out",
    "Analog 1/2 in", "Analog 3/4 in", "Analog 5/6 in", "Analog 7/8 in",
    "S/PDIF 1/2 in", "ADAT 1/2 in", "ADAT 3/4 in", "ADAT 5/6 in",
    "ADAT 7/8 in", "Aux 1/2 out", "HP 1/2 out", "HP 3/4 out",
]
_MAUDIO_BALANCE_LABELS = [
    "Analog 1/2 in", "Analog 3/4 in", "Analog 5/6 in", "Analog 7/8 in",
    "S/PDIF 1/2 in", "ADAT 1/2 in", "ADAT 3/4 in", "ADAT 5/6 in", "ADAT 7/8 in",
]
_MAUDIO_AUX_LABELS = [
    "Stream 1/2 in", "Stream 3/4 in", "Analog 1/2 in", "Analog 3/4 in",
    "Analog 5/6 in", "Analog 7/8 in", "S/PDIF 1/2 in", "ADAT 1/2 in",
    "ADAT 3/4 in", "ADAT 5/6 in", "ADAT 7/8 in",
]

MAUDIO_PARAM_REGISTERS: dict[int, tuple[str, str]] = {}
for _i, _label in enumerate(_MAUDIO_GAIN_LABELS):
    MAUDIO_PARAM_REGISTERS[0x00 + _i * 4] = ("gain", _label)
for _i, _label in enumerate(_MAUDIO_BALANCE_LABELS):
    MAUDIO_PARAM_REGISTERS[0x40 + _i * 4] = ("balance", _label)
for _i, _label in enumerate(_MAUDIO_AUX_LABELS):
    MAUDIO_PARAM_REGISTERS[0x64 + _i * 4] = ("aux send", _label)
MAUDIO_PARAM_REGISTERS[0x90] = ("routing", "analog/digital in -> mixer")
MAUDIO_PARAM_REGISTERS[0x94] = ("routing", "stream in -> mixer")
MAUDIO_PARAM_REGISTERS[0x98] = ("source", "headphone out")
MAUDIO_PARAM_REGISTERS[0x9C] = ("source", "analog out")

# Bit positions in the two mixer-enable registers, from the shift arithmetic in
# FFADO Processing::setValue (special_mixer.cpp:411-481).
_MIX_IN_BITS = (
    [(b, f"Ana {1 + 2 * b}/{2 + 2 * b} -> Mix 1/2") for b in range(4)] +
    [(4 + b, f"Ana {1 + 2 * b}/{2 + 2 * b} -> Mix 3/4") for b in range(4)] +
    [(8 + b, f"ADAT {1 + 2 * b}/{2 + 2 * b} -> Mix 1/2") for b in range(4)] +
    [(12 + b, f"ADAT {1 + 2 * b}/{2 + 2 * b} -> Mix 3/4") for b in range(4)] +
    [(16, "S/PDIF 1/2 -> Mix 1/2"), (17, "S/PDIF 1/2 -> Mix 3/4")]
)
_MIX_STM_BITS = [(0, "Stm 1/2 -> Mix 1/2"), (1, "Stm 1/2 -> Mix 3/4"),
                 (2, "Stm 3/4 -> Mix 1/2"), (3, "Stm 3/4 -> Mix 3/4")]
_HP_SOURCE = {0x01: "Mix 1/2", 0x02: "Mix 3/4", 0x04: "Aux 1/2"}


def decode_maudio_param(offset: int, value: int) -> str:
    """Human-readable meaning of one write into the M-Audio parameter window."""
    entry = MAUDIO_PARAM_REGISTERS.get(offset)
    if entry is None:
        return ""
    kind, label = entry

    if kind in ("gain", "aux send", "balance"):
        first, second = value >> 16, value & 0xFFFF
        is_balance = kind == "balance"
        text = (f"{kind} {label}: "
                f"ch1 {avc_level(first, mute_at_min=not is_balance)}, "
                f"ch2 {avc_level(second, mute_at_min=not is_balance)}")
        # Opposite signs at near-full deflection is the hard-panned stereo
        # default FFADO also writes at init (special_mixer.cpp:88-89).
        if is_balance:
            a, b = avc_signed(first), avc_signed(second)
            if a * b < 0 and min(abs(a), abs(b)) >= 0x7F00:
                text += "  (hard-panned pair)"
        return text

    if kind == "routing":
        bits = _MIX_IN_BITS if offset == 0x90 else _MIX_STM_BITS
        on = [name for bit, name in bits if (value >> bit) & 1]
        return f"mixer inputs ({label}): " + (", ".join(on) if on else "all off")

    if offset == 0x98:
        lo, hi = value & 0xFFFF, value >> 16
        return (f"HP 1/2 from {_HP_SOURCE.get(lo, f'0x{lo:x}')}, "
                f"HP 3/4 from {_HP_SOURCE.get(hi, f'0x{hi:x}')}")

    return (f"Analog 1/2 from {'Aux 1/2' if value & 1 else 'Mix 1/2'}, "
            f"Analog 3/4 from {'Aux 1/2' if value & 2 else 'Mix 3/4'}")


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
    if BEBOB_REG_INFO <= addr_lo < BEBOB_REG_INFO + 0x1000:
        delta = addr_lo - BEBOB_REG_INFO
        return "BEBOB_INFO" if delta == 0 else f"BEBOB_INFO+0x{delta:x}"
    if BEBOB_REG_REQ <= addr_lo < BEBOB_REG_REQ + 0x1000:
        delta = addr_lo - BEBOB_REG_REQ
        return "BEBOB_REQ" if delta == 0 else f"BEBOB_REQ+0x{delta:x}"
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


# Control selectors, per function block type.
# FFADO libavc/audiosubunit/avc_function_block.h:126-127, :151-163, :202-206.
SELECTOR_CONTROL_SELECTOR = {0x01: "selector"}
FEATURE_CONTROL_SELECTOR = {
    0x01: "mute", 0x02: "volume", 0x03: "LR balance", 0x04: "FR balance",
    0x05: "bass", 0x06: "mid", 0x07: "treble", 0x08: "graphic EQ",
    0x09: "AGC", 0x0A: "delay", 0x0B: "bass boost", 0x0C: "loudness",
}
PROCESSING_CONTROL_SELECTOR = {
    0x01: "enable", 0x02: "mode", 0x03: "mixer", 0xF1: "enhanced mixer",
}


def avc_signed(value: int) -> int:
    """A 16-bit AV/C control value as its signed 1/256 dB integer."""
    return value - 0x10000 if value & 0x8000 else value


def avc_level(value: int, *, mute_at_min: bool = True) -> str:
    """An AV/C audio level: signed 16-bit in 1/256 dB.

    'AV/C Audio Subunit Specification 1.0' (1394TA 1999008) section 10.3.  For
    a gain, 0x8000 is the reserved "negative infinity" code and means mute; for
    a balance it is an ordinary endpoint (full deflection to one side), which is
    what FFADO writes at init -- 0x7ffe8000 -- to hard-pan a pair
    (special_mixer.cpp:88-89).  Callers must say which they have.
    """
    if value == 0x8000 and mute_at_min:
        return "mute"
    return f"{avc_signed(value) / 256:+.2f} dB"


def decode_function_block(operands: bytes) -> str:
    """AV/C Audio Subunit FUNCTION BLOCK (opcode 0xB8), section 10.1.

        fb_type, fb_ID, control_attribute, selector_length,
        <selector_length bytes of selector>, control_selector,
        control_data_length, <control_data>

    The selector's shape depends on fb_type, which is why selector_length is on
    the wire at all: Selector and Feature carry two bytes, Processing four.
    Cross-checked against Linux avc_audio_set_selector (bebob_command.c:20-28)
    and FFADO FunctionBlockProcessing::serialize (avc_function_block.cpp:513-530).
    """
    if len(operands) < 3:
        return ""
    fb_type, fb_id, attribute = operands[0], operands[1], operands[2]
    name = FUNCTION_BLOCK_TYPE.get(fb_type, f"fb 0x{fb_type:02x}")
    attr_name = CONTROL_ATTRIBUTE.get(attribute, f"0x{attribute:02x}")
    text = f"{name} id {fb_id} {attr_name}"

    if len(operands) < 4:
        return text
    length = operands[3]
    selector = operands[4:4 + max(length, 0)]
    tail = operands[4 + max(length, 0):]

    # The last selector byte is the control_selector; what precedes it addresses
    # the point inside the function block that the control applies to.
    if fb_type == 0x82 and len(selector) >= 4:
        control_selector = selector[3]
        names = PROCESSING_CONTROL_SELECTOR
        where = (f"in plug {selector[0]} ch {selector[1]}"
                 f" -> out ch {selector[2]}")
    elif len(selector) >= 2:
        control_selector = selector[1]
        names = FEATURE_CONTROL_SELECTOR if fb_type == 0x81 else SELECTOR_CONTROL_SELECTOR
        where = (f"in plug {selector[0]}" if fb_type == 0x80
                 else f"ch {selector[0]}")
    else:
        return text + " sel " + " ".join(f"{b:02x}" for b in selector)

    control_name = names.get(control_selector, f"control 0x{control_selector:02x}")
    text += f": {control_name}, {where}"

    # control_data_length then control_data.  A 2-byte payload is a level for
    # every control that carries one.
    if len(tail) >= 2:
        data = tail[1:1 + tail[0]]
        if len(data) == 2:
            # Only a Feature balance treats 0x8000 as an endpoint; everything
            # else here is a gain, where it is the mute code.
            balance = fb_type == 0x81 and control_selector in (0x03, 0x04)
            level = avc_level(int.from_bytes(data, "big"), mute_at_min=not balance)
            text += f" = {level}"
        elif data:
            text += " = " + " ".join(f"{b:02x}" for b in data)
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


OUI_NAMES = {0x000D6C: "M-Audio", 0x0007F5: "BridgeCo"}


def decode_unit_info(operands: bytes) -> str:
    """AV/C UNIT INFO (opcode 0x30), AV/C General Specification section 9.1.

    A query fills every operand with 0xff; the response carries a constant 0x07,
    then unit_type/unit_id packed into one byte, then a 24-bit company ID.
    """
    if len(operands) < 5:
        return ""
    if all(b == 0xFF for b in operands[:5]):
        return "query"
    unit_type = (operands[1] >> 3) & 0x1F
    unit_id = operands[1] & 0x07
    company = int.from_bytes(operands[2:5], "big")
    name = AVC_SUBUNIT_TYPE.get(unit_type, f"type-0x{unit_type:02x}")
    vendor = OUI_NAMES.get(company)
    text = f"unit_type {name}, unit_id {unit_id}, company 0x{company:06x}"
    return f"{text} ({vendor})" if vendor else text


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
    elif opcode == 0x30:
        detail = decode_unit_info(operands)
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
    if name.startswith("MAUDIO_METER"):
        return "meter"
    if name.startswith(("MAUDIO", "BEBOB_")):
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

def format_elision(packets: int, gaps: int) -> str:
    """One line for an elided stretch.

    FireBug emits a separate marker per gap, and once the high-rate polling
    categories are suppressed those markers land next to each other; the run is
    summed so the reader sees one hole, not twenty.
    """
    text = f"{'':>14}  ... {packets} packets not shown"
    if gaps > 1:
        text += f" ({gaps} gaps)"
    return text


ROM_RUN_MIN = 3
ROM_RUN_OFFSETS_SHOWN = 6
BLOCK_PARAM_LINES = 8


def summarize_rom_run(run: list[Event], show_payload: bool) -> list[str]:
    """Collapse a run of consecutive Config ROM reads into one line.

    A node polling our ROM header re-reads the same two or three quadlets
    forever, which buries the choreography.  A real ROM *scan* walks increasing
    offsets once each, and its detail is worth keeping -- so the run is only
    collapsed when reads outnumber distinct offsets at least two to one.
    """
    distinct = {ev.addr_lo for ev in run}
    if len(run) < ROM_RUN_MIN or len(distinct) * 2 > len(run):
        out: list[str] = []
        for ev in run:
            out.extend(render(ev, show_payload))
        return out

    counts: dict[str, int] = {}
    values: dict[str, set[int]] = {}
    for ev in run:
        name = decode_address(ev.addr_hi or 0, ev.addr_lo or 0)
        counts[name] = counts.get(name, 0) + 1
        if ev.response is not None and ev.response.value is not None:
            values.setdefault(name, set()).add(ev.response.value)

    ordered = sorted(counts, key=lambda n: (-counts[n], n))
    parts = []
    for name in ordered[:ROM_RUN_OFFSETS_SHOWN]:
        text = f"{name} x{counts[name]}"
        seen = values.get(name)
        if seen and len(seen) == 1:
            text += f" = {next(iter(seen)):08x}"
        elif len(seen or ()) > 1:
            text += " (varies)"
        parts.append(text)
    if len(ordered) > ROM_RUN_OFFSETS_SHOWN:
        parts.append(f"+{len(ordered) - ROM_RUN_OFFSETS_SHOWN} more offsets")

    first = run[0]
    arrow = f"{first.src or '????'}->{first.dst or '????'}"
    span = elapsed_ms(first.ts, run[-1].ts)
    return [f"{first.ts}  {arrow}  READ  Config ROM x{len(run)} over {span:.0f} ms"
            f"  [{'; '.join(parts)}]"]


def render(ev: Event, show_payload: bool) -> list[str]:
    out: list[str] = []
    stamp = str(ev.ts)

    if ev.category == "elision":
        return [format_elision(ev.size or 0, 1)]
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
        if ev.addr_hi == MAUDIO_ADDR_HI and ev.value is not None:
            detail = decode_maudio_param(ev.addr_lo - MAUDIO_PARAM_WINDOW, ev.value)
            if detail:
                out.append(f"{'':>14}  {'':>10}  {detail}")
        if ev.response is not None:
            rc = ev.response.rcode
            name = RCODE_NAMES.get(rc, "?")
            dt = elapsed_ms(ev.ts, ev.response.ts)
            out.append(f"{'':>14}  {'':>10}  -> rCode {rc} ({name})  (+{dt:.1f} ms)")
        return out

    if ev.kind == "Bread":
        out.append(f"{stamp}  {arrow}  BLOCK READ {register} [{ev.size} bytes]")
        data = ev.response.payload if ev.response is not None else b""
        if register.startswith("BEBOB_INFO") and data:
            for field in decode_bebob_info(bytes(data)):
                out.append(f"{'':>14}  {'':>10}  {field}")
        elif show_payload and data:
            out.append(f"{'':>14}  raw: " + " ".join(f"{b:02x}" for b in data[:64]))
        return out

    if ev.kind == "Bwrite":
        out.append(f"{stamp}  {arrow}  BLOCK WRITE {register} [{ev.size} bytes]")
        if register.startswith("BEBOB_REQ"):
            if bytes(ev.payload) == MAUDIO_BOOTLOADER_CUE:
                out.append(f"{'':>14}  {'':>10}  bootloader cue: load firmware "
                           f"from flash + reset config to factory settings")
            elif ev.payload:
                out.append(f"{'':>14}  {'':>10}  unrecognised cue: " +
                           " ".join(f"{b:02x}" for b in ev.payload[:12]))
            return out
        if ev.addr_hi == MAUDIO_ADDR_HI and ev.payload:
            base = ev.addr_lo - MAUDIO_PARAM_WINDOW
            quadlets = len(ev.payload) // 4
            shown = quadlets if show_payload else min(quadlets, BLOCK_PARAM_LINES)
            for i in range(shown):
                word = int.from_bytes(ev.payload[i * 4:i * 4 + 4], "big")
                detail = decode_maudio_param(base + i * 4, word)
                if detail:
                    out.append(f"{'':>14}  {'':>10}  +0x{base + i * 4:02x} = "
                               f"{word:08x}  {detail}")
            if quadlets > shown:
                out.append(f"{'':>14}  {'':>10}  ... {quadlets - shown} more "
                           f"quadlets (--payloads to expand)")
        elif show_payload and ev.payload:
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
    "meter": {"meter"},
    "isoch": {"isoch"},
    "bus": {"bus"},
    "csr": {"csr"},
    "setup": {"avc", "cmp", "irm", "vendor", "isoch", "bus"},
}

# Categories dropped from the default view because they are high-rate polling
# with no bearing on the choreography.  Still counted by --stats, and still
# printable by naming them explicitly in --only.
NOISY_CATEGORIES = {"meter"}


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

    elided_packets = 0
    elided_gaps = 0
    rom_run: list[Event] = []

    def emit(lines: list[str]) -> None:
        """Print lines, flushing any elision run that accumulated before them."""
        nonlocal elided_packets, elided_gaps
        if not lines:
            return
        if elided_gaps:
            print(format_elision(elided_packets, elided_gaps))
            elided_packets = elided_gaps = 0
        for line in lines:
            print(line)

    def flush_rom() -> None:
        nonlocal rom_run
        if rom_run:
            run, rom_run = rom_run, []
            emit(summarize_rom_run(run, args.payloads))

    for ev in events:
        if wanted is None:
            if ev.category in NOISY_CATEGORIES:
                continue
        elif ev.category not in wanted:
            continue

        # Accumulate a run of elision markers and emit it as one line when the
        # next real event arrives.
        if ev.category == "elision":
            elided_packets += ev.size or 0
            elided_gaps += 1
            continue

        # Likewise for Config ROM reads, broken when the conversation changes
        # node pair.
        if ev.category == "rom" and ev.addr_hi is not None:
            if rom_run and (ev.src, ev.dst) != (rom_run[0].src, rom_run[0].dst):
                flush_rom()
            rom_run.append(ev)
            continue

        # Many events render to nothing (responses are printed beneath their
        # request), and those must not break up either run.
        lines = render(ev, args.payloads)
        if not lines:
            continue

        flush_rom()
        emit(lines)

    flush_rom()
    if elided_gaps:
        print(format_elision(elided_packets, elided_gaps))

    if args.unparsed and unparsed:
        print("\n--- unparsed lines ---", file=sys.stderr)
        for line in unparsed[:200]:
            print(line, file=sys.stderr)
        print(f"({len(unparsed)} total)", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
