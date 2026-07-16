#!/usr/bin/env python3
"""Calculate AM824/AMDTP stream geometry for a BeBoB audio device.

The calculator intentionally mirrors
``DuplexStreamProfileResolver::AmdtpBandwidthUnits`` in the driver.  It is a
planning aid: device discovery must still determine the actual PCM and MIDI
slots, stream count, and link speed before ASFW starts a stream.

The packet budget uses an SYT interval-sized blocking packet, rather than the
nominal audio frames in one 125-us bus cycle.  This is the conservative
resource geometry ASFW uses for CMP bandwidth allocation.

Example (the initial PHASE 88 hypothesis):

    python3 tools/bebob_geometry.py --rate 48000 --in-pcm 10 --out-pcm 10
"""

from __future__ import annotations

import argparse
import json
import unittest
from dataclasses import asdict, dataclass
from typing import Sequence


TOTAL_S400_BANDWIDTH_UNITS = 6144
MAX_AM824_DBS = 32


@dataclass(frozen=True)
class RateGeometry:
    sample_rate_hz: int
    nominal_frames_per_cycle: int
    syt_interval_frames: int
    fdf: int


# Keep this table in lock-step with Audio/Wire/AMDTP/AmdtpRateGeometry.hpp.
RATE_GEOMETRIES = {
    32000: RateGeometry(32000, 4, 8, 0),
    44100: RateGeometry(44100, 6, 8, 1),
    48000: RateGeometry(48000, 6, 8, 2),
    88200: RateGeometry(88200, 12, 16, 3),
    96000: RateGeometry(96000, 12, 16, 4),
    176400: RateGeometry(176400, 24, 32, 5),
    192000: RateGeometry(192000, 24, 32, 6),
}


@dataclass(frozen=True)
class DirectionGeometry:
    pcm_slots: int
    midi_slots: int
    dbs: int
    cip_payload_bytes: int
    packet_bytes_at_link_speed: int
    bandwidth_units: int
    s400_capacity_percent: float


@dataclass(frozen=True)
class DuplexGeometry:
    rate: RateGeometry
    speed_mbps: int
    device_to_host: DirectionGeometry
    host_to_device: DirectionGeometry
    total_bandwidth_units: int
    total_s400_capacity_percent: float


def parse_speed(value: str) -> int:
    value = value.upper().removeprefix("S")
    try:
        speed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("speed must be S100, S200, S400, or S800") from error
    if speed not in (100, 200, 400, 800):
        raise argparse.ArgumentTypeError("speed must be S100, S200, S400, or S800")
    return speed


def geometry_for_direction(
    *, pcm_slots: int, midi_slots: int, rate: RateGeometry, speed_mbps: int
) -> DirectionGeometry:
    if pcm_slots < 0 or midi_slots < 0:
        raise ValueError("slot counts cannot be negative")

    dbs = pcm_slots + midi_slots
    if not 1 <= dbs <= MAX_AM824_DBS:
        raise ValueError(f"AM824 DBS must be in 1..{MAX_AM824_DBS}; got {dbs}")

    # AM824 data is one quadlet per DBS slot, plus the two-quadlet CIP header.
    cip_payload_bytes = 8 + rate.syt_interval_frames * dbs * 4
    aligned_payload_bytes = (cip_payload_bytes + 3) & ~3

    # Linux sound/firewire/iso-resources.c accounts for a 3-quadlet isoch
    # header, scales the result to S400 allocation units, then ASFW applies the
    # conservative 512-unit fallback when no optimized gap-count cost exists.
    packet_bytes_at_s400 = 12 + aligned_payload_bytes
    if speed_mbps == 100:
        packet_bytes_at_link_speed = packet_bytes_at_s400 * 4
    elif speed_mbps == 200:
        packet_bytes_at_link_speed = packet_bytes_at_s400 * 2
    elif speed_mbps == 400:
        packet_bytes_at_link_speed = packet_bytes_at_s400
    else:
        packet_bytes_at_link_speed = (packet_bytes_at_s400 + 1) // 2

    bandwidth_units = packet_bytes_at_link_speed + 512
    return DirectionGeometry(
        pcm_slots=pcm_slots,
        midi_slots=midi_slots,
        dbs=dbs,
        cip_payload_bytes=cip_payload_bytes,
        packet_bytes_at_link_speed=packet_bytes_at_link_speed,
        bandwidth_units=bandwidth_units,
        s400_capacity_percent=bandwidth_units / TOTAL_S400_BANDWIDTH_UNITS * 100,
    )


def calculate_duplex_geometry(
    *,
    rate_hz: int,
    input_pcm_slots: int,
    output_pcm_slots: int,
    input_midi_slots: int = 0,
    output_midi_slots: int = 0,
    speed_mbps: int = 400,
) -> DuplexGeometry:
    try:
        rate = RATE_GEOMETRIES[rate_hz]
    except KeyError as error:
        supported = ", ".join(str(value) for value in RATE_GEOMETRIES)
        raise ValueError(f"unsupported rate {rate_hz}; supported rates: {supported}") from error

    device_to_host = geometry_for_direction(
        pcm_slots=input_pcm_slots,
        midi_slots=input_midi_slots,
        rate=rate,
        speed_mbps=speed_mbps,
    )
    host_to_device = geometry_for_direction(
        pcm_slots=output_pcm_slots,
        midi_slots=output_midi_slots,
        rate=rate,
        speed_mbps=speed_mbps,
    )
    total = device_to_host.bandwidth_units + host_to_device.bandwidth_units
    return DuplexGeometry(
        rate=rate,
        speed_mbps=speed_mbps,
        device_to_host=device_to_host,
        host_to_device=host_to_device,
        total_bandwidth_units=total,
        total_s400_capacity_percent=total / TOTAL_S400_BANDWIDTH_UNITS * 100,
    )


