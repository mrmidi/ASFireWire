#!/usr/bin/env python3
"""Inspect a PCM WAV/AIFF loopback recording and locate a metronome click train.

This is the DAW-era analysis method: it reads a recording made through a DAW,
which may have compensated the path by the driver's declared latency. For an
uncompensated number use rtl_loopback (see tools/rtl/README.md).

The script deliberately uses only Python's standard library so it can run on a
clean macOS Python installation (the stdlib `aifc` module was removed in
Python 3.13, so AIFF is parsed here directly). It reports zero-based
audio-frame indices; for a mono file, one audio frame is one sample.

Example:
    python3 tools/rtl/analyze_rtl.py recording.wav --bpm 120
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


@dataclass(frozen=True)
class PcmWave:
    path: Path
    sample_rate: int
    sample_width: int
    channels: tuple[tuple[int, ...], ...]

    @property
    def frame_count(self) -> int:
        return len(self.channels[0])

    @property
    def full_scale(self) -> int:
        # Use the magnitude of the most-negative integer as digital full scale.
        return 1 << (self.sample_width * 8 - 1)


def _decode_sample(raw: bytes, offset: int, width: int, big_endian: bool = False,
                   unsigned_8bit: bool = True) -> int:
    if width == 1:
        return raw[offset] - 128 if unsigned_8bit else struct.unpack_from("b", raw, offset)[0]
    order = ">" if big_endian else "<"
    if width == 2:
        return struct.unpack_from(order + "h", raw, offset)[0]
    if width == 3:
        b0, b1, b2 = raw[offset], raw[offset + 1], raw[offset + 2]
        value = (b0 << 16 | b1 << 8 | b2) if big_endian else (b0 | b1 << 8 | b2 << 16)
        return value - (1 << 24) if value & 0x800000 else value
    if width == 4:
        return struct.unpack_from(order + "i", raw, offset)[0]
    raise ValueError(f"unsupported PCM sample width: {width} bytes")


def _deinterleave(raw: bytes, frame_count: int, channel_count: int, sample_width: int,
                  big_endian: bool, unsigned_8bit: bool) -> tuple[tuple[int, ...], ...]:
    frame_width = channel_count * sample_width
    decoded: list[list[int]] = [[] for _ in range(channel_count)]
    for frame in range(frame_count):
        frame_offset = frame * frame_width
        for channel in range(channel_count):
            decoded[channel].append(
                _decode_sample(raw, frame_offset + channel * sample_width, sample_width,
                               big_endian, unsigned_8bit)
            )
    return tuple(tuple(channel) for channel in decoded)


def _extended_to_float(data: bytes) -> float:
    """Decode an 80-bit IEEE 754 extended float (AIFF COMM sample rate)."""
    if len(data) != 10:
        raise ValueError("bad 80-bit extended float")
    exponent = ((data[0] & 0x7F) << 8) | data[1]
    mantissa = int.from_bytes(data[2:10], "big")
    if exponent == 0 and mantissa == 0:
        return 0.0
    if exponent == 0x7FFF:
        raise ValueError("AIFF sample rate is not finite")
    value = mantissa * 2.0 ** (exponent - 16383 - 63)
    return -value if data[0] & 0x80 else value


def read_pcm_aiff(path: Path) -> "PcmWave":
    """Read an uncompressed AIFF or AIFF-C ('NONE' / 'sowt') file."""
    raw_file = path.read_bytes()
    if len(raw_file) < 12 or raw_file[0:4] != b"FORM" or raw_file[8:12] not in (b"AIFF", b"AIFC"):
        raise ValueError("not an AIFF/AIFF-C file")
    is_aifc = raw_file[8:12] == b"AIFC"
    offset = 12
    comm = None
    sound = None
    def plausible_id(at: int) -> bool:
        cid = raw_file[at:at + 4]
        return len(cid) == 4 and all(32 <= c < 127 for c in cid)

    while offset + 8 <= len(raw_file) and (comm is None or sound is None):
        chunk_id = raw_file[offset:offset + 4]
        size = struct.unpack_from(">I", raw_file, offset + 4)[0]
        body = raw_file[offset + 8:offset + 8 + size]
        if len(body) != size:
            raise ValueError(f"truncated AIFF chunk {chunk_id!r}")
        if chunk_id == b"COMM":
            comm = body
        elif chunk_id == b"SSND":
            sound = body
        # Chunks are padded to even length, but some writers (Logic among them)
        # omit the pad after an odd-sized chunk. Follow whichever boundary
        # lands on a plausible chunk id.
        nxt = offset + 8 + size
        if size & 1 and not (plausible_id(nxt + 1)) and plausible_id(nxt):
            offset = nxt
        else:
            offset = nxt + (size & 1)
    if comm is None or sound is None:
        raise ValueError("AIFF is missing its COMM or SSND chunk")
    if len(comm) < 18:
        raise ValueError("AIFF COMM chunk too short")
    channel_count, frame_count, bits = struct.unpack_from(">hIh", comm, 0)
    sample_rate_f = _extended_to_float(comm[8:18])
    big_endian = True
    if is_aifc:
        if len(comm) < 22:
            raise ValueError("AIFF-C COMM chunk too short")
        compression = comm[18:22]
        if compression == b"sowt":
            big_endian = False
        elif compression not in (b"NONE", b"twos"):
            raise ValueError(f"compressed AIFF-C is unsupported ({compression.decode(errors='replace')})")
    if channel_count <= 0 or sample_rate_f <= 0:
        raise ValueError("invalid AIFF channel count or sample rate")
    sample_width = (bits + 7) // 8
    if sample_width not in (1, 2, 3, 4):
        raise ValueError(f"unsupported PCM sample width: {sample_width} bytes")
    data_offset = struct.unpack_from(">I", sound, 0)[0] if len(sound) >= 8 else 0
    raw = sound[8 + data_offset:]
    expected_bytes = frame_count * channel_count * sample_width
    if len(raw) < expected_bytes:
        raise ValueError(f"truncated AIFF: expected {expected_bytes} data bytes, got {len(raw)}")
    return PcmWave(
        path=path,
        sample_rate=int(round(sample_rate_f)),
        sample_width=sample_width,
        # AIFF 8-bit PCM is signed, unlike WAV.
        channels=_deinterleave(raw[:expected_bytes], frame_count, channel_count,
                               sample_width, big_endian, unsigned_8bit=False),
    )


def read_pcm(path: Path) -> "PcmWave":
    """Dispatch on the file's magic bytes, not its extension."""
    with open(path, "rb") as handle:
        head = handle.read(12)
    if head[0:4] == b"FORM":
        return read_pcm_aiff(path)
    return read_pcm_wave(path)


