"""Tests ported from tcat/extension/router_entry.rs."""
import pytest
from pydice.protocol.tcat.router_entry import (
    SrcBlk, DstBlk, RouterEntry,
    serialize_router_entry, deserialize_router_entry,
)
from pydice.protocol.constants import SrcBlkId, DstBlkId


def test_dst_blk_serdes():
    params = DstBlk(id=DstBlkId.Ins1, ch=10)
    byte_val = params.serialize_byte()
    p = DstBlk.deserialize_byte(byte_val)
    assert p == params


def test_src_blk_serdes():
    params = SrcBlk(id=SrcBlkId.Ins1, ch=10)
    byte_val = params.serialize_byte()
    p = SrcBlk.deserialize_byte(byte_val)
    assert p == params


def test_dst_blk_byte_encoding():
    # Ins1 = 5 = 0x05, ch=10=0x0A → byte = (5<<4)|10 = 0x5A
    dst = DstBlk(id=DstBlkId.Ins1, ch=10)
    assert dst.serialize_byte() == 0x5A


def test_src_blk_byte_encoding():
    # Ins1 = 5 = 0x05, ch=10=0x0A → byte = (5<<4)|10 = 0x5A
    src = SrcBlk(id=SrcBlkId.Ins1, ch=10)
    assert src.serialize_byte() == 0x5A


def test_router_entry_roundtrip():
    entry = RouterEntry(
        dst=DstBlk(id=DstBlkId.Ins0, ch=0),
        src=SrcBlk(id=SrcBlkId.Aes, ch=3),
        peak=0xABCD,
    )
    raw = serialize_router_entry(entry)
    assert len(raw) == 4
    result = deserialize_router_entry(raw)
    assert result == entry
