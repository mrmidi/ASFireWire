"""FireBug 2.3 log parser → list[LogEvent]."""
import re
import struct
from dataclasses import dataclass, field

_TS = r"(\d+:\d+:\d+)"

_QREAD = re.compile(
    r"^" + _TS + r"\s+Qread from (\S+) to (\S+), tLabel (\d+) \[ack (\d+)\] (\S+)$"
)
_QWRITE = re.compile(
    r"^" + _TS + r"\s+Qwrite from (\S+) to (\S+), value ([0-9a-f]+), tLabel (\d+) \[ack (\d+)\] (\S+)$"
)
_QRRESP = re.compile(
    r"^" + _TS + r"\s+QRresp from (\S+) to (\S+), tLabel (\d+), value ([0-9a-f]+) \[ack (\d+)\] (\S+)$"
)
_WRRESP = re.compile(
    r"^" + _TS + r"\s+WrResp from (\S+) to (\S+), tLabel (\d+), rCode (\d+) \[ack (\d+)\] (\S+)$"
)
_BREAD = re.compile(
    r"^" + _TS + r"\s+Bread from (\S+) to (\S+), size (\d+), tLabel (\d+) \[ack (\d+)\] (\S+)$"
)
_BRRESP = re.compile(
    r"^" + _TS + r"\s+BRresp from (\S+) to (\S+), tLabel (\d+), size (\d+) \[actual \d+\] \[ack (\d+)\] (\S+)$"
)
_BWRITE = re.compile(
    r"^" + _TS + r"\s+Bwrite fr (\S+) to (\S+), sz (\d+) \[actl \d+\], tLab (\d+) \[ack (\d+)\] (\S+)$"
)
_LOCKRQ = re.compile(
    r"^" + _TS + r"\s+LockRq from (\S+) to (\S+), size (\d+), tLabel (\d+) \[ack (\d+)\] (\S+)$"
)
_LOCKRESP = re.compile(
    r"^" + _TS + r"\s+LockResp from (\S+) to (\S+), size (\d+), tLabel (\d+) \[ack (\d+)\] (\S+)$"
)
_BUSRESET = re.compile(r"^" + _TS + r"\s+BUS RESET")
_SELFID = re.compile(r"^" + _TS + r"\s+Self-ID\s+\S+\s+Node=(\d+)")
_CYCLESTART = re.compile(r"^" + _TS + r"\s+CycleStart from ([^,\s]+)")
_PHYRESUME = re.compile(r"^" + _TS + r"\s+PHY Global Resume from node (\d+)")
_HEXDUMP = re.compile(r"^\s+([0-9a-f]{4})\s+((?:[0-9a-f]{8}\s*)+)")


def _split_address(dest_field: str) -> tuple[str, str | None]:
    """Split 'ffc0.ffff.f000.0400' into ('ffc0', 'ffc0.ffff.f000.0400').
    A plain node ID like 'ffc0' returns ('ffc0', None)."""
    if "." in dest_field:
        node = dest_field.split(".")[0]
        return node, dest_field
    return dest_field, None


@dataclass
class LogEvent:
    timestamp: str
    kind: str               # "Qwrite", "Qread", "QRresp", "WrResp", "Bread", "BRresp",
                            # "Bwrite", "LockRq", "LockResp", "BusReset", "SelfID",
                            # "CycleStart", "PHYResume"
    src: str = ""
    dst: str = ""
    address: str | None = None   # full dotted 48-bit address, e.g. "ffc2.ffff.e000.0074"
    tLabel: int | None = None
    value: int | None = None     # quadlet value (Qwrite/QRresp)
    size: int | None = None      # block size
    payload: bytes | None = None # accumulated block data from hex-dump lines
    rcode: int | None = None     # WrResp rCode
    ack: int | None = None
    speed: str | None = None
    raw_line: str = ""


def parse_log(text: str) -> list[LogEvent]:
    """Parse Apple FireBug 2.3 log text into a list of LogEvent objects."""
    events: list[LogEvent] = []
    payload_buf: bytearray = bytearray()
    last_has_payload = False
    # key=(requester_node, tLabel) → destination_address; used to correlate
    # BRresp/QRresp events with their preceding Bread/Qread requests.
    _pending: dict[tuple[str, int], str] = {}

    for line in text.splitlines():
        # Hex-dump continuation — attach to most recent event
        m = _HEXDUMP.match(line)
        if m and last_has_payload and events:
            quad_part = m.group(2)
            for qword in re.findall(r"[0-9a-f]{8}", quad_part):
                payload_buf.extend(bytes.fromhex(qword))
            events[-1].payload = bytes(payload_buf)
            continue

        # Any other line resets payload accumulation
        last_has_payload = False
        payload_buf = bytearray()

        ev = _try_parse_line(line)
        if ev is not None:
            # Propagate request address to matching response via tLabel
            if ev.kind in ("Bread", "Qread", "LockRq") and ev.address is not None and ev.tLabel is not None:
                _pending[(ev.src, ev.tLabel)] = ev.address
            elif ev.kind in ("BRresp", "QRresp", "LockResp") and ev.tLabel is not None:
                key = (ev.dst, ev.tLabel)
                if key in _pending:
                    ev.address = _pending.pop(key)

            events.append(ev)
            if ev.kind in ("BRresp", "Bwrite", "LockRq", "LockResp"):
                last_has_payload = True
                payload_buf = bytearray()

    return events


