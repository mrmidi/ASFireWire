"""Blocking-mode AMDTP cadence: which cycles carry a DATA packet.

In blocking mode every DATA packet carries exactly ``frames_per_data_packet``
frames (the SYT interval: 8 @1x, 16 @2x, 32 @4x) and the remaining cycles carry
NODATA.  The long-run DATA share is ``rate / (8000 * syt_interval)``:

    48 kHz   48000 / 64000  = 0.75      -> D,D,D,N exactly
    44.1 kHz 44100 / 64000  = 0.6890625 -> 441 DATA per 640 cycles, no short period
    96 kHz   96000 / 128000 = 0.75      -> D,D,D,N
    192 kHz  192000/ 256000 = 0.75      -> D,D,D,N

44.1 kHz is the reason ``AudioTimingGeometry`` sizes packet budgets with
``kMinAvgCadence{Packets,Frames} = 80/441`` (5.5125 frames/packet) instead of
48 kHz's 6: it is the worst case, and it has no short repeating pattern.  The
fractional accumulator below reproduces the exact long-run rate at every tier
without special-casing, which is what a device's own packetiser does.
"""

from __future__ import annotations

from dataclasses import dataclass

from .geometry import CYCLES_PER_SECOND

__all__ = ["BlockingCadence"]


@dataclass
class BlockingCadence:
    """Emits the device's DATA/NODATA sequence, one decision per cycle."""

    sample_rate: int
    frames_per_data_packet: int

    _accumulator: int = 0

    def __post_init__(self) -> None:
        # Start half a step in so the first cycle is DATA, matching a device
        # that begins streaming on a data block rather than an idle packet.
        self._accumulator = self._threshold // 2

    @property
    def _threshold(self) -> int:
        return CYCLES_PER_SECOND * self.frames_per_data_packet

    def next_packet_frames(self) -> int:
        """Frames carried by the next cycle: the SYT interval, or 0 for NODATA."""
        self._accumulator += self.sample_rate
        if self._accumulator >= self._threshold:
            self._accumulator -= self._threshold
            return self.frames_per_data_packet
        return 0

    def reset(self) -> None:
        self._accumulator = self._threshold // 2

    # --- analysis helpers ---------------------------------------------------
    def expected_data_fraction(self) -> float:
        return self.sample_rate / self._threshold

    @staticmethod
    def frames_over(cycles: int, sample_rate: int, frames_per_data_packet: int) -> int:
        """Total frames a fresh cadence emits over ``cycles`` cycles."""
        cadence = BlockingCadence(sample_rate, frames_per_data_packet)
        return sum(cadence.next_packet_frames() for _ in range(cycles))
