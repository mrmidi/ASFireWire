#!/usr/bin/env python3
"""
TX phase-loop event model -- the CONTRACT for the TxPhaseEvent/TxPhaseDecision rewrite.

This is the fixture generator for the TDD plan: it implements, in Python, the event
classification the C++ TxOutputPhaseLoop rewrite must match cycle-for-cycle, then
emits JSON rows that the gtest suite consumes directly. Python and C++ cannot drift
apart silently -- if the model changes, regenerate the fixtures and the tests will
tell you what broke.

THE SPLIT THIS MODEL ENFORCES (the bug we are fixing)
------------------------------------------------------
The current loop conflates three independent mechanisms into one "rebased" bit:

  1. Timestamp continuity  -- is the recovered device phase advancing the way we
                              expect, given how many isoch cycles actually elapsed?
  2. Output lead gate      -- given the current phase relationship, do we have room
                              to ship a DATA packet, or must this cycle be NO-DATA?
  3. Source readiness      -- does the caller even have audio to send this cycle?

Only (1) failing for real (a genuine device-clock jump) should reset the phase map
and re-arm the SYT anchor. (2) and (3) are normal, recurring, steady-state outcomes
that must NOT cost us our anchors -- that is exactly what produced the
"TX PHASE RESET reason=Glitch" spam: every lead-gate NO-DATA was being treated as a
discontinuity.

TWO INDEPENDENT CLASSIFICATIONS PER CYCLE (not one combined enum)
------------------------------------------------------------------
This model reports a `continuityEvent` AND an `emissionEvent` per cycle -- two
genuinely separate readings, not phases of one combined decision:

  continuityEvent: is the recovered device phase advancing the way we expect, and
  which device-phase value should everything below measure against?

  emissionEvent: is the OUTPUT lead currently in the safe operating range? This is
  now a pure HEALTH/DIAGNOSTIC read -- a leading indicator of clicks/underrun --
  decoupled from the cadence decision itself.

WHO DECIDES THE WIRE -- AND WHY THE LOOP DOES NOT
--------------------------------------------------
Two earlier drafts both assumed the loop's lead gate PRODUCES the DATA:NO-DATA
cadence. Both were wrong, for the same root reason: the wire cadence at 48 kHz
blocking mode is not a free choice -- it is a spec-mandated CIP/DBC sequencing
pattern (3 DATA : 1 NO-DATA, specific data-block-count progression) that the DICE
device expects bit-for-bit, and that pattern is already generated deterministically
by PacketAssembler's BlockingCadence48k. A SECOND independent ~3:1 generator (the
lead gate, driven by recovered device phase rather than a fixed position counter)
would inevitably diverge from it on individual cycles -- corrupting DBC continuity
on the wire, or corrupting this loop's own phase tracking (whichever one loses).

So: the cadence layer (PacketAssembler / its caller) is authoritative for whether
DATA goes on the wire. It tells the loop what it decided via `dataCandidate` --
"this is what is actually being shipped this cycle" -- and the loop:
  - mirrors that into `emitData` (so callers can read the loop's view of "did we
    ship," but it is never a second opinion -- always `dataCandidate` when the
    clock is valid),
  - advances `outputPhaseTicks` in lockstep with `dataCandidate` (the phase MUST
    track what actually happened on the wire, not what the loop would have chosen),
  - and SEPARATELY measures the lead and reports whether it sits in the safe
    accept range -- regardless of whether this cycle happened to carry data. A
    "LeadGateNoData" reading on a cycle that DID ship DATA is exactly the useful
    signal: "we just shipped while our lead margin was thin -- watch this."

Earlier drafts also had two narrower bugs worth recording (both moot now that the
lead gate no longer drives `emitData`, but they explain why a naive "run the lead
gate only when continuity is nominal" shape is wrong even as a health read):
  - InitialSeed seeds outputPhase = devicePhase + OPERATING_LEAD, i.e. a perfectly
    healthy ~4608-tick lead -- the health read must see that immediately, not be
    skipped/short-circuited for the seed cycle.
  - OneCycleSlipCompensated substitutes the predicted device phase and continues;
    the health read should measure against that corrected value, not be suppressed.

  continuityEvent (mutually exclusive, priority order):
    ClockInvalid              recovered clock not usable this cycle
    InitialSeed               first usable cycle (or first after clock recovery) --
                              seeds outputPhase = devicePhase + OPERATING_LEAD
    TimingDiscontinuity       device phase deviates from the cycle-delta-corrected
                              prediction by more than one cycle, or a one-cycle slip
                              persists past the tolerance -- REAL clock jump; reseeds
    OneCycleSlipCompensated   device phase is exactly one cycle off prediction, within
                              tolerance -- absorb it (use the predicted phase), DO NOT
                              reset anything
    Nominal                   device phase advanced exactly as predicted

  emissionEvent (mutually exclusive; a HEALTH read, computed whenever the clock is
  valid, using the continuity-corrected device phase -- independent of dataCandidate):
    ClockInvalidNoData        clock invalid -> no phase to measure against
    LeadAcceptedData          lead sits in the safe operating range this cycle
    LeadGateNoData            lead has drifted outside the safe range -- WARNING;
                              this can legitimately co-occur with emitData == true
                              ("shipping while thin") or == false ("withholding
                              while loose") -- it reports lead health, not cadence

Only continuityEvent in {InitialSeed, TimingDiscontinuity} sets
resetPhaseMap/armTransmitAnchor. `emitData` is always exactly `dataCandidate` when
the clock is valid (and `False` when it is not) -- the loop never overrides the
cadence layer's decision, it only tracks it and grades the lead against it.

THE SPARSE-CALLER FIX
---------------------
The old detector assumed "+1 cycle since last call". If the caller is not invoked
exactly once per isoch cycle (e.g. NO-DATA cycles are skipped upstream, or the
transmit-cycle counter free-runs faster than callbacks), that assumption alone
manufactures false discontinuities. This model takes `cycleDelta` -- the number of
isoch cycles the caller measured since its previous call (from request.transmitCycle)
-- and predicts `lastDevicePhase + cycleDelta * TICKS_PER_CYCLE`. Calling every 8
cycles with matching phase deltas must be indistinguishable from calling every cycle.

Usage:
  python3 tools/debug/tx_phase_loop_model.py --scenario steady48 --narrate 12
  python3 tools/debug/tx_phase_loop_model.py --scenario steady48 --json > tests/fixtures/tx_phase_steady48.json
  python3 tools/debug/tx_phase_loop_model.py --scenario sparse-calls --json > ...
  python3 tools/debug/tx_phase_loop_model.py --scenario wrap --json > ...
  python3 tools/debug/tx_phase_loop_model.py --scenario one-cycle-slip --json > ...
  python3 tools/debug/tx_phase_loop_model.py --scenario lead-gate --json > ...
  python3 tools/debug/tx_phase_loop_model.py --scenario clock-invalid --json > ...
  python3 tools/debug/tx_phase_loop_model.py --scenario glitch --json > ...
"""

