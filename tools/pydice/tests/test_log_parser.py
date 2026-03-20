"""Tests for FireBug 2.3 log parser, DICE address annotation, and payload decoder."""
import struct
import textwrap
import pytest
from pydice.protocol.log_parser import parse_log, LogEvent
from pydice.protocol.dice_address_map import annotate
from pydice.protocol.payload_decoder import decode_payload

# ── log_parser tests ──────────────────────────────────────────────────────────

QWRITE_LINE = (
    "061:2383:0612  Qwrite from ffc0 to ffc2.ffff.e000.0074, "
    "value 0000020c, tLabel 46 [ack 2] s100"
)

QREAD_LINE = (
    "057:5806:2099  Qread from ffc2 to ffc0.ffff.f000.0400, tLabel 5 [ack 2] s400"
)

QRRESP_LINE = (
    "057:5806:2313  QRresp from ffc0 to ffc2, tLabel 5, value 0404a54b [ack 1] s400"
)

WRRESP_LINE = (
    "061:2400:0917  WrResp from ffc2 to ffc0, tLabel 46, rCode 0 [ack 1] s100"
)

BUSRESET_LINE = (
    "054:7687:1565  BUS RESET ---------------------------------------------------------------------------"
)

BRRESP_BLOCK = textwrap.dedent("""\
    057:7383:2529  BRresp from ffc2 to ffc0, tLabel 39, size 40 [actual 40] [ack 1] s100
                   0000   0000000a 0000005f 00000069 0000008e   ......._...i....
                   0010   000000f7 0000011a 00000211 00000004   ................
                   0020   00000000 00000000                     ........
""")

BWRITE_BLOCK = textwrap.dedent("""\
    061:3597:2306  Bwrite fr ffc0 to ffc2.ffff.e020.06e8, sz 4 [actl 4], tLab 56 [ack 2] s100
                   0000   deadbeef                              ....
""")

LOCKRQ_BLOCK = textwrap.dedent("""\
    061:2295:2967  LockRq from ffc0 to ffc2.ffff.e000.0028, size 16, tLabel 45 [ack 2] s100
                   0000   ffff0000 00000000 ffc000ff 0000d1cc   ................
""")


def test_qwrite_basic_fields():
    events = parse_log(QWRITE_LINE)
    assert len(events) == 1
    ev = events[0]
    assert ev.kind == "Qwrite"
    assert ev.timestamp == "061:2383:0612"
    assert ev.src == "ffc0"
    assert ev.dst == "ffc2"
    assert ev.address == "ffc2.ffff.e000.0074"
    assert ev.value == 0x0000020C
    assert ev.tLabel == 46
    assert ev.ack == 2
    assert ev.speed == "s100"


def test_qread_fields():
    events = parse_log(QREAD_LINE)
    assert len(events) == 1
    ev = events[0]
    assert ev.kind == "Qread"
    assert ev.src == "ffc2"
    assert ev.dst == "ffc0"
    assert ev.address == "ffc0.ffff.f000.0400"
    assert ev.tLabel == 5
    assert ev.ack == 2
    assert ev.speed == "s400"


def test_qrresp_fields():
    events = parse_log(QRRESP_LINE)
    assert len(events) == 1
    ev = events[0]
    assert ev.kind == "QRresp"
    assert ev.value == 0x0404A54B
    assert ev.tLabel == 5
    assert ev.address is None  # responses have no address field


def test_wrresp_fields():
    events = parse_log(WRRESP_LINE)
    assert len(events) == 1
    ev = events[0]
    assert ev.kind == "WrResp"
    assert ev.rcode == 0
    assert ev.tLabel == 46


def test_busreset_kind():
    events = parse_log(BUSRESET_LINE)
    assert len(events) == 1
    assert events[0].kind == "BusReset"
    assert events[0].timestamp == "054:7687:1565"


