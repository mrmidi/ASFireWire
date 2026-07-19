#!/usr/bin/env python3
"""Model ASFW's coalesced TX data-horizon scheduler under CoreAudio bursts.

This is deliberately narrower than ``tx_payload_ownership_sim.py``.  It
models the implementation introduced for Duet stability:

* CoreAudio emits discrete WriteEnd bursts (normally 512 frames).
* Each burst updates W, then publishes one coalesced preparation request.
* The preparation queue advances E to W + the 400-cycle content horizon when
  it is allowed to run.  It also restores packet preparation to completion +
  the 678-packet lead.
* While the action is delayed, W can cross E (a PayloadWriter
  ``framesWithoutPacket`` loss) independently of descriptor margin.

The model is 48 kHz blocking AMDTP only: D,D,D,N = 8,8,8,0 frames/cycle.
It is a scheduler proof, not a wire/SYT simulator.

Examples:
  python3 tools/tx_data_horizon_burst_sim.py suite
  python3 tools/tx_data_horizon_burst_sim.py run --stall-at 1 --stall-cycles 400
  python3 tools/tx_data_horizon_burst_sim.py run --io-frames 1536 --stall-cycles 250
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace


CYCLES_PER_SECOND = 8_000
SAMPLE_RATE = 48_000
CADENCE = (8, 8, 8, 0)

# Keep these in lock-step with AudioTimingGeometry.hpp.
HW_RING_PACKETS = 48
PREPARATION_LEAD_PACKETS = 678
SHARED_SLOT_PACKETS = 912
CONTENT_HORIZON_PACKETS = 400
DEFAULT_IO_FRAMES = 512


def frames_at_cycle(cycle: int) -> int:
    """Number of 48 kHz blocking frames carried before ``cycle``."""
    blocks, remainder = divmod(cycle, len(CADENCE))
    return blocks * sum(CADENCE) + sum(CADENCE[:remainder])


def horizon_frames(sample_rate: int) -> int:
    return (CONTENT_HORIZON_PACKETS * sample_rate + CYCLES_PER_SECOND - 1) // CYCLES_PER_SECOND


@dataclass(frozen=True)
class Config:
    duration_cycles: int = 8_000
    io_frames: int = DEFAULT_IO_FRAMES
    wake_latency_cycles: int = 0
    stall_at_cycle: int = 1
    stall_cycles: int = 0
    content_horizon_packets: int = CONTENT_HORIZON_PACKETS
    preparation_lead_packets: int = PREPARATION_LEAD_PACKETS
    shared_slot_packets: int = SHARED_SLOT_PACKETS


@dataclass
class Result:
    write_callbacks: int = 0
    preparation_actions: int = 0
    coalesced_requests: int = 0
    dropped_frames: int = 0
    max_exposure_deficit: int = 0
    max_pending_requests: int = 0
    min_descriptor_margin: int = PREPARATION_LEAD_PACKETS
    descriptor_fatal_cycle: int | None = None
    target_limited_actions: int = 0

    @property
    def ok(self) -> bool:
        return self.dropped_frames == 0 and self.descriptor_fatal_cycle is None


def write_cycles(cfg: Config):
    """Yield the integer cycle at which each periodic CoreAudio write lands."""
    write_index = 0
    while True:
        # ceil(write_index * ioFrames / rate * 8000).  The first WriteEnd is
        # at cycle zero, matching the start of the host timeline.
        cycle = (write_index * cfg.io_frames * CYCLES_PER_SECOND + SAMPLE_RATE - 1) // SAMPLE_RATE
        if cycle >= cfg.duration_cycles:
            return
        yield cycle
        write_index += 1


def run(cfg: Config) -> Result:
    if cfg.io_frames <= 0:
        raise ValueError("io_frames must be positive")
    if cfg.preparation_lead_packets + HW_RING_PACKETS > cfg.shared_slot_packets:
        raise ValueError("preparation lead must leave one hardware-ring depth before reuse")

    result = Result(min_descriptor_margin=cfg.preparation_lead_packets)
    writes = iter(write_cycles(cfg))
    next_write = next(writes, None)
    horizon = (cfg.content_horizon_packets * SAMPLE_RATE + CYCLES_PER_SECOND - 1) // CYCLES_PER_SECOND

    write_end = 0
    exposed_end = horizon
    prepared_packet_end = cfg.preparation_lead_packets
    requested_generation = 0
    handled_generation = 0
    wake_pending = False
    wake_due = 0
    stall_end = cfg.stall_at_cycle + cfg.stall_cycles

    for cycle in range(cfg.duration_cycles):
        descriptor_margin = prepared_packet_end - cycle
        result.min_descriptor_margin = min(result.min_descriptor_margin, descriptor_margin)
        if descriptor_margin < HW_RING_PACKETS and result.descriptor_fatal_cycle is None:
            result.descriptor_fatal_cycle = cycle

        # The writer runs first, exactly as WriteEnd observes the *current*
        # exposure. Its request can only improve later writes, never recover a
        # frame that this callback already classified as without-packet.
        while next_write == cycle:
            write_end += cfg.io_frames
            result.write_callbacks += 1
            deficit = max(0, write_end - exposed_end)
            result.dropped_frames += deficit
            result.max_exposure_deficit = max(result.max_exposure_deficit, deficit)

            requested_generation += 1
            if wake_pending:
                result.coalesced_requests += 1
            else:
                wake_pending = True
                wake_due = cycle + cfg.wake_latency_cycles
            result.max_pending_requests = max(
                result.max_pending_requests,
                requested_generation - handled_generation)
            next_write = next(writes, None)

        stalled = cfg.stall_cycles != 0 and cfg.stall_at_cycle <= cycle < stall_end
        if wake_pending and cycle >= wake_due and not stalled:
            # The producer can only prepare through its packet lead. At 48 kHz
            # this is exact cadence rather than a 6-frame average.
            capacity_end = frames_at_cycle(cycle + cfg.preparation_lead_packets)
            target = write_end + horizon
            if target > capacity_end:
                result.target_limited_actions += 1
            exposed_end = max(exposed_end, min(target, capacity_end))
            prepared_packet_end = max(
                prepared_packet_end, cycle + cfg.preparation_lead_packets)
            handled_generation = requested_generation
            wake_pending = False
            result.preparation_actions += 1

    return result


def print_result(label: str, cfg: Config, result: Result) -> None:
    horizon = (cfg.content_horizon_packets * SAMPLE_RATE) / CYCLES_PER_SECOND
    print(label)
    print(
        f"  geometry: horizon={cfg.content_horizon_packets}pkt/{horizon:.0f}fr "
        f"({cfg.content_horizon_packets / 8:.1f}ms) prepLead={cfg.preparation_lead_packets}pkt "
        f"shared={cfg.shared_slot_packets}pkt io={cfg.io_frames}fr")
    print(
        f"  stall: at={cfg.stall_at_cycle} cycles for {cfg.stall_cycles} cycles "
        f"({cfg.stall_cycles / 8:.3f}ms), wakeLatency={cfg.wake_latency_cycles} cycles")
    print(
        f"  result: {'PASS' if result.ok else 'FAIL'} callbacks={result.write_callbacks} "
        f"actions={result.preparation_actions} coalesced={result.coalesced_requests} "
        f"dropFrames={result.dropped_frames} maxDeficit={result.max_exposure_deficit} "
        f"minDescriptorMargin={result.min_descriptor_margin} "
        f"descriptorFatal={result.descriptor_fatal_cycle} targetLimited={result.target_limited_actions}")


def cmd_run(args: argparse.Namespace) -> int:
    cfg = Config(
        duration_cycles=args.duration,
        io_frames=args.io_frames,
        wake_latency_cycles=args.wake_latency,
        stall_at_cycle=args.stall_at,
        stall_cycles=args.stall_cycles,
        content_horizon_packets=args.horizon_packets,
        preparation_lead_packets=args.preparation_lead,
        shared_slot_packets=args.shared_slots,
    )
    result = run(cfg)
    print_result("TX data-horizon burst simulation", cfg, result)
    return 0 if result.ok else 1


def cmd_suite(_args: argparse.Namespace) -> int:
    # The old 576-frame cushion equals 96 packet cycles at 48 kHz. A 22.5 ms
    # queue stall crosses a second 512-frame WriteEnd and loses PCM. The new
    # 400-cycle horizon absorbs a 50 ms queue stall, but correctly does not
    # claim to survive an unbounded stall.
    cases = (
        ("old 96-cycle horizon, 180-cycle stall (must lose PCM)",
         replace(Config(), content_horizon_packets=96, stall_cycles=180), False),
        ("new 400-cycle horizon, 400-cycle stall (must survive)",
         replace(Config(), stall_cycles=400), True),
        ("new 400-cycle horizon, 450-cycle stall (bounded loss)",
         replace(Config(), stall_cycles=450), False),
        ("new horizon, 700-cycle stall (descriptor deadline still wins)",
         replace(Config(), stall_cycles=700), False),
    )
    failed = False
    for label, cfg, expected_ok in cases:
        result = run(cfg)
        print_result(label, cfg, result)
        if result.ok != expected_ok:
            print("  !! unexpected result")
            failed = True
        print()
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    run_parser = sub.add_parser("run", help="run one deterministic burst/stall scenario")
    run_parser.add_argument("--duration", type=int, default=8_000)
    run_parser.add_argument("--io-frames", type=int, default=DEFAULT_IO_FRAMES)
    run_parser.add_argument("--wake-latency", type=int, default=0)
    run_parser.add_argument("--stall-at", type=int, default=1)
    run_parser.add_argument("--stall-cycles", type=int, default=0)
    run_parser.add_argument("--horizon-packets", type=int, default=CONTENT_HORIZON_PACKETS)
    run_parser.add_argument("--preparation-lead", type=int, default=PREPARATION_LEAD_PACKETS)
    run_parser.add_argument("--shared-slots", type=int, default=SHARED_SLOT_PACKETS)
    run_parser.set_defaults(func=cmd_run)

    suite_parser = sub.add_parser("suite", help="run old/new burst-boundary regression cases")
    suite_parser.set_defaults(func=cmd_suite)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