def read_pcm_wave(path: Path) -> PcmWave:
    try:
        with wave.open(str(path), "rb") as reader:
            if reader.getcomptype() != "NONE":
                raise ValueError(
                    f"compressed WAV is unsupported ({reader.getcompname()})"
                )
            channel_count = reader.getnchannels()
            sample_width = reader.getsampwidth()
            sample_rate = reader.getframerate()
            frame_count = reader.getnframes()
            raw = reader.readframes(frame_count)
    except wave.Error as error:
        raise ValueError(f"not a supported PCM WAV: {error}") from error

    if channel_count <= 0 or sample_rate <= 0:
        raise ValueError("invalid WAV channel count or sample rate")
    if sample_width not in (1, 2, 3, 4):
        raise ValueError(f"unsupported PCM sample width: {sample_width} bytes")

    frame_width = channel_count * sample_width
    expected_bytes = frame_count * frame_width
    if len(raw) != expected_bytes:
        raise ValueError(
            f"truncated WAV: expected {expected_bytes} data bytes, got {len(raw)}"
        )

    return PcmWave(
        path=path,
        sample_rate=sample_rate,
        sample_width=sample_width,
        channels=_deinterleave(raw, frame_count, channel_count, sample_width,
                               big_endian=False, unsigned_8bit=True),
    )


def dbfs(value: float, full_scale: int) -> float:
    if value <= 0:
        return float("-inf")
    return 20.0 * math.log10(value / full_scale)


def rms(samples: Sequence[int]) -> float:
    if not samples:
        return 0.0
    return math.sqrt(sum(sample * sample for sample in samples) / len(samples))


