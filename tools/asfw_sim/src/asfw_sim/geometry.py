"""Geometry the simulator runs on, sourced from the driver headers.

``Geometry.from_headers()`` is the only blessed constructor for a "what the
driver actually does today" run.  Sweeps use :meth:`Geometry.evolve` to vary one
knob at a time so a scenario always states its delta from the real geometry.
"""

from __future__ import annotations

from dataclasses import dataclass, replace

from .headers import DriverHeaders, load_driver_headers

__all__ = [
    "Geometry",
    "CYCLES_PER_SECOND",
    "TICKS_PER_CYCLE",
    "TICKS_PER_SECOND",
    "SUPPORTED_RATES",
    "FUTURE_RATES",
]

CYCLES_PER_SECOND = 8_000
TICKS_PER_CYCLE = 3_072
TICKS_PER_SECOND = CYCLES_PER_SECOND * TICKS_PER_CYCLE  # 24_576_000


@dataclass(frozen=True)
class Geometry:
    """Packet/frame budgets for one simulated stream."""

    sample_rate: int

    # TX packet-domain budgets (AudioTimingGeometry.hpp)
    tx_preparation_lead_packets: int
    tx_coverage_lead_packets: int
    tx_shared_slot_packets: int
    tx_hardware_ring_packets: int
    tx_packets_per_group: int
    tx_data_horizon_packets: int
    timeline_slots: int

    # Frame-domain budgets
    hal_io_period_frames: int
    frame_ring_frames: int
    scheduling_jitter_frames: int

    # Blocking AMDTP
    frames_per_data_packet: int

    # ZTS period (AudioTimingGeometry.hpp)
    hal_zero_timestamp_period_frames: int

    # RX replay ring (RxSequenceReplay.hpp)
    replay_capacity: int
    replay_read_delay: int

    # SYT transfer delays (AudioTransportControlBlock.hpp:321-322 defaults).
    # Equal by default, so their difference drops out of the frame projection.
    rx_transfer_delay_ticks: int = 12_800
    tx_transfer_delay_ticks: int = 12_800

    profile_name: str = "unknown"

    # --- derived ------------------------------------------------------------
    @property
    def data_horizon_frames(self) -> int:
        """``AudioTimingGeometry::TxDataHorizonFrames`` at this rate."""
        return (
            self.tx_data_horizon_packets * self.sample_rate + CYCLES_PER_SECOND - 1
        ) // CYCLES_PER_SECOND

    @property
    def ticks_per_frame(self) -> float:
        return TICKS_PER_SECOND / self.sample_rate

    @property
    def data_packet_fraction(self) -> float:
        """Long-run share of cycles that carry a DATA packet in blocking mode."""
        return self.sample_rate / (CYCLES_PER_SECOND * self.frames_per_data_packet)

    @property
    def replay_headroom_packets(self) -> int:
        """``kReadDelay - lead``: how far the reader can stay behind the producer.

        Negative means the TX preparation frontier structurally outruns the RX
        observations that feed it -- the deadlock this simulator exists to prove.
        """
        return self.replay_read_delay - self.tx_preparation_lead_packets

    @property
    def satisfies_replay_invariant(self) -> bool:
        return (
            self.replay_headroom_packets > 0
            and self.replay_capacity >= 2 * self.replay_read_delay
        )

    # --- construction -------------------------------------------------------
    @classmethod
    def from_headers(
        cls,
        sample_rate: int = 48_000,
        headers: DriverHeaders | None = None,
    ) -> "Geometry":
        h = headers or load_driver_headers()
        return cls(
            sample_rate=sample_rate,
            tx_preparation_lead_packets=h.tx_preparation_lead_packets,
            tx_coverage_lead_packets=h.tx_coverage_lead_packets,
            tx_shared_slot_packets=h.tx_shared_slot_packets,
            tx_hardware_ring_packets=h.tx_hardware_ring_packets,
            tx_packets_per_group=h.tx_packets_per_group,
            tx_data_horizon_packets=h.tx_data_horizon_packets,
            timeline_slots=h.timeline_slots,
            hal_io_period_frames=h.hal_io_period_frames,
            frame_ring_frames=h.frame_ring_frames,
            scheduling_jitter_frames=h.scheduling_jitter_frames,
            frames_per_data_packet=frames_per_data_packet_for(
                sample_rate, h.frames_per_data_packet
            ),
            hal_zero_timestamp_period_frames=h.hal_zero_timestamp_period_frames,
            replay_capacity=h.replay_capacity,
            replay_read_delay=h.replay_read_delay,
            profile_name=h.profile_name,
        )

    def evolve(self, **changes: object) -> "Geometry":
        """Return a copy with named fields replaced (for sweeps)."""
        return replace(self, **changes)  # type: ignore[arg-type]


# Rates the driver actually implements today.  48 kHz is the ceiling; the 2x/4x
# tiers below are carried so the geometry math is already right when they land,
# but they are NOT a validation gate -- do not read a 96/192 kHz sim result as a
# statement about shipped behaviour.
SUPPORTED_RATES: tuple[int, ...] = (44_100, 48_000)
FUTURE_RATES: tuple[int, ...] = (88_200, 96_000, 176_400, 192_000)


def frames_per_data_packet_for(sample_rate: int, base_1x: int) -> int:
    """Blocking SYT interval: 8 @1x, 16 @2x, 32 @4x (IEC 61883-6).

    Cross-checked against the rate ladder recorded in
    ``AudioHalBufferProfiles.hpp`` (``kMaxBlockingFramesPerDataPacket = 32``)
    and ``AudioTimingGeometry.hpp``'s reference block.  Only the 1x tier is
    implemented in the driver today (see ``SUPPORTED_RATES``).
    """
    if sample_rate <= 48_000:
        return base_1x
    if sample_rate <= 96_000:
        return base_1x * 2
    return base_1x * 4
