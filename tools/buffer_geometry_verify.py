#!/usr/bin/env python3
"""
Buffer-geometry invariant verifier (FW-53 / safety-offset + ring-bump work).

PURPOSE
-------
Before we enlarge the shared audio ring (to advertise a larger CoreAudio buffer
size than the current 576-frame ceiling) or change the HAL safety offsets, the
proposed numbers must satisfy EVERY compile-time invariant baked into
ASFWDriver/Shared/Isoch/AudioTimingGeometry.hpp as a static_assert. A bump that
"looks fine" routinely breaks one of:

  * cadence alignment   (kTxSharedSlotPackets % {4,6} == 0)
  * timeline fit        (kTxSharedSlotPackets <= kTimelineSlots)
  * ring divisibility   (ring % ioBudget, ring % zts, ring % 32)
  * exposure coverage   (frame-exposure window covers WriteEnd + cushion)

This tool re-derives the whole geometry the SAME way the header does, runs each
invariant as a named check, and:

  1. confirms the CURRENT shipping profile is all-green (regression baseline),
  2. evaluates a CANDIDATE profile for a target max CoreAudio IO size,
  3. `solve` finds the smallest cadence-aligned {timeline, ring} for a target IO.

It does NOT touch any source. Run it, read the table, THEN edit the header.

The exposure cushion is the Defect-B guard: the producer's exposure frontier E
must lead CoreAudio's write frontier W by one full IO window + scheduling
jitter. If CoreAudio is allowed to pick an IO size larger than the budget the
cushion was built on, W jumps past E => silence. So the advertised max IO must
equal the io-budget the cushion is sized for: bumping the buffer ceiling and
sizing the cushion are the SAME change, which is the whole point of verifying
them together.

Usage:
    python3 buffer_geometry_verify.py verify                 # current + 1024 candidate
    python3 buffer_geometry_verify.py verify --max-io 1024
    python3 buffer_geometry_verify.py solve --max-io 1024     # smallest aligned profile
    python3 buffer_geometry_verify.py safety                 # old vs new safety offset
"""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass, replace
from typing import List, Tuple

# -----------------------------------------------------------------------------
# Fixed constants shared by every profile (AudioTimingGeometry.hpp).
# -----------------------------------------------------------------------------
CADENCE_BLOCK_PKTS = 4          # kCadenceBlockPackets
CADENCE_BLOCK_FRAMES = 24       # kCadenceBlockFrames  (=> 6 frames/pkt average)
FRAME_ALIGNMENT = 32            # kFrameAlignment
PKTS_PER_GROUP = 6              # kTxPacketsPerGroup / kTimingGroupPackets
HW_RING_PKTS = 48               # kTxHardwareRingPackets
JITTER_FRAMES = 64              # kSchedulingJitterFrames
MAX_FRAMES_PER_INTERRUPT = 40   # kMaximumNominalFramesPerInterrupt
MIN_DISPATCH_GROUPS = 16        # coverage target: 16 six-packet groups (~12 ms)


