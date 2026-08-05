"""Faithful model of ``RxSequenceReplayState`` / ``RxSequenceReplayReader``.

Mirrors ``ASFWDriver/Audio/Wire/AMDTP/RxSequenceReplay.hpp``:

* ``Publish``            :162-177   producer cursor, slot = cursor % kCapacity
* ``MarkEstablished``    :179-185   requires producer >= kReadDelay
* ``Begin``              :280-293   anchors at producer - kReadDelay
* ``TryRead``            :295-331   inactive -> epoch -> ahead -> overwritten -> slot
* ``Reset``              :153-160   bumps epoch, clears sequences

The seqlock branches (``kSlotSequenceMismatch`` / ``kSlotChanged``) exist in the
header to survive a torn concurrent read.  This simulator is single-threaded and
publishes atomically, so those two cannot fire here; they are represented in the
failure enum for parity with the driver's telemetry but never produced.  That is
a deliberate modelling limit, not an omission -- a torn read is a concurrency
artefact, and this model is about cursor arithmetic.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

__all__ = [
    "ReplayFailure",
    "ReplayEntry",
    "ReplayDiagnostic",
    "RxSequenceReplayState",
    "RxSequenceReplayReader",
]


class ReplayFailure(str, Enum):
    """Mirrors ``RxSequenceReplayReadFailure`` and its ``…Name()`` strings."""

    NONE = "none"
    READER_INACTIVE = "inactive"
    EPOCH_CHANGED = "epoch"
    AHEAD_OF_PRODUCER = "ahead"
    HISTORY_OVERWRITTEN = "overwritten"
    SLOT_SEQUENCE_MISMATCH = "slot-seq"
    SLOT_EPOCH_MISMATCH = "slot-epoch"
    SLOT_CHANGED = "slot-changed"


@dataclass
class ReplayEntry:
    """Mirrors ``RxSequenceEntry``."""

    first_audio_frame: int = 0
    source_cycle_timer: int = 0
    syt_offset: int = 0xFFFF_FFFF  # kNoInfo
    data_blocks: int = 0
    dbc: int = 0
    valid_syt: bool = False


@dataclass
class ReplayDiagnostic:
    """Mirrors ``RxSequenceReplayReadDiagnostic`` (the ``[TxReplay]`` fields)."""

    failure: ReplayFailure = ReplayFailure.NONE
    reader_cursor: int = 0
    producer_cursor: int = 0
    reader_epoch: int = 0
    replay_epoch: int = 0
    replay_established: bool = False

    @property
    def distance(self) -> int:
        """``d=`` in the driver log: reader - producer (negative == behind)."""
        return self.reader_cursor - self.producer_cursor


@dataclass
class RxSequenceReplayState:
    """The shared, bounded RX observation history the TX producer consumes."""

    capacity: int
    read_delay: int

    _slots: list[ReplayEntry | None] = field(default_factory=list, repr=False)
    _sequences: list[int] = field(default_factory=list, repr=False)
    producer_cursor: int = 0
    epoch: int = 0
    established: bool = False

    def __post_init__(self) -> None:
        if self.capacity & (self.capacity - 1):
            raise ValueError(
                f"replay capacity must be a power of two (header static_assert), "
                f"got {self.capacity}"
            )
        self._slots = [None] * self.capacity
        self._sequences = [0] * self.capacity

    def reset(self) -> None:
        self.established = False
        self.producer_cursor = 0
        self.epoch += 1
        self._sequences = [0] * self.capacity

    def publish(self, entry: ReplayEntry) -> None:
        cursor = self.producer_cursor
        index = cursor % self.capacity
        self._slots[index] = entry
        self._sequences[index] = cursor + 1
        self.producer_cursor = cursor + 1

    def mark_established(self) -> bool:
        if self.producer_cursor < self.read_delay:
            return False
        self.established = True
        return True

    def read(self, cursor: int) -> ReplayEntry | None:
        index = cursor % self.capacity
        if self._sequences[index] != cursor + 1:
            return None
        return self._slots[index]


@dataclass
class RxSequenceReplayReader:
    """The TX-side cursor into the history."""

    next_cursor: int = 0
    epoch: int = 0
    active: bool = False

    def reset(self) -> None:
        self.next_cursor = 0
        self.epoch = 0
        self.active = False

    def begin(self, replay: RxSequenceReplayState) -> bool:
        if not replay.established or replay.producer_cursor < replay.read_delay:
            return False
        self.epoch = replay.epoch
        self.next_cursor = replay.producer_cursor - replay.read_delay
        self.active = True
        return True

    def try_read(
        self, replay: RxSequenceReplayState
    ) -> tuple[ReplayEntry | None, ReplayDiagnostic]:
        diag = ReplayDiagnostic(
            reader_cursor=self.next_cursor,
            producer_cursor=replay.producer_cursor,
            reader_epoch=self.epoch,
            replay_epoch=replay.epoch,
            replay_established=replay.established,
        )
        if not self.active:
            diag.failure = ReplayFailure.READER_INACTIVE
            return None, diag
        if replay.epoch != self.epoch:
            diag.failure = ReplayFailure.EPOCH_CHANGED
            return None, diag
        if self.next_cursor >= replay.producer_cursor:
            diag.failure = ReplayFailure.AHEAD_OF_PRODUCER
            return None, diag
        if replay.producer_cursor - self.next_cursor > replay.capacity:
            diag.failure = ReplayFailure.HISTORY_OVERWRITTEN
            return None, diag

        entry = replay.read(self.next_cursor)
        if entry is None:  # pragma: no cover - unreachable single-threaded
            diag.failure = ReplayFailure.SLOT_SEQUENCE_MISMATCH
            return None, diag

        self.next_cursor += 1
        return entry, diag