def test_brresp_payload():
    events = parse_log(BRRESP_BLOCK)
    assert len(events) == 1
    ev = events[0]
    assert ev.kind == "BRresp"
    assert ev.tLabel == 39
    assert ev.size == 40
    assert ev.ack == 1
    assert ev.speed == "s100"
    expected = bytes.fromhex(
        "0000000a" "0000005f" "00000069" "0000008e"
        "000000f7" "0000011a" "00000211" "00000004"
        "00000000" "00000000"
    )
    assert ev.payload == expected


def test_bwrite_payload():
    events = parse_log(BWRITE_BLOCK)
    assert len(events) == 1
    ev = events[0]
    assert ev.kind == "Bwrite"
    assert ev.address == "ffc2.ffff.e020.06e8"
    assert ev.size == 4
    assert ev.payload == bytes.fromhex("deadbeef")


def test_lockrq_payload():
    events = parse_log(LOCKRQ_BLOCK)
    assert len(events) == 1
    ev = events[0]
    assert ev.kind == "LockRq"
    assert ev.size == 16
    assert ev.address == "ffc2.ffff.e000.0028"
    assert ev.payload == bytes.fromhex("ffff0000" "00000000" "ffc000ff" "0000d1cc")


def test_multiple_events_sequence():
    text = "\n".join([QWRITE_LINE, QRRESP_LINE, WRRESP_LINE, BUSRESET_LINE])
    events = parse_log(text)
    assert len(events) == 4
    assert [e.kind for e in events] == ["Qwrite", "QRresp", "WrResp", "BusReset"]


def test_hex_dump_not_attached_to_non_payload_event():
    """Hex dumps after Qwrite/QRresp should be ignored (not attached)."""
    text = textwrap.dedent("""\
        061:2383:0612  Qwrite from ffc0 to ffc2.ffff.e000.0074, value 0000020c, tLabel 46 [ack 2] s100
                       0000   0000000a 0000005f 00000069 0000008e   ........
    """)
    events = parse_log(text)
    assert len(events) == 1
    assert events[0].payload is None


def test_selfid_and_cyclestart():
    text = textwrap.dedent("""\
        054:7687:1565  Self-ID  803fc464  Node=0  Link=0  gap=3f  spd=1394b  C=0  pwr=4: use <3W
        054:7687:2204  CycleStart from ffc2, value 6de08081 = 054:7688:0129 (First one after Bus Reset)
    """)
    events = parse_log(text)
    assert len(events) == 2
    assert events[0].kind == "SelfID"
    assert events[1].kind == "CycleStart"
    assert events[1].src == "ffc2"


# ── dice_address_map tests ────────────────────────────────────────────────────

def test_annotate_clock_config():
    name, decoded = annotate("ffc2.ffff.e000.0074", 0x0000020C)
    assert name == "GLOBAL_CLOCK_SELECT"
    assert decoded == "R48000, Internal"


def test_annotate_clock_config_44100_internal():
    # rate=0x01 (R44100), src=0x0C (Internal) → value = 0x0000010C
    name, decoded = annotate("ffc0.ffff.e000.0074", 0x0000010C)
    assert name == "GLOBAL_CLOCK_SELECT"
    assert "R44100" in decoded
    assert "Internal" in decoded


def test_annotate_enable_true():
    name, decoded = annotate("ffc2.ffff.e000.0078", 1)
    assert name == "GLOBAL_ENABLE"
    assert decoded == "True"


def test_annotate_enable_false():
    name, decoded = annotate("ffc2.ffff.e000.0078", 0)
    assert name == "GLOBAL_ENABLE"
    assert decoded == "False"


def test_annotate_current_rate():
    name, decoded = annotate("ffc2.ffff.e000.0084", 48000)
    assert name == "GLOBAL_SAMPLE_RATE"
    assert decoded == "48000 Hz"


def test_annotate_configrom():
    name, decoded = annotate("ffc0.ffff.f000.0400", 0x0404A54B)
    assert name == "ConfigROM +0x000"
    assert "0x0404a54b" in decoded.lower()