def _try_parse_line(line: str) -> LogEvent | None:  # noqa: C901
    m = _QREAD.match(line)
    if m:
        ts, src, dest, tlabel, ack, spd = m.group(1, 2, 3, 4, 5, 6)
        dst, addr = _split_address(dest)
        return LogEvent(
            timestamp=ts, kind="Qread",
            src=src, dst=dst, address=addr,
            tLabel=int(tlabel), ack=int(ack), speed=spd,
            raw_line=line,
        )

    m = _QWRITE.match(line)
    if m:
        ts, src, dest, val, tlabel, ack, spd = m.group(1, 2, 3, 4, 5, 6, 7)
        dst, addr = _split_address(dest)
        return LogEvent(
            timestamp=ts, kind="Qwrite",
            src=src, dst=dst, address=addr,
            value=int(val, 16), tLabel=int(tlabel), ack=int(ack), speed=spd,
            raw_line=line,
        )

    m = _QRRESP.match(line)
    if m:
        ts, src, dst, tlabel, val, ack, spd = m.group(1, 2, 3, 4, 5, 6, 7)
        return LogEvent(
            timestamp=ts, kind="QRresp",
            src=src, dst=dst,
            value=int(val, 16), tLabel=int(tlabel), ack=int(ack), speed=spd,
            raw_line=line,
        )

    m = _WRRESP.match(line)
    if m:
        ts, src, dst, tlabel, rcode, ack, spd = m.group(1, 2, 3, 4, 5, 6, 7)
        return LogEvent(
            timestamp=ts, kind="WrResp",
            src=src, dst=dst,
            rcode=int(rcode), tLabel=int(tlabel), ack=int(ack), speed=spd,
            raw_line=line,
        )

    m = _BREAD.match(line)
    if m:
        ts, src, dest, size, tlabel, ack, spd = m.group(1, 2, 3, 4, 5, 6, 7)
        dst, addr = _split_address(dest)
        return LogEvent(
            timestamp=ts, kind="Bread",
            src=src, dst=dst, address=addr,
            size=int(size), tLabel=int(tlabel), ack=int(ack), speed=spd,
            raw_line=line,
        )

    m = _BRRESP.match(line)
    if m:
        ts, src, dst, tlabel, size, ack, spd = m.group(1, 2, 3, 4, 5, 6, 7)
        return LogEvent(
            timestamp=ts, kind="BRresp",
            src=src, dst=dst,
            size=int(size), tLabel=int(tlabel), ack=int(ack), speed=spd,
            raw_line=line,
        )

    m = _BWRITE.match(line)
    if m:
        ts, src, dest, size, tlabel, ack, spd = m.group(1, 2, 3, 4, 5, 6, 7)
        dst, addr = _split_address(dest)
        return LogEvent(
            timestamp=ts, kind="Bwrite",
            src=src, dst=dst, address=addr,
            size=int(size), tLabel=int(tlabel), ack=int(ack), speed=spd,
            raw_line=line,
        )

    m = _LOCKRQ.match(line)
    if m:
        ts, src, dest, size, tlabel, ack, spd = m.group(1, 2, 3, 4, 5, 6, 7)
        dst, addr = _split_address(dest)
        return LogEvent(
            timestamp=ts, kind="LockRq",
            src=src, dst=dst, address=addr,
            size=int(size), tLabel=int(tlabel), ack=int(ack), speed=spd,
            raw_line=line,
        )

    m = _LOCKRESP.match(line)
    if m:
        ts, src, dst, size, tlabel, ack, spd = m.group(1, 2, 3, 4, 5, 6, 7)
        return LogEvent(
            timestamp=ts, kind="LockResp",
            src=src, dst=dst,
            size=int(size), tLabel=int(tlabel), ack=int(ack), speed=spd,
            raw_line=line,
        )

    m = _BUSRESET.match(line)
    if m:
        return LogEvent(timestamp=m.group(1), kind="BusReset", raw_line=line)

    m = _SELFID.match(line)
    if m:
        return LogEvent(
            timestamp=m.group(1), kind="SelfID",
            dst=f"Node={m.group(2)}", raw_line=line,
        )

    m = _CYCLESTART.match(line)
    if m:
        return LogEvent(
            timestamp=m.group(1), kind="CycleStart",
            src=m.group(2), raw_line=line,
        )

    m = _PHYRESUME.match(line)
    if m:
        return LogEvent(
            timestamp=m.group(1), kind="PHYResume",
            dst=f"node {m.group(2)}", raw_line=line,
        )

    return None
