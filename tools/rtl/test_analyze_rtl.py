#!/usr/bin/env python3
"""Unit tests for analyze_rtl.py -- synthetic WAV and AIFF click trains.

Run directly or via ctest (tests/tools registers it when Python 3 is found):
    python3 -m unittest tools/rtl/test_analyze_rtl.py
"""

from __future__ import annotations

import math
import struct
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_rtl  # noqa: E402

RATE = 48000
BPM = 120.0
FIRST_CLICK = 278          # frames: the value measured from the real Logic capture
FULL_24 = 1 << 23


def click_train(frames: int, first: int, amplitude: int) -> list[int]:
    period = round(RATE * 60.0 / BPM)
    samples = [0] * frames
    for start in range(first, frames, period):
        # A short symmetric pulse; the peak is the centre sample.
        for offset, gain in ((-2, 0.1), (-1, 0.5), (0, 1.0), (1, 0.5), (2, 0.1)):
            index = start + offset
            if 0 <= index < frames:
                samples[index] = int(amplitude * gain)
    return samples


def pack24(value: int, big_endian: bool) -> bytes:
    raw = value & 0xFFFFFF
    b = raw.to_bytes(3, "big")
    return b if big_endian else b[::-1]


def write_wav24(path: Path, samples: list[int]) -> None:
    with wave.open(str(path), "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(3)
        writer.setframerate(RATE)
        writer.writeframes(b"".join(pack24(s, big_endian=False) for s in samples))


def extended(value: float) -> bytes:
    """Encode a positive float as an 80-bit IEEE 754 extended (AIFF COMM rate)."""
    exponent = math.floor(math.log2(value))
    mantissa = int(value / 2.0 ** exponent * (1 << 63))
    return struct.pack(">HQ", exponent + 16383, mantissa)


def write_aiff24(path: Path, samples: list[int], *, aifc_sowt: bool = False,
                 odd_unpadded_chunk: bool = False) -> None:
    big_endian = not aifc_sowt
    comm = struct.pack(">hIh", 1, len(samples), 24) + extended(RATE)
    if aifc_sowt:
        comm += b"sowt" + bytes([0])  # compression id + empty pascal name (padded below)
        comm += b"\x00"
    data = b"".join(pack24(s, big_endian) for s in samples)
    ssnd = struct.pack(">II", 0, 0) + data
    chunks = b"COMM" + struct.pack(">I", len(comm)) + comm
    if len(comm) & 1:
        chunks += b"\x00"
    chunks += b"SSND" + struct.pack(">I", len(ssnd)) + ssnd
    if len(ssnd) & 1 and not odd_unpadded_chunk:
        chunks += b"\x00"
    chunks += b"MARK" + struct.pack(">I", 2) + b"\x00\x00"
    form = b"AIFC" if aifc_sowt else b"AIFF"
    body = form + chunks
    path.write_bytes(b"FORM" + struct.pack(">I", len(body)) + body)


class AnalyzeRtlTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.samples = click_train(RATE * 2, FIRST_CLICK, FULL_24 // 3)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def assert_click_train(self, audio: analyze_rtl.PcmWave) -> None:
        self.assertEqual(audio.sample_rate, RATE)
        self.assertEqual(audio.sample_width, 3)
        self.assertEqual(audio.channels[0], tuple(self.samples))
        first, clicks = analyze_rtl.find_click_train(audio.channels[0], RATE, BPM, None, 8.0)
        self.assertEqual(first, FIRST_CLICK)
        self.assertEqual([peak for _, peak in clicks], [FIRST_CLICK, FIRST_CLICK + 24000,
                                                       FIRST_CLICK + 48000, FIRST_CLICK + 72000])

    def test_wav_24bit(self) -> None:
        path = self.dir / "a.wav"
        write_wav24(path, self.samples)
        self.assert_click_train(analyze_rtl.read_pcm(path))

    def test_aiff_24bit_big_endian(self) -> None:
        path = self.dir / "a.aif"
        write_aiff24(path, self.samples)
        self.assert_click_train(analyze_rtl.read_pcm(path))

    def test_aifc_sowt_little_endian(self) -> None:
        path = self.dir / "a.aifc"
        write_aiff24(path, self.samples, aifc_sowt=True)
        self.assert_click_train(analyze_rtl.read_pcm(path))

    def test_aiff_odd_chunk_without_pad_byte(self) -> None:
        # Logic omits the pad byte after an odd-sized SSND chunk.
        samples = self.samples + [0]  # 3 bytes/frame -> odd SSND size
        self.samples = samples
        path = self.dir / "odd.aif"
        write_aiff24(path, samples, odd_unpadded_chunk=True)
        audio = analyze_rtl.read_pcm(path)
        self.assertEqual(audio.channels[0], tuple(samples))

    def test_extended_float_round_trip(self) -> None:
        for rate in (44100.0, 48000.0, 96000.0, 192000.0):
            self.assertEqual(analyze_rtl._extended_to_float(extended(rate)), rate)

    def test_rejects_non_audio(self) -> None:
        path = self.dir / "junk.bin"
        path.write_bytes(b"not audio at all")
        with self.assertRaises(ValueError):
            analyze_rtl.read_pcm(path)

    def test_threshold_onset_is_before_peak(self) -> None:
        onset, _ = analyze_rtl.threshold_onset(self.samples, FIRST_CLICK, 1.0, RATE)
        self.assertLessEqual(onset, FIRST_CLICK)
        self.assertGreaterEqual(onset, FIRST_CLICK - 2)


if __name__ == "__main__":
    unittest.main()