def test_annotate_configrom_ascii():
    # 0x31333934 = "1394" in ASCII
    name, decoded = annotate("ffc0.ffff.f000.0404", 0x31333934)
    assert "1394" in decoded


def test_annotate_sw_notify_latch():
    name, _ = annotate("ffc0.00ff.0000.d1cc", 0x00000023)
    assert name == "SW notify latch"


def test_annotate_global_owner():
    # e000.0028 is now GLOBAL_OWNER
    name, decoded = annotate("ffc2.ffff.e000.0028", 0xFFFF0000)
    assert name == "GLOBAL_OWNER"
    assert decoded == "No owner"


def test_annotate_unknown_address():
    name, decoded = annotate("ffc2.ffff.dead.beef", 0xCAFEBABE)
    # Unknown: name should be the region string, value should be hex
    assert "dead.beef" in name
    assert "cafebabe" in decoded.lower()


def test_annotate_none_address():
    name, decoded = annotate(None, 0x1234)
    assert name == ""
    assert decoded == ""


# ── new register annotation tests ────────────────────────────────────────────

def test_annotate_tx_iso_channel_unused():
    name, decoded = annotate("ffc0.ffff.e000.01ac", 0xFFFFFFFF)
    assert name == "TX[0] ISOCHRONOUS channel"
    assert decoded == "unused (-1)"


def test_annotate_tx_speed_s400():
    name, decoded = annotate("ffc0.ffff.e000.01b8", 2)
    assert name == "TX[0] speed"
    assert decoded == "s400"


def test_annotate_rx_iso_channel_zero():
    name, decoded = annotate("ffc0.ffff.e000.03e4", 0)
    assert name == "RX[0] ISOCHRONOUS channel"
    assert decoded == "channel 0"


def test_annotate_global_notification_bits():
    # 0x23 = CLOCK_ACCEPTED(0x20) | TX_CFG_CHG(0x02) | RX_CFG_CHG(0x01)
    name, decoded = annotate("ffc0.ffff.e000.0030", 0x00000023)
    assert "GLOBAL_NOTIFICATION" in name
    assert "CLOCK_ACCEPTED" in decoded
    assert "TX_CFG_CHG" in decoded
    assert "RX_CFG_CHG" in decoded


def test_annotate_section_layout_header():
    name, decoded = annotate("ffc0.ffff.e000.0000", 0x0000000a)
    assert name == "DICE_GLOBAL_OFFSET"
    assert "10 quadlets" in decoded
    assert "0x28" in decoded


def test_annotate_tx_number():
    name, decoded = annotate("ffc0.ffff.e000.01a4", 2)
    assert name == "TX_NUMBER"
    assert decoded == "2"


def test_annotate_rx_number():
    name, decoded = annotate("ffc0.ffff.e000.03dc", 1)
    assert name == "RX_NUMBER"
    assert decoded == "1"


def test_annotate_tx_speed_s100():
    name, decoded = annotate("ffc0.ffff.e000.01b8", 0)
    assert decoded == "s100"


def test_annotate_tx_speed_s800():
    name, decoded = annotate("ffc0.ffff.e000.01b8", 3)
    assert decoded == "s800"


def test_annotate_tx_speed_unknown():
    name, decoded = annotate("ffc0.ffff.e000.01b8", 9)
    assert "[INVESTIGATE]" in decoded


# ── payload_decoder tests ─────────────────────────────────────────────────────

def _make_global_payload() -> bytes:
    """Build a minimal 96-byte global section with R48000 / Internal."""
    raw = bytearray(96)
    # owner hi = 0xFFFF0000 (no owner sentinel upper quadlet)
    raw[0:4] = struct.pack(">I", 0xFFFF0000)
    raw[4:8] = struct.pack(">I", 0x00000000)
    # clock_config at [76:80]: rate=2(R48000) << 8 | src=12(Internal)
    raw[76:80] = struct.pack(">I", (2 << 8) | 0x0C)
    # enable at [80:84]: 1
    raw[80:84] = struct.pack(">I", 1)
    # clock_status at [84:88]: locked=1, rate=2
    raw[84:88] = struct.pack(">I", (2 << 8) | 1)
    # current_rate at [92:96]: 48000
    raw[92:96] = struct.pack(">I", 48000)
    return bytes(raw)


