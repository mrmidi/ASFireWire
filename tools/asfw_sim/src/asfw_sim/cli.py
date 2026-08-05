"""Command line entry point for asfw_sim.

With no arguments it prints the complete live geometry of the driver in the
working tree -- every header constant, everything derived from it, and the
budgets that are not written down anywhere.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from .capture import capture_from_responses, load_capture
from .derive import derive, solve_for_io_budget
from .diagnose import diagnose, fingerprints, frames_lost_per_drop
from .geometry import SUPPORTED_RATES, Geometry
from .headers import DriverHeaders, load_driver_headers
from .scenarios import Scenario, load_scenario, load_scenario_dir, run_scenario
from .sim import SimConfig, run

CYCLES = 8_000


def _default_scenario_dir() -> Path:
    """The scenarios shipped alongside the package."""
    return Path(__file__).resolve().parents[2] / "scenarios"


# --- helpers ------------------------------------------------------------------


def _rule(title: str) -> None:
    print(f"\n\033[1m{title}\033[0m")
    print("-" * max(len(title), 68))


def _row(name: str, value, unit: str = "", note: str = "") -> None:
    tail = f"  {note}" if note else ""
    print(f"  {name:<36} {str(value):>10} {unit:<10}{tail}")


def _ms(packets: int) -> str:
    return f"{packets / 8:.1f} ms"


def _geometry(args) -> Geometry:
    g = Geometry.from_headers(args.rate)
    changes = {}
    if getattr(args, "read_delay", 0):
        changes["replay_read_delay"] = args.read_delay
        changes["replay_capacity"] = max(512, 2 * args.read_delay)
    if getattr(args, "capacity", 0):
        changes["replay_capacity"] = args.capacity
    if getattr(args, "horizon", 0):
        changes["tx_data_horizon_packets"] = args.horizon
    return g.evolve(**changes) if changes else g


def _run(geometry: Geometry, seconds: int, stall_ms: float, stall_at_s: int = 4):
    return run(
        SimConfig(
            geometry=geometry,
            duration_cycles=CYCLES * seconds,
            stall_at_cycle=CYCLES * stall_at_s,
            stall_cycles=int(stall_ms * 8),
        )
    )


def _cliff(geometry: Geometry, seconds: int = 15, hi: int = 6_000) -> int:
    lo = 0
    while lo < hi - 1:
        mid = (lo + hi) // 2
        if _run(geometry, seconds, mid / 8).collapsed:
            hi = mid
        else:
            lo = mid
    return lo


# --- the default report -------------------------------------------------------


def cmd_geometry(args) -> int:
    h: DriverHeaders = load_driver_headers()
    t, r = h.timing, h.replay

    print("\033[1mASFireWire live geometry\033[0m")
    print(f"  source: {t.path.parent}")
    print(f"  active HAL buffer profile: {h.profile_name}")

    _rule("HAL / CoreAudio buffers")
    _row("kFrameRingFrames", t["kFrameRingFrames"], "frames",
         f"{t['kFrameRingFrames'] / 48:.1f} ms @48k -- shared stream ring")
    _row("kHalIoPeriodFrames", t["kHalIoPeriodFrames"], "frames",
         "client IO budget (max CoreAudio callback)")
    _row("kHalZeroTimestampPeriodFrames", t["kHalZeroTimestampPeriodFrames"], "frames",
         "ZTS anchor period")
    _row("kFrameAlignment", t["kFrameAlignment"], "frames")
    _row("kSchedulingJitterFrames", t["kSchedulingJitterFrames"], "frames",
         "single-queue contention cushion")

    _rule("Blocking AMDTP cadence")
    _row("kSampleRateHz", t["kSampleRateHz"], "Hz", "geometry reference rate")
    _row("kFramesPerDataPacket", t["kFramesPerDataPacket"], "frames", "SYT interval @1x")
    _row("kCadenceBlockPackets", t["kCadenceBlockPackets"], "packets")
    _row("kCadenceBlockFrames", t["kCadenceBlockFrames"], "frames")
    _row("kMinAvgCadencePackets", t["kMinAvgCadencePackets"], "packets",
         "worst-case (44.1k) cadence:")
    _row("kMinAvgCadenceFrames", t["kMinAvgCadenceFrames"], "frames",
         f"{t['kMinAvgCadenceFrames'] / t['kMinAvgCadencePackets']:.4f} frames/packet")

    _rule("Interrupt / DMA cadence")
    _row("kTxPacketsPerGroup", t["kTxPacketsPerGroup"], "packets", _ms(t["kTxPacketsPerGroup"]))
    _row("kRxPacketsPerGroup", t["kRxPacketsPerGroup"], "packets", _ms(t["kRxPacketsPerGroup"]))
    _row("kRxDescriptorPackets", t["kRxDescriptorPackets"], "packets", _ms(t["kRxDescriptorPackets"]))
    _row("kTxHardwareRingPackets", t["kTxHardwareRingPackets"], "packets",
         f"{_ms(t['kTxHardwareRingPackets'])} OHCI IT ring")

    _rule("TX packet budgets")
    _row("kTxDataHorizonPackets", t["kTxDataHorizonPackets"], "packets",
         f"{_ms(t['kTxDataHorizonPackets'])} content horizon")
    _row("kTxExposureLeadFrames", t["kTxExposureLeadFrames"], "frames",
         f"{t['kTxExposureLeadFrames'] / 48:.1f} ms @48k")
    _row("kTxExposureLeadPackets", t["kTxExposureLeadPackets"], "packets")
    _row("kTxPreparationSlackPackets", t["kTxPreparationSlackPackets"], "packets")
    _row("kTxCoverageLeadPackets", t["kTxCoverageLeadPackets"], "packets",
         f"{_ms(t['kTxCoverageLeadPackets'])} refill safety")
    _row("kTxFrameExposureWindowPackets", t["kTxFrameExposureWindowPackets"], "packets",
         f"{_ms(t['kTxFrameExposureWindowPackets'])} content exposure")
    _row("kTxPreparationLeadPackets", t["kTxPreparationLeadPackets"], "packets",
         f"{_ms(t['kTxPreparationLeadPackets'])} = coverage + exposure")
    _row("kTxMaxCoveredDeltaConsumedPackets", t["kTxMaxCoveredDeltaConsumedPackets"],
         "packets", "largest absorbable coalesced refill")
    _row("kTxSharedSlotPackets", t["kTxSharedSlotPackets"], "packets",
         f"{_ms(t['kTxSharedSlotPackets'])} backing ring")
    _row("kTimelineSlots", t["kTimelineSlots"], "packets", "timeline slot array")

    _rule("RX replay ring (RxSequenceReplay.hpp)")
    _row("kCapacity", r["kCapacity"], "entries", f"{_ms(r['kCapacity'])} of RX history")
    _row("kReadDelay", r["kReadDelay"], "entries",
         f"{_ms(r['kReadDelay'])} -- PRODUCER-STALL RECOVERY BUDGET")

    _rule("Derived budgets (not written down in any header)")
    lead = t["kTxPreparationLeadPackets"]
    read_delay = r["kReadDelay"]
    horizon = t["kTxDataHorizonPackets"]
    _row("stall tolerance (law)", f"{(read_delay + horizon) / 8:.1f}", "ms",
         "(kReadDelay + horizon) / 8 -- see FINDINGS.md F3")
    _row("replay headroom", read_delay - lead, "packets",
         "kReadDelay - lead; NOT the failure mechanism (F1)")
    _row("prefill before IT arm", t["kTxSharedSlotPackets"], "packets",
         f"{_ms(t['kTxSharedSlotPackets'])} of NODATA at StartIO")
    _row("first reusable slot", t["kTxSharedSlotPackets"], "packets",
         f"{_ms(t['kTxSharedSlotPackets'])} after IT start")

    _rule("Per-rate view")
    print(f"  {'rate':>8} {'frames/pkt':>11} {'DATA share':>11} {'horizon':>10} {'io period':>11}")
    for rate in SUPPORTED_RATES:
        g = Geometry.from_headers(rate, h)
        print(
            f"  {rate:>8} {g.frames_per_data_packet:>11} "
            f"{g.data_packet_fraction:>11.4f} "
            f"{g.data_horizon_frames:>7} fr {t['kHalIoPeriodFrames'] * 1000 / rate:>8.2f} ms"
        )

    _rule("CoreAudio buffer-size range")
    print(
        "  The HAL derives kAudioDevicePropertyBufferFrameSizeRange from the stream\n"
        "  IOMemoryDescriptor; AudioDriverKit exposes no explicit range setter. The\n"
        "  binding input is the compile-time profile below."
    )
    _row("ring", t["kFrameRingFrames"], "frames")
    _row("client IO budget", t["kHalIoPeriodFrames"], "frames")
    print("\n  Run 'asfw-sim plan-io --frames N' to cost a larger buffer size.")
    print()
    return 0


# --- other commands -----------------------------------------------------------


def cmd_plan_io(args) -> int:
    h = load_driver_headers()
    current = derive(h, io_budget_frames=h.timing["kHalIoPeriodFrames"])
    target = solve_for_io_budget(h, args.frames, ring_multiple=args.ring_multiple)

    print(f"\033[1mIO budget {current.io_budget_frames} -> {args.frames} frames\033[0m\n")
    print(f"  {'constant':<34} {'current':>10} {'proposed':>10}")
    print("  " + "-" * 56)
    for label, a, b in [
        ("kHalIoPeriodFrames", current.io_budget_frames, target.io_budget_frames),
        ("kFrameRingFrames", current.frame_ring_frames, target.frame_ring_frames),
        ("kHalZeroTimestampPeriodFrames", current.zts_period_frames, target.zts_period_frames),
        ("kTxDataHorizonPackets", h.timing["kTxDataHorizonPackets"],
         (target.exposure_lead_frames * 8000) // h.timing["kSampleRateHz"]),
        ("kTxExposureLeadFrames", current.exposure_lead_frames, target.exposure_lead_frames),
        ("kTxFrameExposureWindowPackets", current.frame_exposure_window_packets,
         target.frame_exposure_window_packets),
        ("kTxPreparationLeadPackets", current.preparation_lead_packets,
         target.preparation_lead_packets),
        ("kTxSharedSlotPackets", current.shared_slot_packets, target.shared_slot_packets),
        ("kTimelineSlots", current.timeline_slots, target.timeline_slots),
    ]:
        mark = " " if a == b else "*"
        print(f"  {mark}{label:<33} {a:>10} {b:>10}")

    print(f"\n  all header static_asserts hold: {'YES' if target.ok else 'NO'}")
    for failure in target.failures:
        print(f"    FAIL {failure.name}: {failure.detail}")

    shared_bytes = target.shared_slot_packets * (8 + 8 * 64 * 4)
    print(
        f"\n  cost: shared TX slab ~{shared_bytes / 1024:.0f} KiB/stream "
        f"(was ~{current.shared_slot_packets * (8 + 8 * 64 * 4) / 1024:.0f} KiB), "
        f"\n        prefill before IT arm {target.shared_slot_packets / 8:.1f} ms "
        f"(was {current.shared_slot_packets / 8:.1f} ms)"
    )
    print(
        "\n  NOTE: prefill latency scales with kTxSharedSlotPackets because StartIO\n"
        "  validates committedEnd == numSlots before IT arm "
        "(ASFWAudioDevice.cpp:341-345).\n"
    )
    return 0


def cmd_scenario(args) -> int:
    paths = [Path(p) for p in args.paths] or [_default_scenario_dir()]
    scenarios: list[Scenario] = []
    for path in paths:
        if path.is_dir():
            scenarios.extend(load_scenario_dir(path))
        else:
            scenarios.append(load_scenario(path))

    headers = load_driver_headers()
    failures = 0
    for scenario in scenarios:
        print(f"\n\033[1m{scenario.name}\033[0m")
        if scenario.description and not args.quiet:
            for line in scenario.description.strip().splitlines():
                print(f"  \033[2m{line.strip()}\033[0m")

        for outcome in run_scenario(scenario, headers):
            r = outcome.result
            status = (
                "\033[32mPASS\033[0m"
                if outcome.ok
                else "\033[31mFAIL\033[0m"
            )
            verdict = "COLLAPSED" if r.collapsed else "healthy"
            detail = (
                " ".join(f"{k}={v}" for k, v in sorted(outcome.point.items()))
                or "-"
            )
            marker = status if (scenario.expect or scenario.require_valid) else "    "
            print(
                f"  {marker} {detail:<52} written={r.written_fraction * 100:5.1f}% "
                f"data={r.data_packet_fraction:.3f} align={r.align_count} "
                f"{verdict}"
            )
            for warning in outcome.warnings:
                print(f"         \033[33mwarn\033[0m {warning}")
            for failure in outcome.failures:
                print(f"         \033[31m{failure}\033[0m")
            if not outcome.ok:
                failures += 1

    print()
    if failures:
        print(f"\033[31m{failures} scenario point(s) failed\033[0m")
    return 1 if failures else 0


def cmd_fingerprint(args) -> int:
    g = Geometry.from_headers(args.rate)
    print("Simulated failure fingerprints. The discriminator is the DEFICIT SLOPE,")
    print("not any counter: `ahead` appears in healthy runs too.\n")
    print(
        f"  {'cause':<22}{'written':>9}{'ahead':>8}{'reclamp':>9}"
        f"{'align':>7}{'W-E':>9}{'slope/s':>9}"
    )
    print("  " + "-" * 73)
    for f in fingerprints(g):
        print(
            f"  {f.cause:<22}{f.written_fraction * 100:>8.1f}%{f.ahead:>8}"
            f"{f.reclamped:>9}{f.align_count:>7}{f.deficit_frames:>9}"
            f"{f.deficit_slope_per_s:>9.0f}"
        )
        print(f"  {'':<22}\033[2m{f.note}\033[0m")
    print(
        f"\n  calibration: E forfeits {frames_lost_per_drop(g):.2f} frames per lost "
        f"RX observation\n  exposure horizon: {g.data_horizon_frames} frames "
        f"(~{g.data_horizon_frames / max(frames_lost_per_drop(g), 1e-9):.0f} lost "
        "observations, for the life of the stream)\n"
    )
    return 0


def cmd_diagnose(args) -> int:
    g = Geometry.from_headers(args.rate)
    d = diagnose(
        deficit_start_frames=args.deficit_start,
        deficit_end_frames=args.deficit_end,
        elapsed_s=args.elapsed_s,
        geometry=g,
        ahead=args.ahead,
        align_count=args.align,
    )
    print(f"\n  \033[1m{d.cause}\033[0m  (confidence: {d.confidence})\n")
    for line in _wrap(d.reasoning, 72):
        print(f"  {line}")
    if d.estimated_rx_loss_per_s is not None:
        print(
            f"\n  estimated RX loss: {d.estimated_rx_loss_per_s:.1f} packets/s "
            f"({d.estimated_rx_loss_percent:.3f}% of 8000/s)"
        )
    print()
    return 0


def _wrap(text: str, width: int) -> list[str]:
    words, lines, cur = text.split(), [], ""
    for w in words:
        if len(cur) + len(w) + 1 > width:
            lines.append(cur)
            cur = w
        else:
            cur = f"{cur} {w}".strip()
    if cur:
        lines.append(cur)
    return lines


def cmd_capture(args) -> int:
    if args.action == "import":
        payloads = []
        for raw in args.paths:
            for path in sorted(Path(raw).glob("*.json")) if Path(raw).is_dir() else [Path(raw)]:
                payloads.append(json.loads(path.read_text(encoding="utf-8")))
        cap = capture_from_responses(
            payloads, device=args.device, sample_rate=args.rate, notes=args.notes
        )
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        cap.save(out)
        print(f"wrote {out}  ({len(cap.records)} records)\n")
        print(cap.summary())
        return 0

    cap = load_capture(args.paths[0])
    print(cap.summary())
    if args.action == "diagnose":
        slope = cap.deficit_slope_per_s()
        pts = cap.cursor_series()
        if slope is None or len(pts) < 2:
            print("\n  not enough cursor points to classify")
            return 1
        span = pts[-1][0] - pts[0][0]
        d = diagnose(
            deficit_start_frames=pts[0][1] - pts[0][2],
            deficit_end_frames=pts[-1][1] - pts[-1][2],
            elapsed_s=span,
            geometry=cap.geometry_object(),
        )
        print(f"\n  \033[1m{d.cause}\033[0m  (confidence: {d.confidence})")
        for line in _wrap(d.reasoning, 72):
            print(f"  {line}")
        if d.estimated_rx_loss_per_s is not None:
            print(
                f"\n  estimated RX loss: {d.estimated_rx_loss_per_s:.1f} packets/s "
                f"({d.estimated_rx_loss_percent:.3f}%)"
            )
        if span < 300:
            print(
                f"\n  \033[33mwarning\033[0m: {span:.0f} s window. A ramp needs "
                "minutes to separate from a step (FINDINGS F8); prefer >= 300 s."
            )
    return 0


def cmd_run(args) -> int:
    print(_run(_geometry(args), args.seconds, args.stall_ms).summary())
    return 0


def cmd_cliff(args) -> int:
    g = _geometry(args)
    cycles = _cliff(g, args.seconds)
    predicted = (g.replay_read_delay + g.tx_data_horizon_packets) / 8
    print(
        f"rate={g.sample_rate} readDelay={g.replay_read_delay} "
        f"horizon={g.tx_data_horizon_packets}\n"
        f"  survivable producer stall: {cycles / 8:.1f} ms ({cycles} cycles)\n"
        f"  kReadDelay+horizon law predicts: {predicted:.1f} ms"
    )
    return 0


def cmd_sweep(args) -> int:
    base = Geometry.from_headers(args.rate)
    print(f"  {'readDelay':>9} {'horizon':>8} {'cliff':>9} {'law':>9}")
    for read_delay, horizon in [(256, 400), (512, 400), (1024, 400), (256, 160), (1024, 800)]:
        g = base.evolve(
            replay_read_delay=read_delay,
            replay_capacity=max(512, 2 * read_delay),
            tx_data_horizon_packets=horizon,
        )
        print(
            f"  {read_delay:>9} {horizon:>8} {_cliff(g, args.seconds) / 8:>8.1f}ms "
            f"{(read_delay + horizon) / 8:>8.1f}ms"
        )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="asfw-sim", description=__doc__)
    parser.add_argument("--rate", type=int, default=48_000, choices=SUPPORTED_RATES)
    parser.add_argument("--seconds", type=int, default=15)
    parser.add_argument("--read-delay", type=int, default=0)
    parser.add_argument("--capacity", type=int, default=0)
    parser.add_argument("--horizon", type=int, default=0)
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("geometry", help="print the live driver geometry (default)")
    plan = sub.add_parser("plan-io", help="cost a larger CoreAudio IO buffer")
    plan.add_argument("--frames", type=int, required=True)
    plan.add_argument("--ring-multiple", type=int, default=4)
    run_p = sub.add_parser("run", help="run one scenario")
    run_p.add_argument("--stall-ms", type=float, default=0.0)
    sub.add_parser("cliff", help="bisect the stall-tolerance cliff")
    sub.add_parser("sweep", help="compare readDelay/horizon variants")
    scen = sub.add_parser(
        "scenario", help="run YAML hypothesis files (default: shipped scenarios/)"
    )
    scen.add_argument("paths", nargs="*", help="scenario .yaml files or directories")
    scen.add_argument("--quiet", action="store_true", help="omit descriptions")
    sub.add_parser(
        "fingerprint", help="counter signature of each simulated failure cause"
    )
    cap = sub.add_parser("capture", help="import / inspect MCP ring captures")
    cap.add_argument("action", choices=["import", "summary", "diagnose"])
    cap.add_argument("paths", nargs="+", help="raw MCP json files/dir, or a capture")
    cap.add_argument("-o", "--out", default="capture.json")
    cap.add_argument("--device", default="unknown")
    cap.add_argument("--notes", default="")
    diag = sub.add_parser(
        "diagnose", help="classify a real run from two [PayloadWriter] deficits"
    )
    diag.add_argument("--deficit-start", type=int, required=True,
                      help="W-E in frames at the first sample (negative = healthy)")
    diag.add_argument("--deficit-end", type=int, required=True)
    diag.add_argument("--elapsed-s", type=float, required=True)
    diag.add_argument("--ahead", type=int, default=None,
                      help="cumulative [TxReplay] fail=ahead count")
    diag.add_argument("--align", type=int, default=None,
                      help="cumulative [TxAlign] count")

    args = parser.parse_args(argv)
    handler = {
        None: cmd_geometry,
        "geometry": cmd_geometry,
        "plan-io": cmd_plan_io,
        "run": cmd_run,
        "cliff": cmd_cliff,
        "sweep": cmd_sweep,
        "scenario": cmd_scenario,
        "fingerprint": cmd_fingerprint,
        "capture": cmd_capture,
        "diagnose": cmd_diagnose,
    }[args.command]
    return handler(args)


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
