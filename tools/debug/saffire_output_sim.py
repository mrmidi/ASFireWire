#!/usr/bin/env python3
"""
Saffire.kext OUTPUT-PHASE simulator -- the reference TX timing model, narrated.

Reverse-engineered from Focusrite's Saffire.kext (DICE/TCAT), IDA DB Saffire.i64:
  * FillFirewireBuffers   @ 0xE778  -- send callback: derive phase, gate lead, emit SYT
  * adjustOutputPhase     @ 0xC9C2  -- deadbanded slewing PLL for the output phase
  * tstampToOffsets/SYTDiff/extOffsetDiff -- the 24.576 MHz "offset" tick helpers

WHY THIS EXISTS
---------------
Our ASFW DICE TX path runs a free-running "timeline head" that drifts ~4840 frames ahead
of CoreAudio's write pointer, so it needs a giant txOutputOffsetFrames_ (5120) to read
back into written audio. Saffire does NOT work that way. It re-derives the output phase
*every send-callback* from the recovered device master clock, adds about ONE isoch cycle,
deadband-nudges it, and clamps the resulting lead to 1-4 cycles. So the device-facing lead
is 6-24 FRAMES, not thousands -- and there is no two-clock gap to cancel.

This script reproduces that logic with the real constants and prints the math each callback.

VERIFIED CONSTANTS (straight from the disassembly)
--------------------------------------------------
  TICKS_PER_CYCLE = 3072      # 24.576 MHz / 8000 isoch cycles  (one packet = one cycle)
  TICKS_PER_FRAME = 512       # 24.576 MHz / 48000              (=> 3072 ticks = 6 frames)
  PHASE_MOD       = 0xBB80000 # = 196,608,000 = 4096 * 48000 = 64000 cycles = 8 s wrap
  LEAD_ACCEPT     = 7620      # cmp eax,1DC4h @0xEC8A : lead < 7620 ticks (~2.48 cyc) -> DATA
  LEAD_TIGHT      = 3072      # <= 3071 ticks (under 1 cyc) -> warn "lead too tight"
  LEAD_REJECT     = 12287     # cmp eax,2FFFh @0xEC91 : lead > 12287 (~4 cyc) -> NO-DATA
  SYT_RING_SIZE   = 512       # dice_stream+406 index masked &0x1FF
  SYT_RING_SEED   = 256       # (clock + 256) & 0x1FF -- seeded at the ring midpoint

VERIFIED HELPER VALUES (from tests/core/TimingUtilsTests.cpp)
  tstampToOffsets(SYT 0x1000) = 3072         # cycle field 1 -> 1*3072 ticks
  tstampToOffsets(SYT 0x1ABC) = 3072 + 0xABC # cycle 1, sub-cycle 0xABC ticks

Usage:
  python3 tools/debug/saffire_output_sim.py                          # clean happy path
  python3 tools/debug/saffire_output_sim.py --drift-ppm 3000         # PLL rebases on drift
  python3 tools/debug/saffire_output_sim.py --glitch-at 5 --steps 9  # injected discontinuity
"""

from __future__ import annotations

import argparse
import dataclasses
import random
import statistics

# ---- VERIFIED constants ----------------------------------------------------
TICKS_PER_CYCLE = 3072
TICKS_PER_FRAME = 512
FRAMES_PER_CYCLE = TICKS_PER_CYCLE // TICKS_PER_FRAME           # = 6
PHASE_MOD = 0xBB80000                                           # 4096 * 48000
LEAD_ACCEPT = 7620
LEAD_TIGHT = TICKS_PER_CYCLE                                    # 3072 (<=3071 warns)
LEAD_REJECT = 12287
SYT_RING_SIZE = 512
SYT_RING_SEED = 256


def ticks_to_frames(t: int) -> float:
    return t / TICKS_PER_FRAME


# ---- offset-domain helpers (the tcat math) ---------------------------------
def ext_off_diff(a: int, b: int) -> int:
    """
    extOffsetDiff(a, b) @0x14600: signed phase difference in the wrapped tick domain.
    Returns a-b folded into (-PHASE_MOD/2, +PHASE_MOD/2]. Used everywhere to ask
    "how far is the output phase ahead of the current device phase?".
    """
    d = (a - b) % PHASE_MOD
    if d > PHASE_MOD // 2:
        d -= PHASE_MOD
    return d