_SECTION_HEADER_40 = bytes.fromhex(
    "0000000a" "0000005f" "00000069" "0000008e"
    "000000f7" "0000011a" "00000211" "00000004"
    "00000000" "00000000"
)


def test_decode_payload_global_section_clock_select():
    payload = _make_global_payload()
    lines = decode_payload("ffc0.ffff.e000.0028", payload, len(payload))
    combined = "\n".join(lines)
    assert "CLOCK_SELECT" in combined
    assert "R48000" in combined


def test_decode_payload_global_section_owner():
    payload = _make_global_payload()
    lines = decode_payload("ffc0.ffff.e000.0028", payload, len(payload))
    combined = "\n".join(lines)
    assert "OWNER" in combined


def test_decode_payload_section_layout_tx_offset():
    lines = decode_payload("ffc0.ffff.e000.0000", _SECTION_HEADER_40, 40)
    combined = "\n".join(lines)
    assert "DICE_TX_OFFSET" in combined


def test_decode_payload_section_layout_all_sections():
    lines = decode_payload("ffc0.ffff.e000.0000", _SECTION_HEADER_40, 40)
    combined = "\n".join(lines)
    assert "DICE_GLOBAL_OFFSET" in combined
    assert "DICE_RX_OFFSET" in combined
    assert "DICE_EXT_SYNC_OFFSET" in combined


def test_decode_payload_cas_lock_request():
    # LockRq payload: old=0xFFFF000000000000, new=0xffc000ff0000d1cc
    payload = bytes.fromhex("ffff0000" "00000000" "ffc000ff" "0000d1cc")
    lines = decode_payload("ffc0.ffff.e000.0028", payload, 16)
    combined = "\n".join(lines)
    assert "CAS old_val" in combined
    assert "CAS new_val" in combined


def test_decode_payload_none_address_fallback():
    # No address → hex dump fallback
    payload = bytes([0xDE, 0xAD, 0xBE, 0xEF])
    lines = decode_payload(None, payload, 4)
    combined = "\n".join(lines)
    assert "de" in combined.lower()
    assert "ad" in combined.lower()


def test_decode_payload_empty_returns_empty():
    assert decode_payload("ffc0.ffff.e000.0028", b"", None) == []


def test_decode_payload_router_entries():
    # One router entry: 4 bytes
    from pydice.protocol.tcat.router_entry import RouterEntry, DstBlk, SrcBlk, serialize_router_entry
    from pydice.protocol.constants import DstBlkId, SrcBlkId
    entry = RouterEntry(
        dst=DstBlk(id=DstBlkId.Aes, ch=0),
        src=SrcBlk(id=SrcBlkId.Aes, ch=1),
        peak=0,
    )
    payload = serialize_router_entry(entry)
    lines = decode_payload("ffc0.ffff.e020.06e8", payload, len(payload))
    assert len(lines) >= 1
    assert "Aes" in lines[0]


# ── request/response correlation tests ───────────────────────────────────────

BREAD_BRRESP_BLOCK = textwrap.dedent("""\
    057:7383:2000  Bread from ffc0 to ffc2.ffff.e000.0000, size 40, tLabel 39 [ack 2] s100
    057:7383:2529  BRresp from ffc2 to ffc0, tLabel 39, size 40 [actual 40] [ack 1] s100
                   0000   0000000a 0000005f 00000069 0000008e   ......._...i....
                   0010   000000f7 0000011a 00000211 00000004   ................
                   0020   00000000 00000000                     ........
""")

