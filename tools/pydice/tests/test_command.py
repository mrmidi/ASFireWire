"""Tests for codec helpers and FireWireCommand."""
import struct
import pytest
from pydice.protocol.codec import (
    pack_f32, unpack_f32, pack_u32, unpack_u32,
    pack_bool, unpack_bool, pack_label, unpack_label,
)
from pydice.protocol.command import FireWireCommand
from pydice.protocol.constants import FW_BASE, APP_SECTION_BASE


def test_pack_unpack_f32_roundtrip():
    for v in [0.0, 1.0, -1.0, 0.04, 3.14159, -0.9375]:
        assert unpack_f32(pack_f32(v)) == pytest.approx(v, rel=1e-6)


def test_pack_unpack_u32_roundtrip():
    for v in [0, 1, 0xDEADBEEF, 0xFFFFFFFF]:
        assert unpack_u32(pack_u32(v)) == v


def test_pack_u32_big_endian():
    b = pack_u32(0x01020304)
    assert b == bytes([0x01, 0x02, 0x03, 0x04])


def test_pack_bool_true():
    assert unpack_u32(pack_bool(True)) == 1


def test_pack_bool_false():
    assert unpack_u32(pack_bool(False)) == 0


def test_pack_label_roundtrip():
    s = "DesktopKonnekt6"
    b = pack_label(s, 64)
    assert len(b) == 64
    assert unpack_label(b) == s


def test_firewire_command_address():
    cmd = FireWireCommand(
        description="test",
        app_offset=0x000C,
        value=0x42,
        sw_notice=0,
    )
    assert cmd.target_address == FW_BASE + APP_SECTION_BASE + 0x000C


def test_firewire_command_format_display():
    cmd = FireWireCommand(
        description="Output vol",
        app_offset=0x10,
        value=0x0000003C,
        sw_notice=0x05EC,
    )
    display = cmd.format_display()
    assert "WRITE" in display
    assert "0x0000003C" in display.upper() or "3c" in display.lower()
