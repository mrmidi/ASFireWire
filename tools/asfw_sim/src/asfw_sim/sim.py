"""The duplex run loop: RX publishes, TX prepares, CoreAudio writes.

One simulated step is one FireWire cycle (125 us, 8000/s).  Per cycle:

1. RX receives its packet and publishes one replay entry (a real device emits
   one packet per cycle in each direction -- both FireBug captures in the triage
   docs report 0 silent cycles).
2. The IT context transmits one packet, advancing ``completionCursor``.
3. Every ``kTxPacketsPerGroup`` cycles the refill interrupt schedules a
   ``TxPreparationReady`` wake.
4. Every HAL IO period CoreAudio delivers a callback: ``W`` advances, the
   payload writer runs, and a preparation request is published.

Timing model, deliberately: RX and TX run on the same bus clock, so the RX
replay producer advances at exactly the rate the IT completion cursor does.
That is the *charitable* case for the driver -- no drift, no jitter, no stall.
If the geometry deadlocks here it deadlocks everywhere.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .cadence import BlockingCadence
from .geometry import CYCLES_PER_SECOND, Geometry
from .producer import PacketTimeline, Packetizer, prepare_transmit_slots
from .replay import (
    ReplayEntry,
    ReplayFailure,
    RxSequenceReplayReader,
    RxSequenceReplayState,
)
from .writer import PayloadWriter

__all__ = ["SimConfig", "SimResult", "run", "WARMUP_CYCLES"]

#: Cold-start transient excluded from steady-state metrics (2 s).
WARMUP_CYCLES = 8_000 * 2


@dataclass
class SimConfig:
    geometry: Geometry
    duration_cycles: int = 8_000 * 20  # 20 s
    #: Extra cycles of latency between a wake being requested and running.
    wake_latency_cycles: int = 0
    #: Inject a producer stall: (start_cycle, length_cycles).
    stall_at_cycle: int = 0
    stall_cycles: int = 0
    #: Diagnostic control for P2 -- let the reader see unbounded future history.
    #: Physically impossible; it exists only to isolate R as the cause.
    unbounded_replay_history: bool = False


@dataclass
class SimResult:
    config: SimConfig
    cycles: int = 0

    # transport
    completion_cursor: int = 0
    committed_end: int = 0

    # packets
    data_packets: int = 0
    nodata_packets: int = 0

    # replay
    replay_entries_published: int = 0
    replay_failures: dict[ReplayFailure, int] = field(default_factory=dict)
    reclamped: int = 0
    align_count: int = 0
    min_replay_distance: int = 0  # steady state only, see WARMUP_CYCLES

    # audio content
    write_frontier: int = 0
    exposed_frame_end: int = 0
    frames_visited: int = 0
    frames_written: int = 0
    frames_without_packet: int = 0
    frames_outside_packet: int = 0
    max_deficit_frames: int = 0

    # scheduler
    wakes: int = 0
    wakes_hitting_packet_limit: int = 0
    wakes_frame_short: int = 0

    @property
    def data_packet_fraction(self) -> float:
        total = self.data_packets + self.nodata_packets
        return self.data_packets / total if total else 0.0

    @property
    def written_fraction(self) -> float:
        return self.frames_written / self.frames_visited if self.frames_visited else 0.0

    @property
    def collapsed(self) -> bool:
        """Did the stream reach the observed zombie state?

        Signature from the 2026-07-19 Duet run: transport alive (packets still
        emitted every cycle) while essentially no host PCM reaches the wire.
        """
        return self.written_fraction < 0.5

    def failure_count(self, failure: ReplayFailure) -> int:
        return self.replay_failures.get(failure, 0)

    def summary(self) -> str:
        g = self.config.geometry
        fails = ", ".join(
            f"{f.value}={n}" for f, n in sorted(self.replay_failures.items(), key=lambda kv: -kv[1])
        ) or "none"
        return (
            f"rate={g.sample_rate} lead={g.tx_preparation_lead_packets} "
            f"readDelay={g.replay_read_delay} capacity={g.replay_capacity} "
            f"headroom={g.replay_headroom_packets}\n"
            f"  cycles={self.cycles} dataPkt={self.data_packet_fraction:.3f} "
            f"(expected {g.data_packet_fraction:.3f})\n"
            f"  frames visited={self.frames_visited} written={self.frames_written} "
            f"({self.written_fraction * 100:.1f}%) "
            f"withoutPkt={self.frames_without_packet} "
            f"outsidePkt={self.frames_outside_packet}\n"
            f"  maxDeficit={self.max_deficit_frames} frames  "
            f"TxAlign={self.align_count}  reclamped={self.reclamped}\n"
            f"  replay fail: {fails}\n"
            f"  wakes={self.wakes} hitPacketLimit={self.wakes_hitting_packet_limit} "
            f"frameShort={self.wakes_frame_short}\n"
            f"  verdict: {'COLLAPSED' if self.collapsed else 'healthy'}"
        )


def run(config: SimConfig) -> SimResult:
    g = config.geometry
    result = SimResult(config=config)

    timeline = PacketTimeline(timeline_slots=g.timeline_slots)
    packetizer = Packetizer(timeline=timeline)
    packetizer.reset_for_start()
    writer = PayloadWriter(timeline=timeline)

    capacity = (
        1 << 30 if config.unbounded_replay_history else g.replay_capacity
    )
    replay = RxSequenceReplayState(capacity=capacity, read_delay=g.replay_read_delay)
    reader = RxSequenceReplayReader()

    rx_cadence = BlockingCadence(g.sample_rate, g.frames_per_data_packet)
    rx_frame_cursor = 0

    # --- cold start: PrefillTxRingBeforeStart commits one full ring of NODATA.
    # ASFWAudioDevice::StartIO then validates committedEnd == numSlots before
    # IT arm (ASFWAudioDevice.cpp:341-345).
    for packet_index in range(g.tx_shared_slot_packets):
        packetizer.prepare_next_packet(packet_index, 0)
    committed_end = g.tx_shared_slot_packets
    result.nodata_packets += g.tx_shared_slot_packets

    completion_cursor = 0
    write_frontier = 0
    io_period = g.hal_io_period_frames
    frames_per_cycle = g.sample_rate / CYCLES_PER_SECOND
    next_io_cycle = io_period / frames_per_cycle

    pending_wake_at: int | None = None
    min_distance = 0

    for cycle in range(config.duration_cycles):
        # 1. RX publishes one observation per cycle.
        data_blocks = rx_cadence.next_packet_frames()
        replay.publish(
            ReplayEntry(
                first_audio_frame=rx_frame_cursor,
                source_cycle_timer=cycle,
                syt_offset=0 if data_blocks else 0xFFFF_FFFF,
                data_blocks=data_blocks,
                valid_syt=bool(data_blocks),
            )
        )
        rx_frame_cursor += data_blocks
        result.replay_entries_published += 1
        if not replay.established:
            replay.mark_established()

        # 2. The IT context transmits one packet per cycle.
        completion_cursor = cycle

        stalled = (
            config.stall_cycles > 0
            and config.stall_at_cycle <= cycle < config.stall_at_cycle + config.stall_cycles
        )

        # 3./4. Wake scheduling: refill groups and CoreAudio callbacks.
        wake_due = cycle % g.tx_packets_per_group == 0
        io_due = cycle >= next_io_cycle
        if io_due:
            next_io_cycle += io_period / frames_per_cycle
            write_frontier += io_period
            res = writer.write(write_frontier - io_period, io_period)
            result.frames_visited += res.visited
            result.frames_written += res.written
            result.frames_without_packet += res.without_packet
            result.frames_outside_packet += res.outside_packet
            wake_due = True

        if wake_due and pending_wake_at is None:
            pending_wake_at = cycle + config.wake_latency_cycles

        if stalled or pending_wake_at is None or cycle < pending_wake_at:
            continue
        pending_wake_at = None

        # --- TxPreparationReady (ASFWAudioDriverZts.cpp:610-690) -------------
        target_frame_end = write_frontier + g.data_horizon_frames
        outcome = prepare_transmit_slots(
            geometry=g,
            packetizer=packetizer,
            replay=replay,
            reader=reader,
            start_packet_index=committed_end,
            required_packet_index=completion_cursor + g.tx_coverage_lead_packets,
            limit_packet_index=completion_cursor + g.tx_preparation_lead_packets,
            max_to_prepare=g.tx_preparation_lead_packets,
            target_frame_end=target_frame_end,
            allow_recovered_clock=replay.established,
        )
        committed_end += outcome.prepared
        result.wakes += 1
        result.data_packets += outcome.data_packets
        result.nodata_packets += outcome.nodata_packets
        result.reclamped += outcome.reclamped
        for failure, count in outcome.failures.items():
            result.replay_failures[failure] = (
                result.replay_failures.get(failure, 0) + count
            )
        if outcome.hit_packet_limit:
            result.wakes_hitting_packet_limit += 1
        if outcome.frame_short:
            result.wakes_frame_short += 1

        # Steady-state only.  Cold start has a genuine transient: the ring is
        # prefilled to numSlots (912) while the packet limit is completion+678,
        # so preparation is throttled until completion passes 234 while RX keeps
        # publishing -- the reader falls behind and is reclamped once.  That is
        # real behaviour, reported separately as `reclamped`, but it is not the
        # steady-state lag the F1 claim is about.
        if reader.active and cycle >= WARMUP_CYCLES:
            distance = reader.next_cursor - replay.producer_cursor
            min_distance = min(min_distance, distance)

    result.cycles = config.duration_cycles
    result.completion_cursor = completion_cursor
    result.committed_end = committed_end
    result.write_frontier = write_frontier
    result.exposed_frame_end = timeline.exposed_frame_end
    result.max_deficit_frames = writer.counters.max_deficit_frames
    result.align_count = packetizer.align_count
    result.min_replay_distance = min_distance
    return result
