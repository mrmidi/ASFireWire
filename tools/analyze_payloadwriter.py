#!/usr/bin/env python3
"""
Analyze [PayloadWriter] telemetry dumps from a kernel log.

Extracts the load-bearing input for tx_payload_ownership_sim.py: the measured
lead margin between the producer exposure frontier (E = exposedEnd) and the
CoreAudio write window (W = sampleTime + frameCount). leadMargin < 0 is exactly
the `withoutPkt` dropout condition.

Per-line fields are instantaneous (sampleTime, frameCount, exposedEnd,
completion). The miss counters (visited/written/withoutPkt/outsidePkt/...) are
CUMULATIVE, so per-call work is recovered by differencing consecutive lines.

Usage:
    python3 analyze_payloadwriter.py payloadwriter.txt
    python3 analyze_payloadwriter.py payloadwriter.txt --hist
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional

AVG_FRAMES_PER_PKT = 6.0  # blocking 48 kHz cadence (8,8,8,0)

FIELD_RE = re.compile(r"(\w+)=(-?\d+)")


@dataclass
class Record:
    sampleTime: int
    frameCount: int
    frameCapacity: int
    completion: int
    exposedEnd: int
    visited: int
    written: int
    withoutPkt: int
    outsidePkt: int
    raced: int
    nonZero: int

    @property
    def w_end(self) -> int:
        return self.sampleTime + self.frameCount

    @property
    def lead_margin(self) -> int:
        """E - W_end. Negative => tail of IO window past exposure (withoutPkt)."""
        return self.exposedEnd - self.w_end

    @property
    def start_margin(self) -> int:
        """E - W_start."""
        return self.exposedEnd - self.sampleTime


def parse(path: str) -> List[Record]:
    recs: List[Record] = []
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            if "[PayloadWriter]" not in line or "sampleTime=" not in line:
                continue
            kv: Dict[str, int] = {k: int(v) for k, v in FIELD_RE.findall(line)}
            try:
                recs.append(Record(
                    sampleTime=kv["sampleTime"], frameCount=kv["frameCount"],
                    frameCapacity=kv.get("frameCapacity", 0),
                    completion=kv.get("completion", 0), exposedEnd=kv["exposedEnd"],
                    visited=kv["visited"], written=kv["written"],
                    withoutPkt=kv["withoutPkt"], outsidePkt=kv["outsidePkt"],
                    raced=kv.get("raced", 0), nonZero=kv.get("nonZero", 0)))
            except KeyError:
                continue
    return recs


def pct(sorted_vals: List[int], p: float) -> int:
    if not sorted_vals:
        return 0
    return sorted_vals[min(len(sorted_vals) - 1, int(p * len(sorted_vals)))]


def summarize(name: str, vals: List[int]) -> None:
    s = sorted(vals)
    mean = sum(s) / len(s)
    print(f"  {name:14} n={len(s)}  min={s[0]}  p1={pct(s,0.01)}  p5={pct(s,0.05)}  "
          f"median={pct(s,0.5)}  mean={mean:.1f}  p95={pct(s,0.95)}  max={s[-1]}")


def histogram(vals: List[int], width: int = 56) -> None:
    lo, hi = min(vals), max(vals)
    nbins = min(16, max(1, hi - lo + 1))
    step = max(1, (hi - lo + 1 + nbins - 1) // nbins)
    bins: Dict[int, int] = {}
    for v in vals:
        bins[(v - lo) // step] = bins.get((v - lo) // step, 0) + 1
    peak = max(bins.values())
    print("  leadMargin histogram (frames):")
    for b in range(nbins):
        c = bins.get(b, 0)
        bar = "#" * int(width * c / peak) if peak else ""
        edge_lo = lo + b * step
        edge_hi = edge_lo + step - 1
        print(f"    [{edge_lo:+5d}..{edge_hi:+5d}] {c:5d} {bar}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile")
    ap.add_argument("--hist", action="store_true", help="print leadMargin histogram")
    args = ap.parse_args()

    recs = parse(args.logfile)
    if not recs:
        print("no [PayloadWriter] records found", file=sys.stderr)
        sys.exit(1)

    print(f"[PayloadWriter] analysis  ({len(recs)} records)\n")

    # --- Cumulative totals over the whole run (last - first) ---
    first, last = recs[0], recs[-1]
    dv = last.visited - first.visited
    dw = last.written - first.written
    dwo = last.withoutPkt - first.withoutPkt
    do = last.outsidePkt - first.outsidePkt
    dnz = last.nonZero - first.nonZero
    print("Cumulative over captured window (delta last-first):")
    if dv > 0:
        print(f"  visited={dv}  written={dw} ({100*dw/dv:.3f}%)  "
              f"withoutPkt={dwo} ({100*dwo/dv:.3f}%)  outsidePkt={do} ({100*do/dv:.4f}%)")
        print(f"  nonZero(source signal)={dnz} ({100*dnz/dw:.3f}% of written) "
              f"-> source is {'mostly SILENT' if dnz < 0.1*dw else 'active'}")
    # absolute (whole-session) ratios from the final cumulative snapshot
    print("Absolute cumulative (whole session, from final line):")
    print(f"  visited={last.visited}  withoutPkt={last.withoutPkt} "
          f"({100*last.withoutPkt/last.visited:.3f}%)  "
          f"outsidePkt={last.outsidePkt} ({100*last.outsidePkt/last.visited:.4f}%)  "
          f"raced={last.raced}")
    print()

    # --- Lead margin distribution (the sim input) ---
    margins = [r.lead_margin for r in recs]
    starts = [r.start_margin for r in recs]
    print("Lead margin (E - W), the sim's load-bearing input:")
    summarize("E - W_end", margins)
    summarize("E - W_start", starts)
    neg = [m for m in margins if m < 0]
    print(f"  negative E-W_end: {len(neg)}/{len(margins)} calls "
          f"({100*len(neg)/len(margins):.1f}%)  worst={min(margins)} frames")
    print()

    # --- Correlate negative margin with per-call withoutPkt delta ---
    pos_drop = neg_drop = pos_n = neg_n = 0
    for a, b in zip(recs, recs[1:]):
        d = b.withoutPkt - a.withoutPkt
        if b.lead_margin < 0:
            neg_drop += d; neg_n += 1
        else:
            pos_drop += d; pos_n += 1
    print("withoutPkt delta vs leadMargin sign (per-call):")
    if neg_n:
        print(f"  margin<0 calls: {neg_n}  total withoutPkt+={neg_drop}  "
              f"avg={neg_drop/neg_n:.1f}/call")
    if pos_n:
        print(f"  margin>=0 calls: {pos_n}  total withoutPkt+={pos_drop}  "
              f"avg={pos_drop/pos_n:.1f}/call")
    print()

    # --- Pipeline offset: W vs HW completion cursor (DIFFERENT anchor from E;
    #     this is whole-pipeline buffering depth, NOT the E-W dropout driver) ---
    sodiffs = [r.sampleTime - int(round(r.completion * AVG_FRAMES_PER_PKT))
               for r in recs if r.completion > 0]
    if sodiffs:
        summarize("W - HWcompl", sodiffs)
        print(f"  ~= {sum(sodiffs)/len(sodiffs)/AVG_FRAMES_PER_PKT:.1f} packets pipeline depth "
              f"(stable offset; NOT the dropout driver -- E-W above is)")
    print()

    # --- Recommendation for the sim ---
    worst = min(margins)
    print("=> Feed the sim with the MEASURED margin, not an assumed safety:")
    print(f"   worst-case leadMargin = {worst} frames "
          f"(~{worst/AVG_FRAMES_PER_PKT:.1f} packets).")
    print(f"   To kill withoutPkt the fix must push this margin >= 0 with cushion; "
          f"current min is {worst}.")
    print()

    # --- Validate added-lead choices against the MEASURED distribution ---
    # Adding D frames of exposure lead shifts every observed E-W by +D (E and W
    # are clock-locked, confirmed by the stable W-HWcompletion offset). So the
    # residual dropout at a candidate lead is just the negative tail of the
    # shifted empirical distribution -- pure data, no model.
    n = len(margins)
    print("Added-lead validation (shift MEASURED E-W by +delta; no model):")
    print(f"  {'+delta(fr)':>10} {'+pkts':>6} {'neg calls':>11} {'% neg':>8} {'worst':>7}")
    for delta in range(0, 121, 8):
        neg = sum(1 for m in margins if m + delta < 0)
        worstd = min(m + delta for m in margins)
        pkts = round(delta / AVG_FRAMES_PER_PKT)
        print(f"  {delta:>10} {pkts:>6} {neg:>11} {100*neg/n:>7.2f}% {worstd:>+7}")
    print("  (first delta with 0 neg / worst>=0 = the lead the OBSERVED run needs)")

    if args.hist:
        print()
        histogram(margins)


if __name__ == "__main__":
    main()
