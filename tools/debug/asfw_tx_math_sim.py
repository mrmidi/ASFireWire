#!/usr/bin/env python3
"""
ASFireWire TX timeline / packet-ring coverage simulator.

Purpose:
  Use ASFW kernel logs to tune txOutputOffsetFrames_ without rebuilding.
  It parses:
    - ADK snapshot/ring playback(... timeline(...))
    - TX PAYLOAD UNCOVERED slot=... expected=[A,B) gen=... stamp=[C,D) stampGen=...
    - TX RING POPULATE: range=[A, B)

Then it answers:
  - Is TX requesting frames ahead of CoreAudio writtenEnd?
  - Are uncovered misses exactly one ring lap ahead?
  - Which txOutputOffsetFrames candidate would make audioFirst land inside
    [oldestValid, writtenEnd] with safety margin?

Usage:
  python3 asfw_tx_math_sim.py --log ~/DEV/isoch-clean.log --sweep 0:4096:8
  python3 asfw_tx_math_sim.py --log ~/DEV/isoch-clean.log --offsets 768,1536,2304,3072,3584,4096

Optional:
  --target-lead 256
  --capacity 4096
  --fpp 8
"""

from __future__ import annotations

import argparse
import dataclasses
import re
import statistics
from collections import Counter
from pathlib import Path
from typing import Iterable


SNAPSHOT_RE = re.compile(
    r"playback\(wr=(?P<wr>\d+)\s+rd=(?P<rd>\d+)\s+oldest=(?P<oldest>\d+)\s+avail=(?P<avail>\d+).*?"
    r"timeline\(sched=(?P<sched>\d+)\s+done=(?P<done>\d+)"
)

UNCOVERED_RE = re.compile(
    r"TX PAYLOAD UNCOVERED slot=(?P<slot>\d+)\s+"
    r"expected=\[(?P<exp_begin>\d+),(?P<exp_end>\d+)\)\s+"
    r"gen=(?P<gen>\d+)\s+"
    r"stamp=\[(?P<stamp_begin>\d+),(?P<stamp_end>\d+)\)\s+"
    r"stampGen=(?P<stamp_gen>\d+)"
)

POPULATE_RE = re.compile(
    r"TX RING POPULATE: range=\[(?P<begin>\d+),\s*(?P<end>\d+)\)\s+"
    r"count=(?P<count>\d+)\s+oldestValid=(?P<oldest>\d+)"
)


@dataclasses.dataclass(frozen=True)
class Snapshot:
    line_no: int
    wr: int
    rd: int
    oldest: int
    avail: int
    sched: int
    done: int

    @property
    def tx_ahead_of_written(self) -> int:
        return self.sched - self.wr

    @property
    def tx_ahead_of_done(self) -> int:
        return self.sched - self.done


@dataclasses.dataclass(frozen=True)
class Uncovered:
    line_no: int
    slot: int
    exp_begin: int
    exp_end: int
    gen: int
    stamp_begin: int
    stamp_end: int
    stamp_gen: int

    @property
    def lap_delta(self) -> int:
        return self.exp_begin - self.stamp_begin

    @property
    def gen_delta(self) -> int:
        return self.gen - self.stamp_gen


@dataclasses.dataclass(frozen=True)
class Populate:
    line_no: int
    begin: int
    end: int
    count: int
    oldest: int


@dataclasses.dataclass
class OffsetScore:
    offset: int
    total: int = 0
    ok: int = 0
    future: int = 0
    overwritten: int = 0
    unaligned: int = 0
    min_lead: int | None = None
    max_lead: int | None = None
    median_lead: float | None = None
    leads: list[int] = dataclasses.field(default_factory=list)

    def add(self, *, ok: bool, future: bool, overwritten: bool, unaligned: bool, lead: int) -> None:
        self.total += 1
        if ok:
            self.ok += 1
        if future:
            self.future += 1
        if overwritten:
            self.overwritten += 1
        if unaligned:
            self.unaligned += 1
        self.leads.append(lead)

    def finalize(self) -> None:
        if self.leads:
            self.min_lead = min(self.leads)
            self.max_lead = max(self.leads)
            self.median_lead = statistics.median(self.leads)

    @property
    def ok_pct(self) -> float:
        return 0.0 if self.total == 0 else 100.0 * self.ok / self.total


def parse_log(path: Path) -> tuple[list[Snapshot], list[Uncovered], list[Populate]]:
    snapshots: list[Snapshot] = []
    uncovered: list[Uncovered] = []
    populates: list[Populate] = []

    with path.expanduser().open("r", encoding="utf-8", errors="replace") as f:
        for line_no, line in enumerate(f, start=1):
            if m := SNAPSHOT_RE.search(line):
                snapshots.append(Snapshot(
                    line_no=line_no,
                    wr=int(m.group("wr")),
                    rd=int(m.group("rd")),
                    oldest=int(m.group("oldest")),
                    avail=int(m.group("avail")),
                    sched=int(m.group("sched")),
                    done=int(m.group("done")),
                ))
            if m := UNCOVERED_RE.search(line):
                uncovered.append(Uncovered(
                    line_no=line_no,
                    slot=int(m.group("slot")),
                    exp_begin=int(m.group("exp_begin")),
                    exp_end=int(m.group("exp_end")),
                    gen=int(m.group("gen")),
                    stamp_begin=int(m.group("stamp_begin")),
                    stamp_end=int(m.group("stamp_end")),
                    stamp_gen=int(m.group("stamp_gen")),
                ))
            if m := POPULATE_RE.search(line):
                populates.append(Populate(
                    line_no=line_no,
                    begin=int(m.group("begin")),
                    end=int(m.group("end")),
                    count=int(m.group("count")),
                    oldest=int(m.group("oldest")),
                ))

    return snapshots, uncovered, populates