QREAD_QRRESP_PAIR = (
    "057:5806:2099  Qread from ffc2 to ffc0.ffff.f000.0400, tLabel 5 [ack 2] s400\n"
    "057:5806:2313  QRresp from ffc0 to ffc2, tLabel 5, value 0404a54b [ack 1] s400"
)


def test_brresp_gets_address_from_bread():
    events = parse_log(BREAD_BRRESP_BLOCK)
    assert len(events) == 2
    bread, brresp = events
    assert bread.kind == "Bread"
    assert brresp.kind == "BRresp"
    assert brresp.address == "ffc2.ffff.e000.0000"


def test_brresp_payload_decoded_with_correlated_address():
    """After correlation, BRresp payload should decode as DICE section layout."""
    events = parse_log(BREAD_BRRESP_BLOCK)
    brresp = events[1]
    lines = decode_payload(brresp.address, brresp.payload, brresp.size)
    combined = "\n".join(lines)
    assert "DICE_GLOBAL_OFFSET" in combined
    assert "DICE_TX_OFFSET" in combined


def test_qrresp_gets_address_from_qread():
    events = parse_log(QREAD_QRRESP_PAIR)
    assert len(events) == 2
    qread, qrresp = events
    assert qread.kind == "Qread"
    assert qrresp.kind == "QRresp"
    assert qrresp.address == "ffc0.ffff.f000.0400"


def test_correlation_does_not_affect_unmatched_qrresp():
    """A QRresp with no preceding Qread in the same parse should have address=None."""
    events = parse_log(QRRESP_LINE)
    assert events[0].address is None


def test_correlation_cleans_up_pending_on_match():
    """tLabel reuse: second BRresp with same label from different stream should not
    steal the address of an earlier unrelated request."""
    text = textwrap.dedent("""\
        057:0001:0000  Bread from ffc0 to ffc2.ffff.e000.0000, size 40, tLabel 5 [ack 2] s100
        057:0002:0000  BRresp from ffc2 to ffc0, tLabel 5, size 40 [actual 40] [ack 1] s100
                       0000   00000001 00000002 00000003 00000004   ................
        057:0003:0000  BRresp from ffc2 to ffc0, tLabel 5, size 4 [actual 4] [ack 1] s100
                       0000   deadbeef                              ....
    """)
    events = parse_log(text)
    brresps = [e for e in events if e.kind == "BRresp"]
    # First BRresp gets the address; second has no matching pending entry
    assert brresps[0].address == "ffc2.ffff.e000.0000"
    assert brresps[1].address is None


# ── new payload decoder tests ─────────────────────────────────────────────────

def test_decode_nick_name():
    from pydice.protocol.codec import pack_label
    payload = pack_label("Saffire Pro 40", 64)
    lines = decode_payload("ffc0.ffff.e000.0034", payload, len(payload))
    combined = "\n".join(lines)
    assert "Saffire Pro 40" in combined
    assert "Nickname" in combined


def test_decode_clock_source_names():
    from pydice.protocol.tcat.global_section import _serialize_labels
    labels = [
        "AES 1", "AES 2", "AES 3", "AES 4", "AES Any", "ADAT", "TDIF",
        "WC", "Stream-1", "Stream-2", "Stream-3", "Stream-4", "Internal",
    ]
    payload = _serialize_labels(labels, 256)
    lines = decode_payload("ffc0.ffff.e000.0090", payload, len(payload))
    combined = "\n".join(lines)
    assert "Aes1" in combined
    assert "Internal" in combined
    assert "AES 1" in combined


def test_decode_tx_section_basic():
    raw = bytearray()
    raw += struct.pack(">I", 1)           # TX_NUMBER = 1
    raw += struct.pack(">I", 5)           # TX_SIZE = 5 quadlets (20 bytes)
    raw += struct.pack(">I", 0xFFFFFFFF)  # ISO channel = unused
    raw += struct.pack(">I", 16)          # audio channels = 16
    raw += struct.pack(">I", 0)           # MIDI ports = 0
    raw += struct.pack(">I", 2)           # speed = s400
    lines = decode_payload("ffc0.ffff.e000.01a4", bytes(raw), len(raw))
    combined = "\n".join(lines)
    assert "TX_NUMBER: 1" in combined
    assert "TX_SIZE: 5 quadlets" in combined
    assert "unused (-1)" in combined
    assert "16" in combined
    assert "s400" in combined


