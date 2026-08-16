#!/usr/bin/env python3
"""Acceptance check for a FireBug capture of the M-Audio 1814 transmit stream.

**Scope: the M-Audio "special" family path.**  The captures in this directory are
produced by `Audio/Families/BeBoB/MAudio/ComputeInternalTxSyt`, the internal-clock
free-run-plus-hardware-anchor generator used for FireWire 1814 / ProjectMix I/O --
*not* by the general `Audio/Wire/AMDTP` SYT generator, which has its own tests in
`tests/audio/SYTGeneratorTests.cpp`.  Do not read a pass here as a statement about
any other device family.

Two of the four invariants are general and two are not:

  general (IEC 61883-6, any AM824 blocking stream)   syt-step, cadence, dbc
  path-specific (band depends on the anchoring model) lead

It takes the packets ASFW actually put on the wire and tests them against rules
the receiving device can observe, rather than against our own intent.  Each maps
to a named mechanism in a reference:

  syt-step   SYT must advance by exactly 24576000 * SYT_INTERVAL / rate ticks
             per DATA packet.  Integer at 32/48/96/192 kHz; for the 44.1 kHz
             family the exact value is fractional, so consecutive deltas must
             alternate between floor and ceil and the running sum must track
             the exact rational -- the vendor kext does this with a Bresenham
             accumulator (SetupCIP @0x27102: 339/441 on base 4458), Linux with
             a phase accumulator (amdtp-stream.c:429-437).

  cadence    DATA packets per second must equal rate / SYT_INTERVAL, so the
             data:empty ratio is fixed per rate.  Vendor: an independent
             accumulator, 199/640 at 44.1 kHz and 1/4 at 48 kHz.

  lead       SYT names a presentation time ahead of the packet's own transmit
             cycle.  Linux computes 12800 ticks (4.167 cycles) at 48 kHz
             (amdtp-stream.c:288-292); the vendor holds 3-5 cycles and
             re-anchors to +4 (OutputDCLCallback @0x263c2).  The device
             classifies out-of-window packets as 'pkt Future' / 'pkt Past'
             (firmware fw1814.bcd, /syt/ namespace).

  dbc        DBC counts data blocks actually transmitted, so a header-only
             packet carries none and must not advance it (IEC 61883-1 6.2.2).
             Linux advances by desc->data_blocks, zero for the cadence empty
             (amdtp-stream.c:359-363); FFADO's fillNoDataPacketHeader returns 0.

Usage:
    python3 firebug_syt_check.py last.txt --channel 0
    python3 firebug_syt_check.py last.txt --channel 0 --lead-band 2.0 8.0
"""

from __future__ import annotations

import argparse
import sys
from fractions import Fraction

import firebug_parse as fp

TICKS_PER_CYCLE = fp.TICKS_PER_CYCLE
CYCLES_PER_SECOND = fp.CYCLES_PER_SECOND
TICKS_PER_SECOND = TICKS_PER_CYCLE * CYCLES_PER_SECOND

# IEC 61883-6 table 6: SYT_INTERVAL per sampling-frequency class.
SYT_INTERVAL = {32000: 8, 44100: 8, 48000: 8,
                88200: 16, 96000: 16,
                176400: 32, 192000: 32}


class Result:
    def __init__(self) -> None:
        self.rows: list[tuple[str, bool, str]] = []

    def add(self, name: str, ok: bool, detail: str) -> None:
        self.rows.append((name, ok, detail))

    @property
    def failed(self) -> bool:
        return any(not ok for _, ok, _ in self.rows)

    def render(self) -> str:
        width = max(len(n) for n, _, _ in self.rows)
        out = []
        for name, ok, detail in self.rows:
            out.append(f"  {'PASS' if ok else 'FAIL'}  {name:<{width}}  {detail}")
        return "\n".join(out)


def collect(events, channel: int):
    """Ordered (ts, cip, size) for one isoch channel."""
    packets = []
    for ev in events:
        if ev.category != "isochpkt" or ev.value != channel:
            continue
        cip = fp.decode_cip(bytes(ev.payload))
        if cip is None:
            continue
        packets.append((ev.ts, cip, ev.size))
    return packets


def abs_cycle(ts) -> int:
    return ts.seconds * CYCLES_PER_SECOND + ts.cycle


def contiguous_runs(packets):
    """Split into runs of packets that are adjacent *on the wire*.

    FireBug elides most traffic ("[402 packets not shown]"), so two captured
    packets are usually not consecutive cycles.  Any check on packet-to-packet
    continuity -- DBC above all -- is meaningless across an elision, and would
    otherwise report a break for every gap.  Isochronous packets go out one per
    cycle, so adjacency is exactly a cycle difference of one.
    """
    runs, cur = [], []
    for pkt in packets:
        if cur and abs_cycle(pkt[0]) - abs_cycle(cur[-1][0]) != 1:
            if len(cur) > 1:
                runs.append(cur)
            cur = []
        cur.append(pkt)
    if len(cur) > 1:
        runs.append(cur)
    return runs