@dataclass(frozen=True)
class Geometry:
    """One candidate HAL buffer profile + the derived TX packet geometry.

    io_budget_frames == kHalIoPeriodFrames is BOTH the advertised CoreAudio max
    IO size and the basis of the exposure cushion (see module docstring).
    """
    name: str
    ring_frames: int            # frameRingFrames  (shared IOMemoryDescriptor)
    io_budget_frames: int       # kHalIoPeriodFrames == advertised max IO
    zts_frames: int             # kHalZeroTimestampPeriodFrames
    timeline_slots: int         # kTimelineSlots (AmdtpPacketTimeline length)

    # --- derived: exposure cushion (frames) ---------------------------------
    @property
    def exposure_lead_frames(self) -> int:          # kTxExposureLeadFrames
        return self.io_budget_frames + JITTER_FRAMES

    @property
    def exposure_lead_pkts(self) -> int:            # kTxExposureLeadPackets
        # ceil(frames * blockPkts / blockFrames) == ceil(frames / 6), THEN round
        # up to a whole timing group so the downstream sharedSlot stays a
        # multiple of both 6 (group) and 4 (cadence block). The shipping header
        # omits this round-up and is only accidentally aligned at ioBudget=512
        # (576/6 == 96 exactly); any other budget needs it. This is part of the
        # proposed header change.
        raw = (self.exposure_lead_frames * CADENCE_BLOCK_PKTS +
               CADENCE_BLOCK_FRAMES - 1) // CADENCE_BLOCK_FRAMES
        return ((raw + PKTS_PER_GROUP - 1) // PKTS_PER_GROUP) * PKTS_PER_GROUP

    # --- derived: packet-domain TX ownership --------------------------------
    @property
    def slack_pkts(self) -> int:                    # kTxPreparationSlackPackets
        return 2 * HW_RING_PKTS

    @property
    def coverage_lead_pkts(self) -> int:            # kTxCoverageLeadPackets
        return HW_RING_PKTS + self.slack_pkts

    @property
    def frame_exposure_window_pkts(self) -> int:    # kTxFrameExposureWindowPackets
        return 2 * self.exposure_lead_pkts

    @property
    def preparation_lead_pkts(self) -> int:         # kTxPreparationLeadPackets
        return self.coverage_lead_pkts + self.frame_exposure_window_pkts

    @property
    def shared_slot_pkts(self) -> int:              # kTxSharedSlotPackets
        return self.preparation_lead_pkts + HW_RING_PKTS

    @property
    def max_covered_delta_pkts(self) -> int:        # kTxMaxCoveredDeltaConsumedPackets
        return self.preparation_lead_pkts - HW_RING_PKTS


# Invariant := (name, predicate(g) -> bool, detail(g) -> str). Names mirror the
# static_assert messages in AudioTimingGeometry.hpp so a failure points straight
# at the line that would not compile.
INVARIANTS = [
    ("ring % ioBudget == 0",
     lambda g: g.ring_frames % g.io_budget_frames == 0,
     lambda g: f"{g.ring_frames} % {g.io_budget_frames} = {g.ring_frames % g.io_budget_frames}"),
    ("ring % zts == 0",
     lambda g: g.ring_frames % g.zts_frames == 0,
     lambda g: f"{g.ring_frames} % {g.zts_frames} = {g.ring_frames % g.zts_frames}"),
    ("ring % 32 alignment",
     lambda g: g.ring_frames % FRAME_ALIGNMENT == 0,
     lambda g: f"{g.ring_frames} % {FRAME_ALIGNMENT} = {g.ring_frames % FRAME_ALIGNMENT}"),
    ("ring >= ioBudget (holds one max IO)",
     lambda g: g.ring_frames >= g.io_budget_frames,
     lambda g: f"{g.ring_frames} >= {g.io_budget_frames}"),
    ("sharedSlot >= preparationLead",
     lambda g: g.shared_slot_pkts >= g.preparation_lead_pkts,
     lambda g: f"{g.shared_slot_pkts} >= {g.preparation_lead_pkts}"),
    ("maxCoveredDelta >= 16 groups (~12 ms)",
     lambda g: g.max_covered_delta_pkts >= MIN_DISPATCH_GROUPS * PKTS_PER_GROUP,
     lambda g: f"{g.max_covered_delta_pkts} >= {MIN_DISPATCH_GROUPS * PKTS_PER_GROUP}"),
    ("coverageLead == hwRing + slack",
     lambda g: g.coverage_lead_pkts == HW_RING_PKTS + g.slack_pkts,
     lambda g: f"{g.coverage_lead_pkts} == {HW_RING_PKTS + g.slack_pkts}"),
    ("preparationLead <= sharedSlot - hwRing",
     lambda g: g.preparation_lead_pkts <= g.shared_slot_pkts - HW_RING_PKTS,
     lambda g: f"{g.preparation_lead_pkts} <= {g.shared_slot_pkts - HW_RING_PKTS}"),
    ("sharedSlot % packetsPerGroup(6) == 0",
     lambda g: g.shared_slot_pkts % PKTS_PER_GROUP == 0,
     lambda g: f"{g.shared_slot_pkts} % {PKTS_PER_GROUP} = {g.shared_slot_pkts % PKTS_PER_GROUP}"),
    ("sharedSlot % cadenceBlock(4) == 0",
     lambda g: g.shared_slot_pkts % CADENCE_BLOCK_PKTS == 0,
     lambda g: f"{g.shared_slot_pkts} % {CADENCE_BLOCK_PKTS} = {g.shared_slot_pkts % CADENCE_BLOCK_PKTS}"),
    ("exposureLead >= ioBudget + jitter",
     lambda g: g.exposure_lead_frames >= g.io_budget_frames + JITTER_FRAMES,
     lambda g: f"{g.exposure_lead_frames} >= {g.io_budget_frames + JITTER_FRAMES}"),
    ("exposureLead < ring",
     lambda g: g.exposure_lead_frames < g.ring_frames,
     lambda g: f"{g.exposure_lead_frames} < {g.ring_frames}"),
    ("exposureLeadPkts <= sharedSlot",
     lambda g: g.exposure_lead_pkts <= g.shared_slot_pkts,
     lambda g: f"{g.exposure_lead_pkts} <= {g.shared_slot_pkts}"),
    ("frameExposureWindow covers WriteEnd + cushion",
     lambda g: g.frame_exposure_window_pkts * CADENCE_BLOCK_FRAMES >=
               (g.io_budget_frames + g.exposure_lead_frames) * CADENCE_BLOCK_PKTS,
     lambda g: f"{g.frame_exposure_window_pkts * CADENCE_BLOCK_FRAMES} >= "
               f"{(g.io_budget_frames + g.exposure_lead_frames) * CADENCE_BLOCK_PKTS}"),
    ("sharedSlot <= timelineSlots",
     lambda g: g.shared_slot_pkts <= g.timeline_slots,
     lambda g: f"{g.shared_slot_pkts} <= {g.timeline_slots}"),
]


def check(g: Geometry) -> List[Tuple[str, bool, str]]:
    return [(name, pred(g), detail(g)) for name, pred, detail in INVARIANTS]


def derived_table(g: Geometry) -> str:
    return (f"    ring={g.ring_frames}fr  ioBudget/maxIO={g.io_budget_frames}fr  "
            f"zts={g.zts_frames}fr  timeline={g.timeline_slots}slots\n"
            f"    exposureLead={g.exposure_lead_frames}fr ({g.exposure_lead_pkts}pkt)  "
            f"frameWindow={g.frame_exposure_window_pkts}pkt  "
            f"prepLead={g.preparation_lead_pkts}pkt  "
            f"sharedSlot={g.shared_slot_pkts}pkt  "
            f"maxDelta={g.max_covered_delta_pkts}pkt")


def report(g: Geometry) -> bool:
    print(f"[{g.name}]")
    print(derived_table(g))
    rows = check(g)
    all_ok = all(ok for _, ok, _ in rows)
    for name, ok, detail in rows:
        mark = "ok " if ok else "FAIL"
        flag = "" if ok else "   <<<"
        print(f"      [{mark}] {name:48} {detail}{flag}")
    print(f"    => {'ALL INVARIANTS HOLD' if all_ok else 'BROKEN — do not adopt'}\n")
    return all_ok


# -----------------------------------------------------------------------------
# Reference profiles
# -----------------------------------------------------------------------------
# Current shipping geometry (dice-working-1536 + AudioTimingGeometry.hpp).
CURRENT = Geometry(
    name="CURRENT (dice-working-1536)",
    ring_frames=1536,
    io_budget_frames=512,
    zts_frames=1536,
    timeline_slots=512,
)


def solve(max_io: int, zts: int = 1536) -> Geometry:
    """Smallest cadence-aligned profile that advertises `max_io` frames.

    Strategy: io_budget = max_io (cushion basis). Round the derived shared-slot
    packet count up to a multiple of lcm(6,4)=12 by nudging the timeline; size
    the ring to the least common multiple structure that holds maxIO + cushion.
    We do not mutate the header derivation — we just pick a timeline_slots and
    ring_frames large enough that every invariant passes.
    """
    g = Geometry(name=f"CANDIDATE (max IO {max_io})",
                 ring_frames=zts, io_budget_frames=max_io,
                 zts_frames=zts, timeline_slots=512)

    # Timeline must hold the derived shared-slot count (which is already a
    # multiple of 12 because frameExposureWindow = 2*ceil(.) and the +192 base
    # is %12==0 only when exposure_lead_pkts is %6==0). If the derived
    # shared_slot is not %12, the header's own asserts would fail, so we round
    # the timeline up to the next multiple of the group size and report the
    # shared-slot alignment separately.
    timeline = ((g.shared_slot_pkts + PKTS_PER_GROUP - 1) // PKTS_PER_GROUP) * PKTS_PER_GROUP
    timeline = max(timeline, 512)
    g = replace(g, timeline_slots=timeline)

    # Ring: multiple of both io_budget and zts, strictly greater than the
    # exposure lead, and >= maxIO. lcm(maxIO, zts) stepped until > exposureLead.
    step = math.lcm(max_io, zts)
    ring = step
    while ring <= g.exposure_lead_frames or ring < max_io:
        ring += step
    ring = max(ring, zts)
    while ring % FRAME_ALIGNMENT != 0:
        ring += step

    return replace(g, ring_frames=ring, timeline_slots=timeline)


# -----------------------------------------------------------------------------
# Safety offset (RequiredInputSafetyFrames) — old vs proposed
# -----------------------------------------------------------------------------
def required_input_safety_old(profile_in, out_safety, max_io, max_irq, jitter):
    full_window = out_safety + max_io + jitter
    interrupt_batch = max_irq + jitter
    hi = max(full_window, interrupt_batch)
    return max(profile_in, hi), full_window, interrupt_batch


def required_input_safety_new(profile_in, out_safety, max_io, max_irq, jitter):
    # Drop the full_window term: CoreAudio already accounts for the IO buffer
    # size separately. The safety offset is only the data-visibility margin =
    # one interrupt batch + scheduling jitter, aligned up to 32.
    interrupt_batch = max_irq + jitter
    raw = max(profile_in, interrupt_batch)
    aligned = ((raw + FRAME_ALIGNMENT - 1) // FRAME_ALIGNMENT) * FRAME_ALIGNMENT
    return aligned, interrupt_batch


def cmd_safety(_args) -> None:
    print("Safety offset: current (fullWindow) vs proposed (interruptBatch)\n")
    print("  Reference values (~50fr @48k, in==out, x2/x4 rate): "
          "Apogee Duet plist, Saffire RE, AppleFWAudio decomp.\n")
    out_safety = 48
    print(f"  {'rate':>6} {'maxIO':>6} {'out':>5} {'in(old=624 model)':>18} "
          f"{'in(new)':>9}  note")
    for rate, max_io, max_irq in ((48000, 512, 40), (96000, 1024, 80), (192000, 2048, 160)):
        old, full, batch = required_input_safety_old(48, out_safety, max_io, max_irq, JITTER_FRAMES)
        new, _ = required_input_safety_new(48, out_safety, max_io, max_irq, JITTER_FRAMES)
        print(f"  {rate:>6} {max_io:>6} {out_safety:>5} {old:>18} {new:>9}  "
              f"fullWindow={full} batch={batch}")
    print("\n  @48k: 624 -> 128  (output stays 48; ~10.7 ms phantom input latency removed)")


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------
def cmd_verify(args) -> None:
    print("=" * 78)
    print("BUFFER GEOMETRY INVARIANT CHECK")
    print("=" * 78)
    base_ok = report(CURRENT)
    if not base_ok:
        print("!! CURRENT profile failed — the model does not match the header. "
              "Fix the model before trusting candidates.\n")
    cand = solve(args.max_io, zts=args.zts)
    cand_ok = report(cand)
    print("-" * 78)
    print(f"baseline (current) : {'PASS' if base_ok else 'FAIL'}")
    print(f"candidate (maxIO {args.max_io}) : {'PASS' if cand_ok else 'FAIL'}")
    if cand_ok:
        print(f"\nProposed header changes for maxIO={args.max_io}:")
        print(f"  HAL profile          frameRingFrames = {cand.ring_frames}  "
              f"clientIoBudgetFrames = {cand.io_budget_frames}  "
              f"zeroTimestampPeriodFrames = {cand.zts_frames}")
        print(f"  AudioTimingGeometry  kTimelineSlots = {cand.timeline_slots}")
        print(f"  (exposure lead, packet windows, shared-slot ring all derive "
              f"automatically and stay green)")


def cmd_solve(args) -> None:
    cand = solve(args.max_io, zts=args.zts)
    report(cand)


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp_v = sub.add_parser("verify", help="check current + candidate")
    sp_v.add_argument("--max-io", type=int, default=1024)
    sp_v.add_argument("--zts", type=int, default=1536)

    sp_s = sub.add_parser("solve", help="smallest aligned profile for a target IO")
    sp_s.add_argument("--max-io", type=int, default=1024)
    sp_s.add_argument("--zts", type=int, default=1536)

    sub.add_parser("safety", help="old vs new safety offset table")

    args = p.parse_args()
    if args.cmd == "verify":
        cmd_verify(args)
    elif args.cmd == "solve":
        cmd_solve(args)
    elif args.cmd == "safety":
        cmd_safety(args)


if __name__ == "__main__":
    main()
