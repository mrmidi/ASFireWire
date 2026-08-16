#!/usr/bin/env python3
"""Reference AMDTP transmit model for the M-Audio 1814 internal-clock path.

**Scope: the M-Audio "special" family path** — the behaviour
`Audio/Families/BeBoB/MAudio/ComputeInternalTxSyt` is expected to produce.  This
is not a model of the general `Audio/Wire/AMDTP` generator.

It exists to answer "what *should* the wire look like", independently of what we
currently emit, and to generate a golden capture that `firebug_syt_check.py`
accepts.  Three rules, each taken from a reference rather than invented:

1. **Cadence.**  DATA packets per cycle = rate / (SYT_INTERVAL * 8000), run as a
   Bresenham accumulator.  At 48 kHz that is 3/4; at 44.1 kHz 441/640, whose
   complement 199/640 is exactly the vendor kext's empty-packet generator
   (`SetupCIP` @0x27102).

2. **SYT step.**  Presentation time advances by exactly
   24576000 * SYT_INTERVAL / rate ticks per DATA packet, kept as a rational so
   the 44.1 kHz family neither rounds nor drifts.  The vendor uses base 4458
   with a 339/441 correction; Linux uses a 147-phase accumulator
   (`amdtp-stream.c:429-437`).  Both realise the same exact value.

3. **DBC.**  Advances by the data blocks actually carried, so a header-only
   packet advances it by zero (IEC 61883-1 6.2.2, `amdtp-stream.c:359-363`,
   FFADO `fillNoDataPacketHeader`).

The lead is a consequence, not a parameter: seeding presentation time at
`first transmit position + transfer_delay` makes it self-sustaining, because
step * (data packets per cycle) == 3072 ticks exactly for every rate.  Linux's
transfer delay is `0x2e00 - 3072 + step` (`amdtp-stream.c:288-292`).

Usage:
    python3 amdtp_tx_reference.py --rate 48000 --cycles 64 --emit golden48k.txt
    python3 amdtp_tx_reference.py --rate 44100 --cycles 640 --self-check
"""

from __future__ import annotations

import argparse
from fractions import Fraction

TICKS_PER_CYCLE = 3072
CYCLES_PER_SECOND = 8000
TICKS_PER_SECOND = TICKS_PER_CYCLE * CYCLES_PER_SECOND
SYT_FIELD_TICKS = 16 * TICKS_PER_CYCLE

SYT_INTERVAL = {32000: 8, 44100: 8, 48000: 8,
                88200: 16, 96000: 16,
                176400: 32, 192000: 32}
SFC = {32000: 0, 44100: 1, 48000: 2, 88200: 3, 96000: 4, 176400: 5, 192000: 6}

# 1814 playback geometry at the 44.1/48 kHz band, S/PDIF: 6 PCM + 1 MIDI.
DEFAULT_DBS = 7


