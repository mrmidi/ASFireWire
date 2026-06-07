#!/usr/bin/env python3
"""
ASFireWire TX packet-ring / two-clock coverage SIMULATOR.

Companion to asfw_tx_math_sim.py:
  - asfw_tx_math_sim.py  -> parses real kernel logs, tunes offset from captured data.
  - asfw_tx_ring_sim.py  -> forward-simulates the pipeline from first principles, so you
                            can reproduce the bug and prove a fix WITHOUT hardware or logs.

WHAT IT MODELS
--------------
The host->device TX path has two independent clocks:

  * write head  (wr)    -- CoreAudio HAL clock. Producer. Writes PCM into a circular
                           output ring of `capacity` frames and stamps each completed
                           packet slot with [begin, end) + a generation number.
  * timeline head (sched) -- wire / IT-DMA clock. Consumer. Per packet it computes
                           audioFirst = sched - offset and reads the slot that frame
                           maps to.

Once rate-locked the two heads sit a fixed `gap = sched - wr` apart (observed 4436..4856
on real hardware). The consumer must subtract that gap back off via `offset` so audioFirst
lands BEHIND the write head, inside an already-written slot.

THE BUG (offset too small):
  audioFirst = sched - offset  lands AHEAD of wr (in the future). CoreAudio has not
  written that frame yet, so the ring slot it maps to still holds the PREVIOUS 4096-frame
  lap's data -> generation mismatch -> coverage check fails -> silence is emitted.

This sim reproduces exactly that, using the same modular slot/generation arithmetic as
ASFWDriver/AudioEngine/DirectIsoch/IsochAudioTxPipeline.hpp (RelativeFrame / SlotForFrame /
GenerationForFrame) and the coverage check in IsochAudioTxPipeline.cpp:1360.

USAGE
-----
  # Reproduce the real bug (gap 4840, the old offset 3072) and prove the 5120 fix:
  python3 tools/debug/asfw_tx_ring_sim.py --gap 4840 --compare 3072,5120

  # Single offset, show a few example packets (covered + uncovered log-style lines):
  python3 tools/debug/asfw_tx_ring_sim.py --gap 4840 --offset 3072 --show 4

  # Sweep offsets to find the safe window and the smallest safe value:
  python3 tools/debug/asfw_tx_ring_sim.py --gap 4840 --sweep 0:8192:8 --target-lead 128

  # Show run-to-run fragility: a fixed offset that is safe at one gap can fail at another
  # (sched-wr was observed at 4436..4856 across real captures). Just vary --gap:
  python3 tools/debug/asfw_tx_ring_sim.py --gap 4460 --offset 5120     # OK, big lead
  python3 tools/debug/asfw_tx_ring_sim.py --gap 5200 --offset 5120     # BROKEN, future
"""

from __future__ import annotations

import argparse
import dataclasses
import statistics
from typing import Optional


