"""TX packet timeline, packetiser, and the ``PrepareTransmitSlots`` loop.

Mirrors, in order:

* ``AmdtpPacketTimeline::ExposeDataPacket`` / ``MarkNoDataPacket``
  (``AmdtpPacketTimeline.cpp:77-110``) -- **only a DATA packet advances
  ``exposedFrameEnd_``**.  This single asymmetry is what turns a replay miss
  into a frozen exposure frontier.
* ``AmdtpTxPacketizer::PrepareNextPacket`` (``AmdtpTxPacketizer.cpp:143-220``)
  -- ``isData`` requires ``replayDataBlocks != 0`` when ``replayValid``;
  ``nextAudioFrame_`` advances only on DATA.
* ``PrepareTransmitSlots`` (``ASFWAudioDriverZts.cpp:189-547``) -- the loop
  bound, the ``frameTargetSatisfied`` early exit, and the
  ``ahead`` / ``overwritten`` / epoch branches.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field

from .geometry import TICKS_PER_CYCLE, TICKS_PER_SECOND, Geometry
from .replay import (
    ReplayFailure,
    RxSequenceReplayReader,
    RxSequenceReplayState,
)

__all__ = ["PacketTimeline", "Packetizer", "PrepareOutcome", "prepare_transmit_slots"]


@dataclass
class PacketTimeline:
    """Frame<->packet mapping for packets still resident in the ring."""

    timeline_slots: int
    exposed_frame_end: int = 0
    _data_packets: deque[tuple[int, int, int]] = field(
        default_factory=deque, repr=False
    )  # (packet_index, first_frame, frames)

    def reset(self) -> None:
        self.exposed_frame_end = 0
        self._data_packets.clear()

    def expose_data_packet(
        self, packet_index: int, first_frame: int, frames: int
    ) -> None:
        self._data_packets.append((packet_index, first_frame, frames))
        # Retire packets the ring has overwritten.
        while (
            self._data_packets
            and self._data_packets[0][0] <= packet_index - self.timeline_slots
        ):
            self._data_packets.popleft()
        end = first_frame + frames
        if end > self.exposed_frame_end:
            self.exposed_frame_end = end

    def mark_no_data_packet(self, packet_index: int) -> None:
        """NODATA carries no frames: the exposure frontier does NOT move."""
        while (
            self._data_packets
            and self._data_packets[0][0] <= packet_index - self.timeline_slots
        ):
            self._data_packets.popleft()

    @property
    def first_retained_frame(self) -> int | None:
        """First audio frame still backed by a resident packet slot."""
        return self._data_packets[0][1] if self._data_packets else None

    def covers(self, absolute_frame: int) -> bool:
        """``SnapshotSlotForAudioFrame`` reduced to its decision."""
        if absolute_frame >= self.exposed_frame_end:
            return False
        first = self.first_retained_frame
        return first is not None and absolute_frame >= first


@dataclass
class Packetizer:
    """Mirrors ``AmdtpTxPacketizer``'s frame-cursor half."""

    timeline: PacketTimeline
    next_audio_frame: int = 0
    frame_cursor_aligned: bool = False
    cursor_epoch: int = 0
    align_count: int = 0

    def reset_for_start(self) -> None:
        self.next_audio_frame = 0
        self.frame_cursor_aligned = False
        self.cursor_epoch = 0
        self.align_count = 0
        self.timeline.reset()

    def align_frame_cursor_once(self, frame_index: int) -> bool:
        """``AlignFrameCursorOnce``: a no-op unless alignment is armed."""
        if self.frame_cursor_aligned:
            return False
        self.next_audio_frame = frame_index
        self.frame_cursor_aligned = True
        self.cursor_epoch += 1
        self.align_count += 1
        return True

    def rearm_frame_cursor_alignment(self) -> None:
        self.frame_cursor_aligned = False
        self.cursor_epoch += 1

    def prepare_next_packet(self, packet_index: int, data_blocks: int) -> bool:
        """Returns True if a DATA packet was produced."""
        if data_blocks > 0:
            first = self.next_audio_frame
            self.timeline.expose_data_packet(packet_index, first, data_blocks)
            self.next_audio_frame = first + data_blocks
            return True
        self.timeline.mark_no_data_packet(packet_index)
        return False