from __future__ import annotations

import argparse
import dataclasses
import enum
import json

# ---- ASFW::Timing / TxOutputPhaseLoop constants -----------------------------
TICKS_PER_CYCLE = 3072                  # 24.576 MHz / 8000
TICKS_PER_FRAME = 512                   # 24.576 MHz / 48000
ONE_SECOND_TICKS = 24_576_000           # device-phase domain wraps here
OPERATING_LEAD = 4608                   # 1.5 cycles -- seed/reseed target
LEAD_TIGHT = TICKS_PER_CYCLE            # < 1 cycle of lead -> near-underrun warning
LEAD_ACCEPT = 7620                      # lead < this -> ship DATA
LEAD_REJECT = 12287                     # lead > this -> hard NO-DATA (diagnostic only;
                                        # the accept check already gates at 7620)
DEVICE_SUBCYCLE_TICKS = 0x0B0           # device's constant recovered sub-cycle (Saffire Pro24)

# Continuity-check tuning. CONTINUITY_TOLERANCE absorbs measurement jitter around an
# exact prediction; SLIP_TOLERANCE_LIMIT bounds how many *consecutive* one-cycle slips
# we absorb before calling it a real discontinuity (Saffire: "slipCounter <= 2").
CONTINUITY_TOLERANCE = 96
SLIP_TOLERANCE_LIMIT = 2