@dataclasses.dataclass
class Ring:
    """Mirror of TxPcmPacketRing geometry + the three pure index helpers (see .hpp)."""
    capacity: int          # outputFrameCapacity (frames), e.g. 4096
    fpp: int               # framesPerPacket, e.g. 8
    base_frame: int = 0    # baseFrame (audio base the timeline was seeded from)

    @property
    def packet_slots(self) -> int:
        return self.capacity // self.fpp

    # RelativeFrame: (absoluteFrame - baseFrame) mod capacity   (.hpp:284)
    def relative_frame(self, frame: int) -> int:
        return (frame - self.base_frame) % self.capacity

    # SlotForFrame: (RelativeFrame / fpp) mod packetSlots       (.hpp:291)
    def slot_for_frame(self, frame: int) -> int:
        return (self.relative_frame(frame) // self.fpp) % self.packet_slots

    # GenerationForFrame: ((absoluteFrame - baseFrame) / capacity) + 1   (.hpp:305)
    def generation_for_frame(self, frame: int) -> int:
        if frame < self.base_frame:
            return 0
        return ((frame - self.base_frame) // self.capacity) + 1


@dataclasses.dataclass
class Stamp:
    begin: int
    end: int
    generation: int


@dataclasses.dataclass
class Result:
    offset: int
    total: int = 0
    covered: int = 0
    future: int = 0          # audioFirst past the write head (not yet produced)
    overwritten: int = 0     # audioFirst older than oldestValid (lapped by producer)
    gen_mismatch: int = 0    # right slot, wrong lap (the headline symptom)
    leads: list[int] = dataclasses.field(default_factory=list)
    samples: list[str] = dataclasses.field(default_factory=list)

    @property
    def covered_pct(self) -> float:
        return 0.0 if not self.total else 100.0 * self.covered / self.total


def simulate(
    *,
    ring: Ring,
    offset: int,
    gap: int,
    packets: int,
    target_lead: int,
    warmup_laps: int = 2,
    capture_samples: int = 0,
) -> Result:
    """
    Two-clock simulation. Each step = one isoch packet (~125 us).

    Faithful clock model:
      * sched (timeline head) is the PRIMARY, packet-aligned clock: it advances by exactly
        fpp per packet, as the IT-DMA does.
      * wr (write head) is DERIVED: wr = sched - gap. The HAL block size is not a multiple
        of fpp, so wr can sit at any sub-packet phase -- exactly as on hardware, where
        writtenEnd was observed at e.g. 673060 (not 8-aligned).
      * the producer only STAMPS whole packets, so slot stamps land on the fpp grid up to
        the write frontier even though wr itself is unaligned.
      * offset is aligned the way the driver does it: (offset / fpp) * fpp.

    This keeps audioFirst = sched - offset packet-aligned (so it coincides with a stamp
    boundary), while wr's sub-packet phase only affects the future/lead boundary -- matching
    PreparePayload() in IsochAudioTxPipeline.cpp.
    """
    fpp = ring.fpp
    cap = ring.capacity
    offset_eff = (offset // fpp) * fpp
    res = Result(offset=offset_eff)
    stamps: dict[int, Stamp] = {}

    # Timeline head starts a couple of laps in, so the ring has been running a while.
    sched = ring.base_frame + warmup_laps * cap            # packet-aligned
    # Write frontier: exclusive end of the last fully-written (stamped) packet.
    frontier = max(ring.base_frame, ((sched - gap) // fpp) * fpp)

    # Pre-stamp the lap currently occupying the ring (producer history).
    f = max(ring.base_frame, frontier - cap)
    while f < frontier:
        stamps[ring.slot_for_frame(f)] = Stamp(f, f + fpp, ring.generation_for_frame(f))
        f += fpp

    for _ in range(packets):
        wr = sched - gap                                    # may be sub-packet
        written_end = wr
        oldest_valid = wr - cap

        # Producer stamps every newly completed packet up to the write head.
        while frontier + fpp <= wr:
            stamps[ring.slot_for_frame(frontier)] = Stamp(
                frontier, frontier + fpp, ring.generation_for_frame(frontier))
            frontier += fpp

        # Consumer: read the slot audioFirst maps to and check coverage.
        audio_first = sched - offset_eff                   # packet-aligned
        audio_end = audio_first + fpp
        c_slot = ring.slot_for_frame(audio_first)
        expected_gen = ring.generation_for_frame(audio_first)
        stamp = stamps.get(c_slot)
        lead = written_end - audio_end

        future = audio_end > written_end
        overwritten = audio_first < oldest_valid
        covered = (
            stamp is not None
            and stamp.generation == expected_gen
            and stamp.begin <= audio_first
            and stamp.end >= audio_end
        )

        res.total += 1
        res.leads.append(lead)
        if covered:
            res.covered += 1
        else:
            if future:
                res.future += 1
            if overwritten:
                res.overwritten += 1
            if stamp is not None and stamp.generation != expected_gen:
                res.gen_mismatch += 1
            if capture_samples and len(res.samples) < capture_samples:
                res.samples.append(
                    f"  UNCOVERED slot={c_slot} "
                    f"expected=[{audio_first},{audio_end}) gen={expected_gen} "
                    f"stamp=[{stamp.begin if stamp else 0},{stamp.end if stamp else 0}) "
                    f"stampGen={stamp.generation if stamp else 0} "
                    f"timeline=[{sched},{sched + fpp}) offset={offset_eff} "
                    f"written=[{oldest_valid},{written_end}) "
                    f"lagToWritten={written_end - audio_first} "
                    f"missToStamp={audio_first - (stamp.begin if stamp else 0)}"
                )

        sched += fpp
    return res


def min_safe_offset(gap: int, fpp: int, target_lead: int) -> int:
    """Smallest packet-aligned offset with writtenEnd - audioEnd >= target_lead."""
    floor = gap + fpp + target_lead
    return ((floor + fpp - 1) // fpp) * fpp


def print_result(res: Result, *, target_lead: int, capacity: int, fpp: int) -> None:
    leads = res.leads
    lo = min(leads) if leads else 0
    med = statistics.median(leads) if leads else 0
    hi = max(leads) if leads else 0
    verdict = "OK" if res.covered == res.total else "BROKEN"
    print(f"offset={res.offset:5d}  covered={res.covered_pct:6.1f}%  "
          f"({res.covered}/{res.total})  future={res.future}  old={res.overwritten}  "
          f"genMismatch={res.gen_mismatch}  lead[min/med/max]={lo}/{med:.0f}/{hi}  "
          f"=> {verdict}")
    for line in res.samples:
        print(line)


def run_single(ring: Ring, *, offset: int, args: argparse.Namespace) -> Result:
    res = simulate(
        ring=ring, offset=offset, gap=args.gap, packets=args.packets,
        target_lead=args.target_lead, warmup_laps=args.warmup, capture_samples=args.show,
    )
    print_result(res, target_lead=args.target_lead, capacity=ring.capacity, fpp=ring.fpp)
    return res


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--capacity", type=int, default=4096, help="output ring frames")
    p.add_argument("--fpp", type=int, default=8, help="framesPerPacket")
    p.add_argument("--gap", type=int, default=4840,
                   help="sched - wr phase between wire and HAL clocks (observed 4436..4856)")
    p.add_argument("--packets", type=int, default=20000, help="packets to simulate")
    p.add_argument("--target-lead", type=int, default=128,
                   help="required writtenEnd - audioEnd safety margin (frames)")
    p.add_argument("--warmup", type=int, default=2, help="laps to pre-fill before measuring")
    p.add_argument("--offset", type=int, help="single offset to simulate")
    p.add_argument("--sweep", help="start:end:step, e.g. 0:8192:8")
    p.add_argument("--compare", help="comma-separated offsets to A/B, e.g. 3072,5120")
    p.add_argument("--show", type=int, default=0,
                   help="print up to N example uncovered packets (log-style)")
    args = p.parse_args()

    ring = Ring(capacity=args.capacity, fpp=args.fpp)
    rec = min_safe_offset(args.gap, args.fpp, args.target_lead)
    safe_ceiling = args.gap + args.capacity  # audioFirst hits oldestValid here

    print("=== ASFW TX ring two-clock simulation ===")
    print(f"capacity={args.capacity} fpp={args.fpp} packetSlots={ring.packet_slots} "
          f"gap(sched-wr)={args.gap}")
    print(f"safe offset window: [{rec}, {safe_ceiling}]  "
          f"(floor = gap+fpp+lead, ceiling = gap+capacity)")
    print(f"recommended smallest-safe offset: {rec}")
    print()

    if args.compare:
        offsets = [int(x, 0) for x in args.compare.split(",") if x.strip()]
        print("--- compare ---")
        for off in offsets:
            args.show = max(args.show, 2) if off < rec else 0
            run_single(ring, offset=off, args=args)
        return 0

    if args.offset is not None:
        run_single(ring, offset=args.offset, args=args)
        return 0

    if args.sweep:
        s, e, step = (int(x, 0) for x in args.sweep.split(":"))
        print("--- sweep ---")
        best: Optional[int] = None
        for off in range(s, e + 1, step):
            res = simulate(ring=ring, offset=off, gap=args.gap, packets=args.packets,
                           target_lead=args.target_lead, warmup_laps=args.warmup)
            if res.covered == res.total and best is None:
                best = off
            # Only print the transition band to keep output readable.
            if best is None or off <= best + 4 * step:
                print_result(res, target_lead=args.target_lead,
                             capacity=ring.capacity, fpp=ring.fpp)
        print()
        if best is not None:
            print(f"smallest fully-covered offset in sweep: {best}")
        else:
            print("no fully-covered offset in sweep range")
        return 0

    # Default: demonstrate the real-world before/after.
    print("--- default demo: old offset 3072 vs fixed 5120 ---")
    for off in (3072, 5120):
        args.show = 2 if off < rec else 0
        run_single(ring, offset=off, args=args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