class AmdtpTxReference:
    """Generates the packet sequence one cycle at a time."""

    def __init__(self, rate: int, dbs: int = DEFAULT_DBS, node_id: int = 0):
        if rate not in SYT_INTERVAL:
            raise ValueError(f"unsupported rate {rate}")
        self.rate = rate
        self.dbs = dbs
        self.node_id = node_id
        self.interval = SYT_INTERVAL[rate]
        self.fdf = SFC[rate]

        # Rule 2: exact ticks per SYT event, held as a rational.
        self.step = Fraction(TICKS_PER_SECOND * self.interval, rate)
        # Rule 1: exact DATA packets per cycle.
        self.data_per_cycle = Fraction(rate, self.interval * CYCLES_PER_SECOND)
        # Linux transfer delay: TRANSFER_DELAY_TICKS - TICKS_PER_CYCLE + step.
        self.transfer_delay = Fraction(0x2E00 - TICKS_PER_CYCLE) + self.step

        self._cadence_acc = Fraction(0)
        self._presentation = None      # absolute ticks, rational
        self._dbc = 0
        self._events = 0

    # -- invariant that makes the lead self-sustaining -------------------
    def check_closure(self) -> None:
        assert self.step * self.data_per_cycle == TICKS_PER_CYCLE, (
            "step * data-per-cycle must equal one cycle exactly")

    def next_packet(self, transmit_ticks: int):
        """Advance one cycle. Returns a dict describing the packet emitted."""
        self._cadence_acc += self.data_per_cycle
        is_data = self._cadence_acc >= 1
        if is_data:
            self._cadence_acc -= 1

        # Anchor on the first DATA packet, not the first cycle: the lead is
        # defined against the transmit time of the packet that carries the SYT,
        # and a leading empty would otherwise offset every timestamp by a cycle.
        if is_data and self._presentation is None:
            self._presentation = Fraction(transmit_ticks) + self.transfer_delay

        if not is_data:
            # Rule 3: a header-only packet carries no blocks, so DBC is unchanged.
            return {"data": False, "dbc": self._dbc, "syt": 0xFFFF,
                    "blocks": 0, "size": 8}

        syt_abs = int(self._presentation)
        syt = self._to_syt_field(syt_abs)
        pkt = {"data": True, "dbc": self._dbc, "syt": syt,
               "blocks": self.interval,
               "size": 8 + self.interval * self.dbs * 4,
               "presentation": syt_abs}

        self._presentation += self.step
        self._dbc = (self._dbc + self.interval) & 0xFF
        self._events += 1
        return pkt

    @staticmethod
    def _to_syt_field(abs_ticks: int) -> int:
        w = abs_ticks % SYT_FIELD_TICKS
        return ((w // TICKS_PER_CYCLE) << 12) | (w % TICKS_PER_CYCLE)

    def cip(self, pkt) -> tuple[int, int]:
        q0 = (self.node_id << 24) | (self.dbs << 16) | pkt["dbc"]
        if pkt["data"]:
            q1 = 0x90000000 | (self.fdf << 16) | pkt["syt"]
        else:
            q1 = 0x90FFFFFF
        return q0, q1


def emit_trace(model: AmdtpTxReference, cycles: int, path: str,
               channel: int = 0, start_sec: int = 20, start_cycle: int = 100,
               sub_offset: int = 1446) -> dict:
    """Write a FireBug-format capture that firebug_parse.py can read."""
    lines = []
    first_ts = f"{start_sec:03d}:{start_cycle:04d}:{sub_offset:04d}"
    # Anchor line: a synthetic trace has no analyzer skew, so CT == timestamp.
    lines.append(f"               Isoch channel {channel} ACTIVE at {first_ts} "
                 f"(CT {start_sec:03d}:{start_cycle:04d}), speed s400")

    stats = {"data": 0, "empty": 0}
    for i in range(cycles):
        abs_cycle = start_cycle + i
        sec = start_sec + abs_cycle // CYCLES_PER_SECOND
        cyc = abs_cycle % CYCLES_PER_SECOND
        transmit_ticks = abs_cycle * TICKS_PER_CYCLE + sub_offset

        pkt = model.next_packet(transmit_ticks)
        stats["data" if pkt["data"] else "empty"] += 1
        q0, q1 = model.cip(pkt)

        ts = f"{sec:03d}:{cyc:04d}:{sub_offset:04d}"
        lines.append(f"{ts}  Isoch channel {channel}, tag 1, sy 0, "
                     f"size {pkt['size']} [actual {pkt['size']}] s400")

        words = [q0, q1]
        if pkt["data"]:
            # AM824: label 0x40 on PCM slots, 0x80 on the MIDI slot.
            for b in range(pkt["blocks"]):
                for s in range(model.dbs):
                    words.append(0x80000000 if s == model.dbs - 1 else 0x40000000)
        for off in range(0, len(words), 4):
            chunk = words[off:off + 4]
            lines.append("               %04x   %s" %
                         (off * 4, " ".join(f"{w:08x}" for w in chunk)))

    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    return stats


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--dbs", type=int, default=DEFAULT_DBS)
    ap.add_argument("--cycles", type=int, default=64)
    ap.add_argument("--channel", type=int, default=0)
    ap.add_argument("--emit", metavar="PATH", help="write a FireBug-format capture")
    ap.add_argument("--self-check", action="store_true",
                    help="verify the rational closure and long-run ratios")
    args = ap.parse_args()

    model = AmdtpTxReference(args.rate, args.dbs)
    model.check_closure()
    print(f"rate={args.rate} SYT_INTERVAL={model.interval} DBS={args.dbs}")
    print(f"  step            = {model.step}  ({float(model.step):.6f} ticks)")
    print(f"  data per cycle  = {model.data_per_cycle}  "
          f"({float(model.data_per_cycle):.6f})")
    print(f"  empties         = {1 - model.data_per_cycle}")
    print(f"  transfer delay  = {float(model.transfer_delay):.1f} ticks "
          f"({float(model.transfer_delay) / TICKS_PER_CYCLE:.3f} cycles)")
    print(f"  closure         : step * data/cycle = "
          f"{model.step * model.data_per_cycle} ticks per cycle")

    if args.self_check:
        m = AmdtpTxReference(args.rate, args.dbs)
        n_data = 0
        for i in range(args.cycles):
            p = m.next_packet(i * TICKS_PER_CYCLE)
            n_data += 1 if p["data"] else 0
        want = model.data_per_cycle * args.cycles
        print(f"  self-check      : {n_data} DATA in {args.cycles} cycles, "
              f"exact {float(want):.3f} -> "
              f"{'OK' if abs(n_data - want) < 1 else 'MISMATCH'}")

    if args.emit:
        stats = emit_trace(model, args.cycles, args.emit, args.channel)
        print(f"  wrote {args.emit}: {stats['data']} DATA, {stats['empty']} empty")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