def format_geometry(geometry: DuplexGeometry) -> str:
    def format_direction(label: str, direction: DirectionGeometry) -> list[str]:
        return [
            f"{label}:",
            f"  AM824 slots (DBS): {direction.dbs} "
            f"({direction.pcm_slots} PCM + {direction.midi_slots} MIDI)",
            f"  CIP payload:       {direction.cip_payload_bytes} bytes "
            f"(8-byte CIP + {geometry.rate.syt_interval_frames} frames × DBS × 4)",
            f"  Packet cost:       {direction.packet_bytes_at_link_speed} S400 allocation bytes",
            f"  CMP bandwidth:     {direction.bandwidth_units} units "
            f"({direction.s400_capacity_percent:.2f}% of 6144 S400 units)",
        ]

    lines = [
        "BeBoB AM824/AMDTP geometry (ASFW CMP model)",
        f"Rate: {geometry.rate.sample_rate_hz} Hz; FDF/SFC={geometry.rate.fdf}; "
        f"nominal={geometry.rate.nominal_frames_per_cycle} frames/cycle; "
        f"budget SYT interval={geometry.rate.syt_interval_frames} frames",
        f"Link speed: S{geometry.speed_mbps}",
        "",
        *format_direction("Device → host (capture)", geometry.device_to_host),
        "",
        *format_direction("Host → device (playback)", geometry.host_to_device),
        "",
        f"Duplex CMP bandwidth: {geometry.total_bandwidth_units} units "
        f"({geometry.total_s400_capacity_percent:.2f}% of 6144 S400 units)",
    ]
    return "\n".join(lines)


class GeometryTests(unittest.TestCase):
    def test_phase_88_initial_10x10_48k_s400(self) -> None:
        geometry = calculate_duplex_geometry(
            rate_hz=48000, input_pcm_slots=10, output_pcm_slots=10
        )
        self.assertEqual(geometry.rate.syt_interval_frames, 8)
        self.assertEqual(geometry.device_to_host.dbs, 10)
        self.assertEqual(geometry.device_to_host.cip_payload_bytes, 328)
        self.assertEqual(geometry.device_to_host.packet_bytes_at_link_speed, 340)
        self.assertEqual(geometry.device_to_host.bandwidth_units, 852)
        self.assertEqual(geometry.host_to_device.bandwidth_units, 852)
        self.assertEqual(geometry.total_bandwidth_units, 1704)

    def test_midi_slots_are_part_of_dbs(self) -> None:
        geometry = calculate_duplex_geometry(
            rate_hz=48000,
            input_pcm_slots=10,
            input_midi_slots=1,
            output_pcm_slots=10,
        )
        self.assertEqual(geometry.device_to_host.dbs, 11)
        self.assertEqual(geometry.device_to_host.cip_payload_bytes, 360)
        self.assertEqual(geometry.device_to_host.bandwidth_units, 884)

    def test_96k_uses_16_frame_budget_interval(self) -> None:
        geometry = calculate_duplex_geometry(
            rate_hz=96000, input_pcm_slots=10, output_pcm_slots=10
        )
        self.assertEqual(geometry.device_to_host.cip_payload_bytes, 648)
        self.assertEqual(geometry.device_to_host.bandwidth_units, 1172)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rate", type=int, default=48000, help="sample rate in Hz (default: 48000)")
    parser.add_argument("--speed", type=parse_speed, default=400, help="link speed: S100/S200/S400/S800")
    parser.add_argument("--in-pcm", type=int, default=10, help="device-to-host PCM slots (default: 10)")
    parser.add_argument("--out-pcm", type=int, default=10, help="host-to-device PCM slots (default: 10)")
    parser.add_argument("--in-midi-slots", type=int, default=0, help="device-to-host MIDI AM824 slots")
    parser.add_argument("--out-midi-slots", type=int, default=0, help="host-to-device MIDI AM824 slots")
    parser.add_argument("--json", action="store_true", help="emit machine-readable geometry")
    parser.add_argument("--self-test", action="store_true", help="run built-in sample calculations")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(GeometryTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1

    try:
        geometry = calculate_duplex_geometry(
            rate_hz=args.rate,
            input_pcm_slots=args.in_pcm,
            output_pcm_slots=args.out_pcm,
            input_midi_slots=args.in_midi_slots,
            output_midi_slots=args.out_midi_slots,
            speed_mbps=args.speed,
        )
    except ValueError as error:
        raise SystemExit(f"error: {error}") from error

    if args.json:
        print(json.dumps(asdict(geometry), indent=2))
    else:
        print(format_geometry(geometry))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
