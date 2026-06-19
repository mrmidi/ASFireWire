"""Shared serialization helpers for DICE protocol (all big-endian)."""
import struct


def pack_f32(v: float) -> bytes:
    return struct.pack(">f", v)


def unpack_f32(b: bytes) -> float:
    return struct.unpack(">f", b[:4])[0]


def pack_u32(v: int) -> bytes:
    return struct.pack(">I", v)


def unpack_u32(b: bytes) -> int:
    return struct.unpack(">I", b[:4])[0]


def pack_u8_in_quad(v: int) -> bytes:
    """Serialize a u8 value into a 4-byte big-endian quadlet (value in MSB)."""
    return struct.pack(">I", (v & 0xFF) << 24)


def unpack_u8_from_quad(b: bytes) -> int:
    """Deserialize u8 from a 4-byte big-endian quadlet (value in MSB)."""
    val = struct.unpack(">I", b[:4])[0]
    return (val >> 24) & 0xFF


def pack_bool(v: bool) -> bytes:
    return pack_u32(1 if v else 0)


def unpack_bool(b: bytes) -> bool:
    return unpack_u32(b) != 0


def pack_label(s: str, size: int = 64) -> bytes:
    """Pack a label into DICE wire format.

    DICE stores strings with bytes reversed within each 4-byte quadlet (big-endian quad,
    but characters within each quad are in reversed byte order relative to natural order).
    """
    encoded = bytearray(s.encode("ascii", errors="replace")[:size])
    # Pad to size
    buf = bytearray(size)
    buf[: len(encoded)] = encoded
    # Reverse bytes within each 4-byte quadlet
    for i in range(0, size, 4):
        buf[i : i + 4] = buf[i : i + 4][::-1]
    return bytes(buf)


def unpack_label(b: bytes) -> str:
    """Decode a DICE wire-format label (bytes reversed per quadlet, null-terminated)."""
    buf = bytearray(b)
    # Reverse bytes within each 4-byte quadlet
    for i in range(0, len(buf), 4):
        buf[i : i + 4] = buf[i : i + 4][::-1]
    # Find null terminator
    null_pos = buf.find(b"\x00")
    if null_pos != -1:
        buf = buf[:null_pos]
    return buf.decode("ascii", errors="replace")


def _swap_quads(buf: bytearray) -> bytearray:
    """Reverse bytes within each 4-byte quadlet in-place."""
    for i in range(0, len(buf), 4):
        buf[i : i + 4] = buf[i : i + 4][::-1]
    return buf


def pack_labels(labels: list[str], size: int = 256) -> bytes:
    """Serialize a list of 20-byte DICE labels (bytes reversed per quad) into a flat buffer."""
    LABEL_SIZE = 20
    result = bytearray(size)
    for i, label in enumerate(labels):
        offset = i * LABEL_SIZE
        if offset + LABEL_SIZE > size:
            break
        chunk = bytearray(LABEL_SIZE)
        encoded = label.encode("ascii", errors="replace")[:LABEL_SIZE]
        chunk[: len(encoded)] = encoded
        chunk = _swap_quads(chunk)
        result[offset : offset + LABEL_SIZE] = chunk
    return bytes(result)


def unpack_labels(b: bytes) -> list[str]:
    """Deserialize a flat buffer of 20-byte DICE labels (bytes reversed per quad)."""
    LABEL_SIZE = 20
    labels = []
    for offset in range(0, len(b), LABEL_SIZE):
        chunk = bytearray(b[offset : offset + LABEL_SIZE])
        if len(chunk) < LABEL_SIZE:
            break
        chunk = _swap_quads(chunk)
        null_pos = chunk.find(b"\x00")
        if null_pos != -1:
            chunk = chunk[:null_pos]
        labels.append(chunk.decode("ascii", errors="replace"))
    return labels
