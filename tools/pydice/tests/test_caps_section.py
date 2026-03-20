"""Tests ported from tcat/extension/caps_section.rs."""
from pydice.protocol.tcat.caps_section import (
    ExtensionCaps, RouterCaps, MixerCaps, GeneralCaps, AsicType,
    serialize, deserialize,
)


def test_caps_serdes():
    raw = bytes([
        0xff, 0x00, 0x00, 0x07,
        0x23, 0x12, 0x0c, 0xe7,
        0x00, 0x00, 0x1b, 0xa3,
    ])
    caps = ExtensionCaps(
        router=RouterCaps(
            is_exposed=True,
            is_readonly=True,
            is_storable=True,
            maximum_entry_count=0xff00,
        ),
        mixer=MixerCaps(
            is_exposed=True,
            is_readonly=True,
            is_storable=True,
            input_device_id=0x0e,
            output_device_id=0x0c,
            input_count=0x12,
            output_count=0x23,
        ),
        general=GeneralCaps(
            dynamic_stream_format=True,
            storage_avail=True,
            peak_avail=False,
            max_tx_streams=0x0a,
            max_rx_streams=0x0b,
            stream_format_is_storable=True,
            asic_type=AsicType.DiceII,
        ),
    )

    # serialize and compare with raw
    r = serialize(caps)
    assert r == raw, f"serialize mismatch:\n  got  {r.hex()}\n  want {raw.hex()}"

    # deserialize and compare with caps
    c = deserialize(raw)
    assert c == caps