@dataclass
class PrepareOutcome:
    """What one ``TxPreparationReady`` wake actually achieved."""

    prepared: int = 0
    data_packets: int = 0
    nodata_packets: int = 0
    failures: dict[ReplayFailure, int] = field(default_factory=dict)
    realigned: int = 0
    reclamped: int = 0
    stopped_short: bool = False
    frame_short: bool = False
    hit_packet_limit: bool = False
    exposed_frame_end_before: int = 0
    exposed_frame_end_after: int = 0

    def note_failure(self, failure: ReplayFailure) -> None:
        self.failures[failure] = self.failures.get(failure, 0) + 1


def prepare_transmit_slots(
    *,
    geometry: Geometry,
    packetizer: Packetizer,
    replay: RxSequenceReplayState,
    reader: RxSequenceReplayReader,
    start_packet_index: int,
    required_packet_index: int,
    limit_packet_index: int,
    max_to_prepare: int,
    target_frame_end: int,
    allow_recovered_clock: bool,
) -> PrepareOutcome:
    """One ``TxPreparationReady`` pass (``ASFWAudioDriverZts.cpp:189-547``)."""
    outcome = PrepareOutcome(
        exposed_frame_end_before=packetizer.timeline.exposed_frame_end
    )
    next_packet = start_packet_index

    def frame_target_satisfied() -> bool:
        return (
            target_frame_end == 0
            or packetizer.timeline.exposed_frame_end >= target_frame_end
        )

    while next_packet < limit_packet_index and outcome.prepared < max_to_prepare:
        if next_packet >= required_packet_index and frame_target_satisfied():
            break

        data_blocks = 0

        if allow_recovered_clock:
            if not reader.active:
                reader.begin(replay)

            entry, diag = reader.try_read(replay)

            # `overwritten` is repositionable, not a fault: re-anchor and retry
            # in place, without re-arming alignment (Zts.cpp:253-273).
            if entry is None and diag.failure is ReplayFailure.HISTORY_OVERWRITTEN:
                outcome.reclamped += 1
                if reader.begin(replay):
                    entry, diag = reader.try_read(replay)

            if entry is not None:
                data_blocks = entry.data_blocks
            else:
                outcome.note_failure(diag.failure)
                if diag.failure is ReplayFailure.AHEAD_OF_PRODUCER:
                    # Hold the reader, ship one NODATA (Zts.cpp:308-317).
                    pass
                else:
                    # A real RX-domain transition: drop the reader and re-arm
                    # the frame cursor (Zts.cpp:318-334).
                    reader.reset()
                    packetizer.rearm_frame_cursor_alignment()

            if entry is not None and entry.data_blocks != 0:
                # The first DATA read after a re-arm re-projects the cursor
                # (Zts.cpp:399-437).  The projection is what compensates for the
                # reader running behind the RX producer: the observation is
                # mapped forward from the cycle it was RECEIVED to the cycle the
                # packet being prepared will be TRANSMITTED.  Dropping this term
                # would make the model lag by the read delay for the wrong reason.
                delta_ticks = (
                    next_packet - entry.source_cycle_timer
                ) * TICKS_PER_CYCLE + (
                    geometry.tx_transfer_delay_ticks - geometry.rx_transfer_delay_ticks
                )
                if delta_ticks >= 0:
                    projected = entry.first_audio_frame + (
                        delta_ticks * geometry.sample_rate // TICKS_PER_SECOND
                    )
                    aligned_frame = (
                        projected // geometry.frames_per_data_packet
                    ) * geometry.frames_per_data_packet
                    if packetizer.align_frame_cursor_once(aligned_frame):
                        outcome.realigned += 1

        is_data = packetizer.prepare_next_packet(next_packet, data_blocks)
        if is_data:
            outcome.data_packets += 1
        else:
            outcome.nodata_packets += 1

        next_packet += 1
        outcome.prepared += 1

    outcome.exposed_frame_end_after = packetizer.timeline.exposed_frame_end
    prepare_until = start_packet_index + outcome.prepared
    outcome.stopped_short = prepare_until < required_packet_index
    outcome.frame_short = target_frame_end != 0 and not frame_target_satisfied()
    outcome.hit_packet_limit = next_packet >= limit_packet_index
    return outcome