def ext_offset_diff_1s(a: int, b: int) -> int:
    """Signed shortest-path delta (a - b) in the 1-second (8000-cycle) domain."""
    d = (a - b) % ONE_SECOND_TICKS
    if d > ONE_SECOND_TICKS // 2:
        d -= ONE_SECOND_TICKS
    return d


def encode_offset_ticks_to_syt(offset_ticks: int) -> int:
    """[cycle4:offset12], device sub-cycle grafted on -- mirrors SYTGenerator/Saffire."""
    graft = offset_ticks - (offset_ticks % TICKS_PER_CYCLE) + DEVICE_SUBCYCLE_TICKS
    field = graft % (16 * TICKS_PER_CYCLE)
    return (((field // TICKS_PER_CYCLE) & 0x0F) << 12) | (field % TICKS_PER_CYCLE)


class TxContinuityEvent(enum.Enum):
    CLOCK_INVALID = "ClockInvalid"
    INITIAL_SEED = "InitialSeed"
    NOMINAL = "Nominal"
    ONE_CYCLE_SLIP_COMPENSATED = "OneCycleSlipCompensated"
    TIMING_DISCONTINUITY = "TimingDiscontinuity"


class TxEmissionEvent(enum.Enum):
    """A lead-HEALTH read, not a cadence decision -- see module docstring. Computed
    whenever the clock is valid, independent of dataCandidate/emitData."""
    CLOCK_INVALID_NO_DATA = "ClockInvalidNoData"
    LEAD_ACCEPTED_DATA = "LeadAcceptedData"   # lead sits in the safe operating range
    LEAD_GATE_NO_DATA = "LeadGateNoData"      # lead has drifted outside it -- WARNING


@dataclasses.dataclass
class TxPhaseDecision:
    emitData: bool
    syt: int
    outputPhaseTicks: int
    leadTicks: int
    resetPhaseMap: bool
    armTransmitAnchor: bool
    continuityEvent: TxContinuityEvent
    emissionEvent: TxEmissionEvent

    def to_row(self, cycle: int, devicePhaseTicks: int, cycleDelta: int,
               recoveredClockValid: bool, dataCandidate: bool) -> dict:
        return {
            "cycle": cycle,
            "inputPhase": devicePhaseTicks,
            "cycleDelta": cycleDelta,
            "valid": recoveredClockValid,
            "dataCandidate": dataCandidate,
            "expectedContinuityEvent": self.continuityEvent.value,
            "expectedEmissionEvent": self.emissionEvent.value,
            "expectedEmitData": self.emitData,
            "expectedSyt": self.syt,
            "expectedOutputPhase": self.outputPhaseTicks,
            "expectedLeadTicks": self.leadTicks,
            "expectedResetMap": self.resetPhaseMap,
            "expectedArmAnchor": self.armTransmitAnchor,
        }


class TxPhaseLoopModel:
    """Reference model for the rewritten TxOutputPhaseLoop::ProcessCycle."""

    def __init__(self) -> None:
        self.outputPhaseTicks = 0
        self.lastDevicePhase = -1     # -1 == "no prior sample" (post-seed / post-reset)
        self.slipCounter = 0
        self.seeded = False
        self.contDiag = {e: 0 for e in TxContinuityEvent}
        self.emitDiag = {e: 0 for e in TxEmissionEvent}

    def _seed(self, devicePhaseTicks: int) -> None:
        self.outputPhaseTicks = devicePhaseTicks + OPERATING_LEAD
        self.seeded = True
        self.slipCounter = 0

    def process(self, devicePhaseTicks: int, cycleDelta: int, recoveredClockValid: bool,
                dataCandidate: bool, framesPerPacket: int) -> TxPhaseDecision:
        # 0. Clock not usable: hold everything, do not touch anchors. The next valid
        #    sample re-seeds fresh (InitialSeed), exactly like start-of-stream. There
        #    is no device phase to gate against, so emission is forced NO-DATA --
        #    this is the one legitimate case where continuity and emission collapse
        #    into a single combined outcome.
        if not recoveredClockValid:
            self.seeded = False
            self.lastDevicePhase = -1
            self.slipCounter = 0
            self.contDiag[TxContinuityEvent.CLOCK_INVALID] += 1
            self.emitDiag[TxEmissionEvent.CLOCK_INVALID_NO_DATA] += 1
            return TxPhaseDecision(False, 0xFFFF, self.outputPhaseTicks, 0, False, False,
                                   TxContinuityEvent.CLOCK_INVALID, TxEmissionEvent.CLOCK_INVALID_NO_DATA)

        resetMap = False
        armAnchor = False
        usedDevicePhase = devicePhaseTicks

        # 1. Timestamp continuity -- classifies *which* device-phase value the lead
        #    gate below measures against. Only InitialSeed / TimingDiscontinuity may
        #    reset the map and re-arm the anchor; this is the ONLY thing continuity
        #    decides. It does NOT decide whether we emit DATA this cycle -- that is
        #    entirely the lead gate's job, run unconditionally in step 2.
        if not self.seeded:
            self._seed(devicePhaseTicks)
            continuityEvent = TxContinuityEvent.INITIAL_SEED
            resetMap = True
            armAnchor = True
        else:
            expectedNext = (self.lastDevicePhase + cycleDelta * TICKS_PER_CYCLE) % ONE_SECOND_TICKS
            diff = ext_offset_diff_1s(devicePhaseTicks, expectedNext)
            if abs(diff) <= CONTINUITY_TOLERANCE:
                continuityEvent = TxContinuityEvent.NOMINAL
                self.slipCounter = 0
            elif (abs(abs(diff) - TICKS_PER_CYCLE) <= CONTINUITY_TOLERANCE
                  and self.slipCounter < SLIP_TOLERANCE_LIMIT):
                continuityEvent = TxContinuityEvent.ONE_CYCLE_SLIP_COMPENSATED
                usedDevicePhase = expectedNext   # absorb: trust the prediction, not the jitter
                self.slipCounter += 1
            else:
                self._seed(devicePhaseTicks)
                continuityEvent = TxContinuityEvent.TIMING_DISCONTINUITY
                resetMap = True
                armAnchor = True

        self.lastDevicePhase = devicePhaseTicks

        # 2. Emission -- the cadence layer (caller) is AUTHORITATIVE for whether
        #    DATA goes on the wire (see module docstring for why a second
        #    independent ~3:1 generator here would be wrong); `dataCandidate`
        #    carries that decision in. The loop's job is to TRACK reality, not
        #    decide it: emitData mirrors dataCandidate exactly, and the phase
        #    accumulator advances in lockstep with what was actually shipped.
        #
        #    Separately -- and unconditionally, regardless of dataCandidate -- the
        #    loop grades the CURRENT lead against the safe operating range. This is
        #    a pure health/diagnostic read: "LeadGateNoData while emitData==True"
        #    means "we just shipped while thin" (early click warning); "...while
        #    emitData==False" means "we're idle while the lead is loose." Neither
        #    classification touches resetPhaseMap/armTransmitAnchor or overrides
        #    the cadence decision -- it only measures and reports.
        lead = ext_offset_diff_1s(self.outputPhaseTicks, usedDevicePhase)
        emissionEvent = (TxEmissionEvent.LEAD_ACCEPTED_DATA if lead < LEAD_ACCEPT
                         else TxEmissionEvent.LEAD_GATE_NO_DATA)

        emitData = dataCandidate
        present = self.outputPhaseTicks
        syt = encode_offset_ticks_to_syt(present) if emitData else 0xFFFF

        if emitData:
            self.outputPhaseTicks += framesPerPacket * TICKS_PER_FRAME

        self.contDiag[continuityEvent] += 1
        self.emitDiag[emissionEvent] += 1
        return TxPhaseDecision(emitData, syt, present, lead, resetMap, armAnchor,
                               continuityEvent, emissionEvent)


# ============================================================================
# Scenarios -- each yields (devicePhaseTicks, cycleDelta, recoveredClockValid,
# dataCandidate, framesPerPacket) tuples to feed the model, one per processed cycle.
#
# `dataCandidate` now DRIVES `emitData` (the cadence layer is authoritative -- see
# module docstring), so scenarios feed a realistic 3:1 pattern through it rather
# than a constant True. _natural_cadence mirrors PacketAssembler's BlockingCadence48k
# DATA:NO-DATA shape at 48 kHz / 8-frame packets (3*8 + 1*0 == 4*6 frames/cycle); the
# model does not need PacketAssembler's exact DBC sequencing, only a realistic
# dataCandidate stream to drive emitData/phase-advance through.
# ============================================================================

def _natural_cadence(i: int) -> bool:
    return (i % 4) != 3


def _steady48(n: int):
    dev = 0
    for i in range(n):
        yield dev, 1, True, _natural_cadence(i), 8
        dev = (dev + TICKS_PER_CYCLE) % ONE_SECOND_TICKS


def _sparse_calls(n: int, stride: int = 8):
    dev = 0
    for i in range(n):
        yield dev, stride, True, _natural_cadence(i), 8
        dev = (dev + stride * TICKS_PER_CYCLE) % ONE_SECOND_TICKS


def _wrap(n: int):
    dev = ONE_SECOND_TICKS - 10 * TICKS_PER_CYCLE
    for i in range(n):
        yield dev, 1, True, _natural_cadence(i), 8
        dev = (dev + TICKS_PER_CYCLE) % ONE_SECOND_TICKS   # wraps mid-run


def _one_cycle_slip(n: int, slip_at=(20, 21, 50)):
    """Inject isolated +1-cycle jumps (device skipped a cycle) at chosen indices,
    each separated by enough normal cycles for the slip counter to reset, except
    for one back-to-back pair (20, 21) used to exercise the slipCounter<=2 bound."""
    dev = 0
    extra = 0
    for i in range(n):
        if i in slip_at:
            extra += TICKS_PER_CYCLE
        yield (dev + extra) % ONE_SECOND_TICKS, 1, True, _natural_cadence(i), 8
        dev = (dev + TICKS_PER_CYCLE) % ONE_SECOND_TICKS


def _lead_gate(n: int):
    """Isolate the lead-HEALTH read (not a cadence stress -- emission no longer
    decides cadence). Feed the natural 3:1 dataCandidate pattern but ship 16-frame
    packets (2x the natural 8): outputPhase then advances ~2x faster than the
    device phase the cadence pattern was sized for, so the lead grows roughly
    without bound and LEAD_GATE_NO_DATA dominates -- a realistic "frame-count
    misconfigured relative to the wire cadence" fault, and proof the health read
    fires/recurs freely without ever touching resetPhaseMap/armTransmitAnchor."""
    dev = 0
    for i in range(n):
        yield dev, 1, True, _natural_cadence(i), 16
        dev = (dev + TICKS_PER_CYCLE) % ONE_SECOND_TICKS


def _clock_invalid(n: int, invalid_until: int = 30):
    dev = 0
    for i in range(n):
        valid = i >= invalid_until
        yield dev, 1, valid, _natural_cadence(i), 8
        dev = (dev + TICKS_PER_CYCLE) % ONE_SECOND_TICKS


def _glitch(n: int, glitch_at: int = 40, glitch_jump: int = 50_000):
    dev = 0
    extra = 0
    for i in range(n):
        if i == glitch_at:
            extra += glitch_jump
        yield (dev + extra) % ONE_SECOND_TICKS, 1, True, _natural_cadence(i), 8
        dev = (dev + TICKS_PER_CYCLE) % ONE_SECOND_TICKS


SCENARIOS = {
    "steady48":      lambda n: _steady48(n),
    "sparse-calls":  lambda n: _sparse_calls(n),
    "wrap":          lambda n: _wrap(n),
    "one-cycle-slip": lambda n: _one_cycle_slip(n),
    "lead-gate":     lambda n: _lead_gate(n),
    "clock-invalid": lambda n: _clock_invalid(n),
    "glitch":        lambda n: _glitch(n),
}

DEFAULT_COUNTS = {
    "steady48": 8000,
    "sparse-calls": 400,
    "wrap": 40,
    "one-cycle-slip": 80,
    "lead-gate": 200,
    "clock-invalid": 80,
    "glitch": 100,
}


def run(scenario: str, count: int | None):
    n = count if count is not None else DEFAULT_COUNTS[scenario]
    model = TxPhaseLoopModel()
    rows = []
    for cycle, (dev, delta, valid, candidate, fpp) in enumerate(SCENARIOS[scenario](n)):
        decision = model.process(dev, delta, valid, candidate, fpp)
        rows.append(decision.to_row(cycle, dev, delta, valid, candidate))
    return model, rows


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--scenario", choices=list(SCENARIOS), default="steady48")
    p.add_argument("--count", type=int, default=None)
    p.add_argument("--json", action="store_true", help="emit fixture rows as a JSON array")
    p.add_argument("--narrate", type=int, default=0, help="print the first N rows")
    args = p.parse_args()

    model, rows = run(args.scenario, args.count)

    if args.json:
        print(json.dumps(rows, indent=1))
        return 0

    print(f"=== TxPhaseLoop event model: scenario={args.scenario} cycles={len(rows)} ===")
    for row in rows[:args.narrate]:
        print(f"cycle={row['cycle']:5} phase={row['inputPhase']:9} d={row['cycleDelta']:3} "
              f"valid={int(row['valid'])} cand={int(row['dataCandidate'])}  "
              f"-> cont={row['expectedContinuityEvent']:22} emit={row['expectedEmissionEvent']:18} "
              f"data={int(row['expectedEmitData'])} syt=0x{row['expectedSyt']:04x} "
              f"lead={row['expectedLeadTicks']:6} "
              f"resetMap={int(row['expectedResetMap'])} arm={int(row['expectedArmAnchor'])}")

    print("\n--- continuity event counts ---")
    for e in TxContinuityEvent:
        if model.contDiag[e]:
            print(f"  {e.value:26} {model.contDiag[e]}")
    print("\n--- emission event counts ---")
    for e in TxEmissionEvent:
        if model.emitDiag[e]:
            print(f"  {e.value:26} {model.emitDiag[e]}")

    data = sum(1 for r in rows if r["expectedEmitData"])
    nodata = len(rows) - data
    print(f"\nDATA={data} NO-DATA={nodata} ratio~{(data / nodata) if nodata else float('inf'):.2f}:1 "
          f"(cadence is the caller's decision now -- dataCandidate drives this directly; "
          f"the model's scenarios feed a natural 3:1 pattern through it)")
    resets = sum(1 for r in rows if r["expectedResetMap"])
    print(f"resetPhaseMap fired {resets} time(s) "
          f"(InitialSeed={model.contDiag[TxContinuityEvent.INITIAL_SEED]} "
          f"TimingDiscontinuity={model.contDiag[TxContinuityEvent.TIMING_DISCONTINUITY]})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
