#!/usr/bin/env python3
"""Inspect a PCM WAV loopback recording and locate a metronome click train.

The script deliberately uses only Python's standard library so it can run on a
clean macOS Python installation.  It reports zero-based audio-frame indices;
for a mono file, one audio frame is one sample.

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


def _decode_sample(raw: bytes, offset: int, width: int) -> int:
    if width == 1:
        return raw[offset] - 128
    if width == 2:
        return struct.unpack_from("<h", raw, offset)[0]
    if width == 3:
        value = raw[offset] | (raw[offset + 1] << 8) | (raw[offset + 2] << 16)
        return value - (1 << 24) if value & 0x800000 else value
    if width == 4:
        return struct.unpack_from("<i", raw, offset)[0]
    raise ValueError(f"unsupported PCM sample width: {width} bytes")


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

    decoded: list[list[int]] = [[] for _ in range(channel_count)]
    for frame in range(frame_count):
        frame_offset = frame * frame_width
        for channel in range(channel_count):
            decoded[channel].append(
                _decode_sample(raw, frame_offset + channel * sample_width, sample_width)
            )

    return PcmWave(
        path=path,
        sample_rate=sample_rate,
        sample_width=sample_width,
        channels=tuple(tuple(channel) for channel in decoded),
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
        description="Report PCM WAV statistics, prominent peaks, and metronome timing."
    )
    parser.add_argument("wav", type=Path, help="uncompressed integer-PCM WAV file")
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
        audio = read_pcm_wave(args.wav)
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
        f"Format: integer PCM, {audio.sample_width * 8}-bit little-endian, "
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