def test_decode_tx_section_with_channel_names():
    from pydice.protocol.tcat.global_section import _serialize_labels
    names_raw = _serialize_labels(["Analog 1", "Analog 2"], 256)
    raw = bytearray()
    raw += struct.pack(">I", 1)           # TX_NUMBER = 1
    raw += struct.pack(">I", 70)          # TX_SIZE = 70 quadlets (280 bytes)
    raw += struct.pack(">I", 0xFFFFFFFF)  # ISO channel
    raw += struct.pack(">I", 8)           # audio channels
    raw += struct.pack(">I", 0)           # MIDI ports
    raw += struct.pack(">I", 2)           # speed
    raw += names_raw                      # 256 bytes of names
    raw += bytes(280 - 16 - 256)          # pad to fill TX_SIZE stride
    lines = decode_payload("ffc0.ffff.e000.01a4", bytes(raw), len(raw))
    combined = "\n".join(lines)
    assert "TX[0] ch 0: 'Analog 1'" in combined
    assert "TX[0] ch 1: 'Analog 2'" in combined


def test_decode_rx_section_basic():
    raw = bytearray()
    raw += struct.pack(">I", 1)   # RX_NUMBER = 1
    raw += struct.pack(">I", 5)   # RX_SIZE = 5 quadlets
    raw += struct.pack(">I", 3)   # ISO channel = 3
    raw += struct.pack(">I", 0)   # seq start = 0
    raw += struct.pack(">I", 8)   # audio channels = 8
    raw += struct.pack(">I", 1)   # MIDI ports = 1
    lines = decode_payload("ffc0.ffff.e000.03dc", bytes(raw), len(raw))
    combined = "\n".join(lines)
    assert "RX_NUMBER: 1" in combined
    assert "RX_SIZE: 5 quadlets" in combined
    assert "channel 3" in combined
    assert "seq start: 0" in combined
    assert "audio channels: 8" in combined
    assert "MIDI ports: 1" in combined


def test_decode_tcat_ext_header():
    payload = struct.pack(">IIII", 10, 5, 20, 8)  # two offset/size pairs
    lines = decode_payload("ffc0.ffff.e020.0000", payload, len(payload))
    combined = "\n".join(lines)
    assert "TCAT_SECT_0_OFFSET" in combined
    assert "TCAT_SECT_0_SIZE" in combined
    assert "TCAT_SECT_1_OFFSET" in combined
    assert "TCAT_SECT_1_SIZE" in combined
    assert "10 quadlets" in combined


# ── LockRq→LockResp correlation tests ────────────────────────────────────────

LOCKRQ_LOCKRESP_BLOCK = textwrap.dedent("""\
    114:3788:1049  LockRq from ffc0 to ffc2.ffff.f000.0220, size 8, tLabel 15 [ack 2] s100
                   0000   00001333 000011f3                     ...3....
    114:3803:0769  LockResp from ffc2 to ffc0, size 4, tLabel 15 [ack 1] s100
                   0000   00001333                              ...3
""")


def test_lockresp_gets_address_from_lockrq():
    events = parse_log(LOCKRQ_LOCKRESP_BLOCK)
    lockrq, lockresp = events[0], events[1]
    assert lockrq.kind == "LockRq"
    assert lockresp.kind == "LockResp"
    assert lockresp.address == "ffc2.ffff.f000.0220"


# ── IRM payload decoder tests ─────────────────────────────────────────────────