def field_encode(phase: int) -> int:
    """
    Encode a tick phase into a 16-bit AMDTP SYT, exactly as FillFirewireBuffers does
    @0xED86:  LOWORD = (phase % 3072) | ((phase / 3072) & 0xF) << 12.
      top 4 bits  = cycle count (mod 16)
      low 12 bits = cycle offset in 24.576 MHz ticks (0..3071)
    """
    cyc = (phase // TICKS_PER_CYCLE) & 0xF
    sub = phase % TICKS_PER_CYCLE
    return (cyc << 12) | sub


def syt_to_ticks(syt: int) -> int:
    """Inverse of field_encode == tstampToOffsets for a 16-bit SYT (verified by tests)."""
    return ((syt >> 12) & 0xF) * TICKS_PER_CYCLE + (syt & 0xFFF)


# ---- the deadbanded output-phase PLL ---------------------------------------
# The operating lead is NOT a single literal in the binary -- adjustOutputPhase computes it
# from the device period + transfer delay. What the disassembly *fixes* is the window the lead
# must live in: seed = +3072 (1 cycle), accept < 7620, reject > 12287, warn if <= 3071. So the
# steady-state lead is somewhere in [3072, 7620); we model it at the middle (~1.5 cycles).
OPERATING_LEAD = TICKS_PER_CYCLE + TICKS_PER_CYCLE // 2     # 4608 ticks = 1.5 cycles = 9 frames


@dataclasses.dataclass
class PllState:
    target_lead: int = OPERATING_LEAD       # ~1.5 isoch cycles ahead (inside [3072,7620))
    deadband: int = 256                     # a2+392 max-adjust: hold inside, nudge outside
    holds: int = 0
    rebases: int = 0


def adjust_output_phase(dev_phase: int, candidate: int, st: PllState) -> tuple[int, str]:
    """
    Simplified model of adjustOutputPhase @0xC9C2 (the RE'd internals do a modular-division
    slew; the BEHAVIOUR that matters and is reproduced exactly here is the DEADBAND):

      err = (lead - target_lead)        where lead = extOffsetDiff(candidate, dev_phase)
      if |err| <= deadband:  HOLD   -> keep candidate (no correction, just track min/max)
      else:                  REBASE -> snap phase to dev_phase + target_lead

    The point: it does NOT re-anchor every packet. It lets the phase free-run at one cycle
    per callback and only corrects when the device clock has slipped past the deadband. This
    is the "hold-and-nudge / takeStamp-at-wrap" discipline -- the opposite of per-tick
    re-extrapolation, which was our ZTS bug.
    """
    lead = ext_off_diff(candidate, dev_phase)
    err = lead - st.target_lead
    if abs(err) <= st.deadband:
        st.holds += 1
        return candidate % PHASE_MOD, f"hold (|err|={abs(err)} <= deadband {st.deadband})"
    st.rebases += 1
    rebased = (dev_phase + st.target_lead) % PHASE_MOD
    return rebased, f"REBASE (|err|={abs(err)} > deadband {st.deadband}) -> dev+target"


# ---- the send-callback loop (FillFirewireBuffers core) ---------------------
@dataclasses.dataclass
class Stats:
    data: int = 0
    nodata: int = 0
    tight: int = 0
    glitch: int = 0
    leads: list[int] = dataclasses.field(default_factory=list)


def gate_label(lead: int) -> tuple[str, bool]:
    """The two cmp gates @0xEC8A (1DC4h) and @0xEC91 (2FFFh)."""
    if lead < LEAD_ACCEPT:
        tight = lead <= LEAD_TIGHT - 1            # <= 3071
        return ("ACCEPT" + ("  [warn: under 1 cycle]" if tight else ""), True)
    if lead > LEAD_REJECT:
        return ("REJECT  [warn: over 4 cycles] -> NO-DATA", False)
    return ("REJECT -> NO-DATA", False)           # 7620..12287: drop, no extra warn


def run(callbacks: int, drift_ppm: float, narrate_steps: int, target_lead: int,
        deadband: int, glitch_at: int, glitch_jump: int, seed: int) -> None:
    rng = random.Random(seed)
    pll = PllState(target_lead=target_lead, deadband=deadband)
    stats = Stats()

    # Recovered device master clock (dev+1570780), in ticks. A SYT-smoothed clock advances on
    # a DETERMINISTIC cadence -- nominally one isoch cycle (3072 ticks) per send-callback. Real
    # life: the device crystal differs from our local cadence by a few ppm, so the phase ramps
    # slowly (absorbed by the PLL deadband). Discrete discontinuities (re-lock, dropout) are
    # injected explicitly via --glitch-at, not modelled as per-packet noise.
    nominal = TICKS_PER_CYCLE * (1.0 + drift_ppm / 1.0e6)
    acc = 0.0
    cont_tol = TICKS_PER_CYCLE // 2               # only > half a cycle counts as a break
    dev_phase = 0
    out_phase = -1                                # dice_stream+380: -1 until first valid
    ring_idx = (0 + SYT_RING_SEED) & (SYT_RING_SIZE - 1)
    pred_next = -1                                # dice_stream+376 continuity predictor

    print(f"\n=== send-callback loop  (target_lead={target_lead} ticks="
          f"{ticks_to_frames(target_lead):.1f} frames, deadband={deadband}, "
          f"drift={drift_ppm:g} ppm) ===\n")

    for n in range(callbacks):
        acc += nominal + (rng.uniform(-1.0, 1.0))          # sub-tick rounding noise only
        if glitch_at >= 0 and n == glitch_at:              # injected discontinuity
            acc += glitch_jump
        dev_phase = round(acc) % PHASE_MOD

        # Continuity check (dice_stream+376): predict dev advanced exactly one cycle.
        cont = "first"
        if pred_next >= 0:
            derr = ext_off_diff(dev_phase, pred_next)
            if abs(derr) <= cont_tol:
                cont = "ok" if derr == 0 else f"drift derr={derr:+d}"
            elif abs(derr) == TICKS_PER_CYCLE:    # exactly 1 cycle slip -> tolerated (<=2x)
                cont = f"1-cycle slip (tol, derr={derr:+d})"
            else:
                cont = f"GLITCH derr={derr:+d} -> resync"
                stats.glitch += 1
                out_phase = -1                    # force re-seed (drops continuity)
        pred_next = (dev_phase + TICKS_PER_CYCLE) % PHASE_MOD

        # Candidate output phase: carried-over running phase, else seed = dev + ONE cycle.
        if out_phase >= 0:
            candidate = out_phase
            seed_src = "carry (dice_stream+380)"
        else:
            candidate = (dev_phase + TICKS_PER_CYCLE) % PHASE_MOD
            seed_src = "seed = dev + 3072 (1 cycle)"

        adj_phase, pll_note = adjust_output_phase(dev_phase, candidate, pll)
        lead = ext_off_diff(adj_phase, dev_phase)
        label, is_data = gate_label(lead)
        stats.leads.append(lead)

        if is_data:
            stats.data += 1
            if "under 1 cycle" in label:
                stats.tight += 1
            syt = field_encode(adj_phase)
            # Advance running phase by one SYT-ring cadence entry (here: nominal 1 cycle).
            out_phase = (adj_phase + TICKS_PER_CYCLE) % PHASE_MOD
            ring_idx = (ring_idx + 1) & (SYT_RING_SIZE - 1)
        else:
            stats.nodata += 1
            syt = 0xFFFF                          # NO-DATA marker
            out_phase = -1                        # phase=-1: re-seed next callback

        if n < narrate_steps:
            print(f"callback {n:>3}  devPhase={dev_phase:>9} "
                  f"(cyc={dev_phase // TICKS_PER_CYCLE % 16} sub={dev_phase % TICKS_PER_CYCLE})  "
                  f"continuity={cont}")
            print(f"            candidate: {seed_src} = {candidate}")
            print(f"            PLL:       {pll_note} -> outPhase={adj_phase}")
            print(f"            lead = extOffsetDiff(out,dev) = {lead} ticks "
                  f"= {lead / TICKS_PER_CYCLE:.2f} cyc = {ticks_to_frames(lead):.1f} frames")
            print(f"            gate [{LEAD_TIGHT}..{LEAD_ACCEPT})ok / >{LEAD_REJECT} reject: "
                  f"{label}")
            if is_data:
                print(f"            SYT  = field_encode({adj_phase}) = 0x{syt:04X} "
                      f"(cyc={(syt >> 12) & 0xF} sub={syt & 0xFFF})   ring_idx now {ring_idx}")
            print()

    _summary(stats, pll, callbacks)


def _summary(stats: Stats, pll: PllState, callbacks: int) -> None:
    leads = stats.leads
    lo, med, hi = min(leads), statistics.median(leads), max(leads)
    print("=== summary ===")
    print(f"callbacks        : {callbacks}")
    print(f"DATA / NO-DATA   : {stats.data} / {stats.nodata}")
    print(f"PLL holds/rebases: {pll.holds} / {pll.rebases}   "
          f"(deadband keeps it from re-anchoring every packet)")
    print(f"continuity glitch: {stats.glitch}   tight-lead warns: {stats.tight}")
    print(f"output lead ticks: min={lo} med={med:.0f} max={hi}")
    print(f"output lead FRAMES: min={ticks_to_frames(lo):.1f} "
          f"med={ticks_to_frames(med):.1f} max={ticks_to_frames(hi):.1f}  "
          f"(bounded ~1..4 cycles by design)")
    print()
    print("CONTRAST WITH ASFW DICE:")
    print(f"  Saffire device-facing lead stays {ticks_to_frames(lo):.0f}..{ticks_to_frames(hi):.0f} "
          f"frames because phase is re-derived per callback from the recovered device clock.")
    print(f"  ASFW txOutputOffsetFrames_=5120 frames exists ONLY to cancel a free-running")
    print(f"  timeline that drifts ~4840 frames from CoreAudio's write head -- a gap Saffire")
    print(f"  never creates. Same 3072/4096 units; ~340x larger offset = architectural, not timing.")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--callbacks", type=int, default=2000, help="send-callbacks to simulate")
    p.add_argument("--steps", type=int, default=8, help="callbacks to narrate in full")
    p.add_argument("--drift-ppm", type=float, default=0.0,
                   help="device-vs-local clock offset in ppm (ramps phase; PLL absorbs it)")
    p.add_argument("--target-lead", type=int, default=OPERATING_LEAD,
                   help=f"PLL operating lead in ticks (default {OPERATING_LEAD} = 1.5 cyc; "
                        f"verified window is [3072,7620). Try 3072 to see tight-lead warns.)")
    p.add_argument("--deadband", type=int, default=256, help="PLL max-adjust deadband (ticks)")
    p.add_argument("--glitch-at", type=int, default=-1,
                   help="callback index to inject a discontinuity (default: none)")
    p.add_argument("--glitch-jump", type=int, default=5000,
                   help="tick jump for the injected glitch (>half cycle triggers resync)")
    p.add_argument("--seed", type=int, default=1, help="RNG seed")
    args = p.parse_args()

    print("=== Saffire.kext output-phase model (RE'd FillFirewireBuffers/adjustOutputPhase) ===")
    print(f"TICKS_PER_CYCLE={TICKS_PER_CYCLE}  TICKS_PER_FRAME={TICKS_PER_FRAME}  "
          f"FRAMES_PER_CYCLE={FRAMES_PER_CYCLE}")
    print(f"PHASE_MOD=0x{PHASE_MOD:X} ({PHASE_MOD} = 4096*48000 = 64000 cyc = 8 s)")
    print(f"lead gates: tight<= {LEAD_TIGHT - 1}  accept< {LEAD_ACCEPT} "
          f"({ticks_to_frames(LEAD_ACCEPT):.1f} frames)  reject> {LEAD_REJECT} "
          f"({ticks_to_frames(LEAD_REJECT):.1f} frames)")
    print(f"SYT cadence ring: {SYT_RING_SIZE} entries, seeded at midpoint {SYT_RING_SEED}")

    # sanity: verified helper values from TimingUtilsTests
    assert syt_to_ticks(0x1000) == 3072
    assert syt_to_ticks(0x1ABC) == 3072 + 0xABC
    assert field_encode(3072) == 0x1000
    print("helper self-check: syt_to_ticks/field_encode match TimingUtilsTests values  [OK]")

    run(args.callbacks, args.drift_ppm, args.steps, args.target_lead,
        args.deadband, args.glitch_at, args.glitch_jump, args.seed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