def percentile(values: Sequence[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = round((len(ordered) - 1) * min(1.0, max(0.0, fraction)))
    return ordered[index]


def estimate_noise_rms(samples: Sequence[int], sample_rate: int) -> float:
    # A low percentile of 10 ms block RMS values is resistant to clicks and the
    # sustained instrument note, while still describing the recorded floor.
    block_size = max(1, round(sample_rate * 0.010))
    block_rms = [
        rms(samples[start : start + block_size])
        for start in range(0, len(samples), block_size)
    ]
    return percentile(block_rms, 0.10)


def strongest_sample(samples: Sequence[int], start: int, end: int) -> int:
    start = max(0, start)
    end = min(len(samples), end)
    if start >= end:
        raise ValueError("empty peak-search range")
    return max(range(start, end), key=lambda index: abs(samples[index]))


def threshold_onset(
    samples: Sequence[int], peak_index: int, noise_rms: float, sample_rate: int
) -> tuple[int, float]:
    """Return a conservative threshold-based onset immediately before a peak."""
    peak = abs(samples[peak_index])
    threshold = max(peak * 0.005, noise_rms * 8.0)
    search_start = max(0, peak_index - round(sample_rate * 0.050))

    onset = peak_index
    for index in range(peak_index, search_start - 1, -1):
        if abs(samples[index]) < threshold:
            onset = index + 1
            break
    return onset, threshold


def find_click_train(
    samples: Sequence[int],
    sample_rate: int,
    bpm: float,
    search_seconds: float | None,
    search_window_ms: float,
) -> tuple[int, list[tuple[int, int]]]:
    period = sample_rate * 60.0 / bpm
    initial_search_frames = (
        round(search_seconds * sample_rate)
        if search_seconds is not None
        else max(1, round(period))
    )
    first_peak = strongest_sample(samples, 0, initial_search_frames)
    radius = max(1, round(search_window_ms * sample_rate / 1000.0))

    clicks: list[tuple[int, int]] = []
    beat = 0
    while True:
        target = round(first_peak + beat * period)
        if target >= len(samples):
            break
        peak = strongest_sample(samples, target - radius, target + radius + 1)
        clicks.append((target, peak))
        beat += 1
    return first_peak, clicks


def prominent_peaks(
    samples: Sequence[int],
    full_scale: int,
    threshold_dbfs: float,
    minimum_distance: int,
    limit: int,
) -> list[int]:
    threshold = full_scale * (10.0 ** (threshold_dbfs / 20.0))
    candidates = [
        index
        for index in range(1, len(samples) - 1)
        if abs(samples[index]) >= threshold
        and abs(samples[index]) >= abs(samples[index - 1])
        and abs(samples[index]) > abs(samples[index + 1])
    ]
    candidates.sort(key=lambda index: abs(samples[index]), reverse=True)

    selected: list[int] = []
    for index in candidates:
        if all(abs(index - existing) >= minimum_distance for existing in selected):
            selected.append(index)
            if len(selected) == limit:
                break
    return sorted(selected)


def format_db(value: float) -> str:
    return "-inf" if not math.isfinite(value) else f"{value:.2f}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Report PCM WAV/AIFF statistics, prominent peaks, and metronome timing."
    )
    parser.add_argument("wav", type=Path, help="uncompressed integer-PCM WAV or AIFF file")
    parser.add_argument("--bpm", type=float, default=120.0)
    parser.add_argument("--beats-per-bar", type=int, default=4)
    parser.add_argument(
        "--channel",
        type=int,
        default=1,
        help="one-based channel used for click analysis (default: 1)",
    )
    parser.add_argument(
        "--first-search-seconds",
        type=float,
        help="first-click search span (default: one beat)",
    )
    parser.add_argument(
        "--beat-search-ms",
        type=float,
        default=8.0,
        help="peak-search radius around each predicted beat (default: 8)",
    )
    parser.add_argument("--top-peaks", type=int, default=16)
    parser.add_argument("--peak-threshold-dbfs", type=float, default=-60.0)
    parser.add_argument("--peak-min-distance-ms", type=float, default=100.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.bpm <= 0 or args.beats_per_bar <= 0:
        print("error: --bpm and --beats-per-bar must be positive", file=sys.stderr)
        return 2
    if args.first_search_seconds is not None and args.first_search_seconds <= 0:
        print("error: --first-search-seconds must be positive", file=sys.stderr)
        return 2
    if args.beat_search_ms <= 0 or args.peak_min_distance_ms <= 0:
        print("error: peak-search distances must be positive", file=sys.stderr)
        return 2
    if args.top_peaks < 0:
        print("error: --top-peaks cannot be negative", file=sys.stderr)
        return 2

    try:
        audio = read_pcm(args.wav)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    if not 1 <= args.channel <= len(audio.channels):
        print(
            f"error: --channel must be between 1 and {len(audio.channels)}",
            file=sys.stderr,
        )
        return 2

    selected = audio.channels[args.channel - 1]
    duration = audio.frame_count / audio.sample_rate
    print(f"File: {audio.path}")
    print(
        f"Format: integer PCM, {audio.sample_width * 8}-bit, "
        f"{audio.sample_rate} Hz, {len(audio.channels)} channel(s)"
    )
    print(f"Length: {audio.frame_count} frames ({duration:.6f} s)")

    for channel_number, channel in enumerate(audio.channels, start=1):
        peak_index = max(range(len(channel)), key=lambda index: abs(channel[index]))
        channel_rms = rms(channel)
        mean = sum(channel) / len(channel)
        print(
            f"Channel {channel_number}: peak={channel[peak_index]} "
            f"at frame {peak_index} ({peak_index / audio.sample_rate:.6f} s), "
            f"peak={format_db(dbfs(abs(channel[peak_index]), audio.full_scale))} dBFS, "
            f"RMS={format_db(dbfs(channel_rms, audio.full_scale))} dBFS, "
            f"DC={mean:.2f}"
        )

    noise_rms = estimate_noise_rms(selected, audio.sample_rate)
    first_peak, click_train = find_click_train(
        selected,
        audio.sample_rate,
        args.bpm,
        args.first_search_seconds,
        args.beat_search_ms,
    )
    onset, onset_threshold = threshold_onset(
        selected, first_peak, noise_rms, audio.sample_rate
    )

    print()
    print(f"Analysis channel: {args.channel}")
    print(
        f"Estimated floor: {format_db(dbfs(noise_rms, audio.full_scale))} dBFS "
        "(10th percentile of 10 ms RMS blocks)"
    )
    print(
        f"First click peak: frame {first_peak} "
        f"({first_peak / audio.sample_rate:.6f} s), "
        f"{format_db(dbfs(abs(selected[first_peak]), audio.full_scale))} dBFS"
    )
    print(f"Samples before first click peak: {first_peak} (zero-based frame index)")
    print(
        f"Threshold onset: frame {onset} ({onset / audio.sample_rate:.6f} s), "
        f"{first_peak - onset} frames before peak; threshold="
        f"{format_db(dbfs(onset_threshold, audio.full_scale))} dBFS"
    )

    period = audio.sample_rate * 60.0 / args.bpm
    print()
    print(
        f"Metronome grid: {args.bpm:g} BPM, period={period:.3f} frames; "
        f"search radius=+/-{args.beat_search_ms:g} ms"
    )
    print("beat  bar.beat  target    peak   delta       time  peak dBFS")
    for beat_index, (target, peak) in enumerate(click_train):
        bar = beat_index // args.beats_per_bar + 1
        beat_in_bar = beat_index % args.beats_per_bar + 1
        print(
            f"{beat_index + 1:>4}  {bar}.{beat_in_bar:<4}  {target:>7}  {peak:>7}  "
            f"{peak - target:>+6}  {peak / audio.sample_rate:>9.6f}  "
            f"{format_db(dbfs(abs(selected[peak]), audio.full_scale)):>9}"
        )

    if args.top_peaks:
        minimum_distance = max(
            1, round(args.peak_min_distance_ms * audio.sample_rate / 1000.0)
        )
        peaks = prominent_peaks(
            selected,
            audio.full_scale,
            args.peak_threshold_dbfs,
            minimum_distance,
            args.top_peaks,
        )
        print()
        print(
            f"Prominent peaks: >= {args.peak_threshold_dbfs:g} dBFS, "
            f">= {args.peak_min_distance_ms:g} ms apart"
        )
        print(" frame       time  sample  peak dBFS")
        for peak in peaks:
            print(
                f"{peak:>6}  {peak / audio.sample_rate:>9.6f}  "
                f"{selected[peak]:>7}  "
                f"{format_db(dbfs(abs(selected[peak]), audio.full_scale)):>9}"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