def check_syt_step(runs, rate: int, interval: int, res: Result) -> None:
    exact = Fraction(TICKS_PER_SECOND * interval, rate)
    lo, hi = exact.numerator // exact.denominator, -(-exact.numerator // exact.denominator)

    deltas = []
    for run in runs:
        prev = None
        for _, cip, _ in run:
            if cip.syt == 0xFFFF:
                continue
            t = cip.syt_ticks
            if prev is not None:
                deltas.append((t - prev) % (16 * TICKS_PER_CYCLE))
            prev = t

    if not deltas:
        res.add("syt-step", False, "no DATA packets with a SYT")
        return

    bad = [d for d in deltas if d not in (lo, hi)]
    drift = sum(deltas) - exact * len(deltas)
    ok = not bad and abs(drift) <= 1
    detail = (f"n={len(deltas)} exact={float(exact):.4f} "
              f"seen={{{','.join(str(d) for d in sorted(set(deltas)))}}} "
              f"drift={float(drift):+.3f} ticks")
    if bad:
        detail += f"  OFF-GRID={sorted(set(bad))[:4]}"
    res.add("syt-step", ok, detail)


def check_cadence(runs, rate: int, interval: int, res: Result) -> None:
    data = sum(1 for run in runs for _, c, _ in run if c.syt != 0xFFFF)
    empty = sum(1 for run in runs for _, c, _ in run if c.syt == 0xFFFF)
    total = data + empty
    if total == 0:
        res.add("cadence", False, "no packets")
        return
    want = Fraction(rate, interval * CYCLES_PER_SECOND)   # data packets per cycle
    got = Fraction(data, total)
    # A short capture cannot resolve the exact ratio; allow one packet of slack.
    ok = abs(float(got) - float(want)) <= 1.0 / total + 1e-9
    res.add("cadence", ok,
            f"data={data} empty={empty} ratio={float(got):.4f} expected={float(want):.4f}")


def check_lead(packets, offset, band, res: Result) -> None:
    if offset is None:
        res.add("lead", False, "no CT anchor line in trace; lead unknowable")
        return
    leads = []
    for ts, cip, _ in packets:
        if cip.syt == 0xFFFF:
            continue
        bus = (ts.seconds * CYCLES_PER_SECOND + ts.cycle + offset) % (
            fp.SECONDS_MODULUS * CYCLES_PER_SECOND)
        position = (bus % 16) * TICKS_PER_CYCLE + ts.offset
        lead = ((cip.syt_ticks - position) % (16 * TICKS_PER_CYCLE)) / TICKS_PER_CYCLE
        leads.append(lead)
    if not leads:
        res.add("lead", False, "no DATA packets with a SYT")
        return
    lo, hi = min(leads), max(leads)
    ok = band[0] <= lo and hi <= band[1]
    res.add("lead", ok,
            f"n={len(leads)} min={lo:.2f} max={hi:.2f} cyc  band=[{band[0]}, {band[1]}]")


def check_dbc(runs, res: Result) -> None:
    """DBC must advance by the data blocks carried: zero for a header-only packet."""
    viol, pairs = [], 0
    for run in runs:
        prev_dbc = prev_blocks = None
        for ts, cip, size in run:
            blocks = 0
            if cip.syt != 0xFFFF and cip.dbs:
                blocks = max(size - 8, 0) // 4 // cip.dbs
            if prev_dbc is not None:
                pairs += 1
                want = (prev_dbc + prev_blocks) & 0xFF
                if cip.dbc != want:
                    viol.append((str(ts), prev_dbc, prev_blocks, cip.dbc, want))
            prev_dbc, prev_blocks = cip.dbc, blocks
    ok = not viol
    if ok:
        detail = f"{pairs} adjacent pairs, continuous"
    else:
        t, pd, pb, got, want = viol[0]
        detail = (f"{len(viol)}/{pairs} breaks; first at {t}: "
                  f"prev=0x{pd:02x}+{pb} -> got 0x{got:02x}, want 0x{want:02x}")
    res.add("dbc", ok, detail)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--channel", type=int, default=0,
                    help="isoch channel to validate (default 0 = host->device)")
    ap.add_argument("--lead-band", type=float, nargs=2, default=(2.0, 8.0),
                    metavar=("LO", "HI"),
                    help="acceptable SYT lead in cycles (default 2.0 8.0; "
                         "Linux 4.17-4.83, vendor 3-5)")
    args = ap.parse_args()

    events, _ = fp.parse(args.trace)
    fp.pair_transactions(events)
    packets = collect(events, args.channel)

    print(f"{args.trace}  channel {args.channel}")
    if not packets:
        print("  no CIP-headered isoch packets on this channel")
        return 2

    rates = {c.fdf & 0x07 for _, c, _ in packets if c.fdf != 0xFF}
    if len(rates) != 1:
        print(f"  cannot determine a single rate (SFC seen: {sorted(rates)})")
        return 2
    rate = fp.SFC_RATE[next(iter(rates))]
    interval = SYT_INTERVAL[rate]
    dbs = {c.dbs for _, c, _ in packets}
    print(f"  rate={rate} Hz  SYT_INTERVAL={interval}  DBS={sorted(dbs)}  "
          f"packets={len(packets)}")

    offset = fp.bus_cycle_offset(args.trace)
    runs = contiguous_runs(packets)
    print(f"  contiguous runs={len(runs)} "
          f"(lengths {[len(r) for r in runs][:8]}) -- continuity checked within runs only")

    res = Result()
    check_syt_step(runs, rate, interval, res)
    check_cadence(runs, rate, interval, res)
    check_lead(packets, offset, tuple(args.lead_band), res)
    check_dbc(runs, res)
    print(res.render())
    return 1 if res.failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