def test_irm_bandwidth_allocate():
    # old=0x000011f3 (4595), new=0x000010f3 (4339) → allocate 256 (0x100) units
    payload = struct.pack(">II", 0x000011f3, 0x000010f3)
    lines = decode_payload("ffc0.ffff.f000.0220", payload, len(payload))
    combined = "\n".join(lines)
    assert "IRM_BANDWIDTH_AVAILABLE" in combined
    assert "old=4595" in combined
    assert "new=4339" in combined
    assert "allocate 256" in combined


def test_irm_bandwidth_release():
    # old=0x000010f3 (4339), new=0x000011f3 (4595) → release 256 units
    payload = struct.pack(">II", 0x000010f3, 0x000011f3)
    lines = decode_payload("ffc0.ffff.f000.0220", payload, len(payload))
    combined = "\n".join(lines)
    assert "release 256" in combined


def test_irm_bandwidth_lockresp():
    # LockResp: 4 bytes = returned old value
    payload = struct.pack(">I", 0x000011f3)
    lines = decode_payload("ffc0.ffff.f000.0220", payload, len(payload))
    combined = "\n".join(lines)
    assert "returned" in combined
    assert "4595" in combined


def test_irm_channels_hi_allocate_ch0():
    # old=0xfffffffe, new=0x7ffffffe → allocate channel 0 (bit 31 cleared)
    payload = struct.pack(">II", 0xfffffffe, 0x7ffffffe)
    lines = decode_payload("ffc0.ffff.f000.0224", payload, len(payload))
    combined = "\n".join(lines)
    assert "IRM_CHANNELS_AVAILABLE_HI" in combined
    assert "allocate channel 0" in combined


def test_irm_channels_hi_allocate_ch1():
    # old=0x7ffffffe, new=0x3ffffffe → allocate channel 1 (bit 30 cleared)
    payload = struct.pack(">II", 0x7ffffffe, 0x3ffffffe)
    lines = decode_payload("ffc0.ffff.f000.0224", payload, len(payload))
    combined = "\n".join(lines)
    assert "allocate channel 1" in combined


def test_irm_channels_hi_release():
    # old=0x3ffffffe, new=0x7ffffffe → release channel 1 (bit 30 set)
    payload = struct.pack(">II", 0x3ffffffe, 0x7ffffffe)
    lines = decode_payload("ffc0.ffff.f000.0224", payload, len(payload))
    combined = "\n".join(lines)
    assert "release channel 1" in combined


def test_irm_channels_lo_no_change():
    # old=new=0xffffffff → compare-verify (broadcast channel trick)
    payload = struct.pack(">II", 0xffffffff, 0xffffffff)
    lines = decode_payload("ffc0.ffff.f000.0228", payload, len(payload))
    combined = "\n".join(lines)
    assert "IRM_CHANNELS_AVAILABLE_LO" in combined
    assert "no change" in combined


def test_irm_channels_lockresp():
    payload = struct.pack(">I", 0x7ffffffe)
    lines = decode_payload("ffc0.ffff.f000.0224", payload, len(payload))
    combined = "\n".join(lines)
    assert "returned" in combined
    assert "0x7ffffffe" in combined


def test_lockresp_decoded_with_correlated_address():
    """After LockRq→LockResp correlation, LockResp payload decodes as IRM."""
    events = parse_log(LOCKRQ_LOCKRESP_BLOCK)
    lockresp = events[1]
    lines = decode_payload(lockresp.address, lockresp.payload, lockresp.size)
    combined = "\n".join(lines)
    assert "IRM_BANDWIDTH_AVAILABLE" in combined
    assert "returned" in combined


def test_annotate_irm_bandwidth():
    from pydice.protocol.dice_address_map import annotate
    name, decoded = annotate("ffc0.ffff.f000.0220", 4595)
    assert name == "IRM_BANDWIDTH_AVAILABLE"
    assert "4595" in decoded


def test_annotate_irm_channels_hi():
    from pydice.protocol.dice_address_map import annotate
    name, decoded = annotate("ffc0.ffff.f000.0224", 0x7ffffffe)
    assert name == "IRM_CHANNELS_AVAILABLE_HI"
