"""Model of ``AmdtpPayloadWriter::WriteFloat32Interleaved``'s classification.

``AmdtpPayloadWriter.cpp:100-111`` is the whole model:

    if (!timeline_->SnapshotSlotForAudioFrame(absoluteFrame, snap)) {
        if (absoluteFrame >= timeline_->ExposedFrameEnd()) ++withoutPacket;
        else                                              ++outsidePacket;
        continue;                       // <- the frame is dropped, not retained
    }

``withoutPacket`` is the ``W > E`` under-exposure loss; ``outsidePacket`` is the
stale/retired-slot race.  The incident signature that matters is
``withoutPacket`` dominating while ``outsidePacket`` stays near zero -- that says
the packets were never prepared, not that they were prepared and reused.
"""

from __future__ import annotations

from dataclasses import dataclass

from .producer import PacketTimeline

__all__ = ["PayloadWriteResult", "PayloadWriterCounters", "PayloadWriter"]


@dataclass
class PayloadWriteResult:
    visited: int = 0
    written: int = 0
    without_packet: int = 0
    outside_packet: int = 0


@dataclass
class PayloadWriterCounters:
    """Cumulative counters, named as in the driver's ``[PayloadWriter]`` record."""

    visited: int = 0
    written: int = 0
    without_packet: int = 0
    outside_packet: int = 0
    under_exposure_calls: int = 0
    under_exposure_frames: int = 0
    max_deficit_frames: int = 0
    first_deficit_frame: int | None = None

    @property
    def written_fraction(self) -> float:
        return self.written / self.visited if self.visited else 0.0


@dataclass
class PayloadWriter:
    timeline: PacketTimeline
    counters: PayloadWriterCounters = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.counters is None:
            self.counters = PayloadWriterCounters()

    def write(self, first_frame: int, frame_count: int) -> PayloadWriteResult:
        """One CoreAudio IO callback's worth of host frames."""
        result = PayloadWriteResult(visited=frame_count)
        write_end = first_frame + frame_count
        exposed_end = self.timeline.exposed_frame_end

        if write_end > exposed_end:
            deficit = write_end - exposed_end
            self.counters.under_exposure_calls += 1
            self.counters.under_exposure_frames += deficit
            if deficit > self.counters.max_deficit_frames:
                self.counters.max_deficit_frames = deficit
            if self.counters.first_deficit_frame is None:
                self.counters.first_deficit_frame = first_frame

        for i in range(frame_count):
            frame = first_frame + i
            if self.timeline.covers(frame):
                result.written += 1
            elif frame >= self.timeline.exposed_frame_end:
                result.without_packet += 1
            else:
                result.outside_packet += 1

        self.counters.visited += result.visited
        self.counters.written += result.written
        self.counters.without_packet += result.without_packet
        self.counters.outside_packet += result.outside_packet
        return result