def parse_offsets(args: argparse.Namespace) -> list[int]:
    values: set[int] = set()
    if args.offsets:
        for part in args.offsets.split(","):
            part = part.strip()
            if part:
                values.add(int(part, 0))
    if args.sweep:
        start_s, end_s, step_s = args.sweep.split(":")
        start, end, step = int(start_s, 0), int(end_s, 0), int(step_s, 0)
        if step <= 0:
            raise ValueError("--sweep step must be > 0")
        values.update(range(start, end + 1, step))
    if not values:
        values.update([768, 1536, 2304, 3072, 3584, 4096])
    return sorted(values)


def score_offsets(
    snapshots: Iterable[Snapshot],
    offsets: Iterable[int],
    *,
    fpp: int,
    target_lead: int,
) -> list[OffsetScore]:
    out: list[OffsetScore] = []

    for offset in offsets:
        score = OffsetScore(offset=offset)
        for s in snapshots:
            audio_first = s.sched - offset
            audio_end = audio_first + fpp
            lead = s.wr - audio_end

            future = audio_end > s.wr
            overwritten = audio_first < s.oldest
            base = s.oldest - (s.oldest % fpp)
            unaligned = (audio_first % fpp) != (base % fpp)
            ok = (not future) and (not overwritten) and lead >= target_lead

            score.add(ok=ok, future=future, overwritten=overwritten, unaligned=unaligned, lead=lead)

        score.finalize()
        out.append(score)

    return out


def print_summary(
    snapshots: list[Snapshot],
    uncovered: list[Uncovered],
    populates: list[Populate],
    *,
    capacity: int,
) -> None:
    print("=== Parsed log ===")
    print(f"snapshots: {len(snapshots)}")
    print(f"uncovered: {len(uncovered)}")
    print(f"populate ranges: {len(populates)}")
    print()

    if snapshots:
        ahead = [s.tx_ahead_of_written for s in snapshots]
        print("=== TX scheduled head vs CoreAudio writtenEnd ===")
        print(f"sched - wr: min={min(ahead)} median={statistics.median(ahead):.1f} max={max(ahead)} frames")
        print("first snapshots:")
        for s in snapshots[:5]:
            print(
                f"  line {s.line_no}: wr={s.wr} oldest={s.oldest} sched={s.sched} "
                f"sched-wr={s.tx_ahead_of_written}"
            )
        print()

    if uncovered:
        lap_hist = Counter(u.lap_delta for u in uncovered)
        gen_hist = Counter(u.gen_delta for u in uncovered)
        print("=== Uncovered slot misses ===")
        print("lap delta histogram, expected.begin - stamp.begin:")
        for value, count in lap_hist.most_common(10):
            marker = "  <== one ring lap" if value == capacity else ""
            print(f"  {value:>8}: {count}{marker}")
        print("generation delta histogram:")
        for value, count in gen_hist.most_common(10):
            print(f"  {value:>8}: {count}")
        print("first uncovered:")
        for u in uncovered[:5]:
            print(
                f"  line {u.line_no}: slot={u.slot} expected=[{u.exp_begin},{u.exp_end}) "
                f"stamp=[{u.stamp_begin},{u.stamp_end}) delta={u.lap_delta} "
                f"gen={u.gen} stampGen={u.stamp_gen}"
            )
        print()


def print_scores(scores: list[OffsetScore], *, target_lead: int) -> None:
    print(f"=== Offset sweep target: writtenEnd - audioEnd >= {target_lead} frames ===")
    print("offset  ok%     ok/total   future  old     unalign  minLead  medLead  maxLead")
    print("------  ------  ---------  ------  ------  -------  -------  -------  -------")

    for s in scores:
        median = None if s.median_lead is None else round(s.median_lead, 1)
        print(
            f"{s.offset:6d}  {s.ok_pct:6.1f}  "
            f"{s.ok:4d}/{s.total:<4d}  "
            f"{s.future:6d}  {s.overwritten:6d}  {s.unaligned:7d}  "
            f"{str(s.min_lead):>7}  {str(median):>7}  {str(s.max_lead):>7}"
        )

    good = [s for s in scores if s.total and s.future == 0 and s.overwritten == 0 and s.ok == s.total]
    if good:
        best = min(good, key=lambda x: x.offset)
        print()
        print(f"Suggested smallest fully-safe offset: {best.offset} frames")
    elif scores:
        best = max(scores, key=lambda x: (x.ok_pct, -x.future, -x.overwritten, -x.offset))
        print()
        print(
            "No fully-safe offset in tested set. Best tested candidate: "
            f"{best.offset} frames ({best.ok_pct:.1f}% ok, future={best.future}, old={best.overwritten})."
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--capacity", type=int, default=4096)
    parser.add_argument("--fpp", type=int, default=8)
    parser.add_argument("--target-lead", type=int, default=128)
    parser.add_argument("--offsets", help="comma-separated offsets, e.g. 768,1536,2304,3072")
    parser.add_argument("--sweep", help="start:end:step, e.g. 0:4096:8")
    args = parser.parse_args()

    snapshots, uncovered, populates = parse_log(args.log)
    offsets = parse_offsets(args)

    print_summary(snapshots, uncovered, populates, capacity=args.capacity)
    scores = score_offsets(snapshots, offsets, fpp=args.fpp, target_lead=args.target_lead)
    print_scores(scores, target_lead=args.target_lead)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
