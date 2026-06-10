// TxOutputPhaseLoop.hpp
// ASFW - Phase-driven TX output timing derived from the Saffire reference model.

#pragma once

#include <cstdint>
#include "../../../AudioWire/AMDTP/TimingUtils.hpp"

namespace ASFW::AudioEngine::DirectIsoch {

/// Free-running TX output-phase accumulator + lead gate + device-drift monitor.
///
/// Design (see tools/debug/tx_phase_loop_model.py for the contract -- it is the
/// authoritative reference model; the JSON it emits under tests/fixtures/ pins the
/// per-cycle event sequence this loop must reproduce):
///
///   Each cycle yields TWO independent classifications -- continuity and emission --
///   reported separately so a recurring outcome of one never masquerades as an
///   instability signal of the other, AND so they can freely co-occur (an early
///   draft folded them into one enum gated on "continuity nominal", which silently
///   forced kInitialSeed and kOneCycleSlipCompensated cycles to NO-DATA -- wrong:
///   Saffire seeds/absorbs-slip and ships DATA in the very same callback; suppressing
///   it manufactures an artificial sub-one-cycle lead one beat later).
///
///   1. Continuity (ContinuityEvent) -- the ONLY classification that may declare a
///      real discontinuity and choose which device-phase value emission measures
///      against. The recovered device phase is expected to advance by `cycleDelta`
///      cycles since the previous call -- NOT a hard-coded one cycle, because the
///      caller is not guaranteed to be invoked exactly once per isoch cycle. Within
///      tolerance of the prediction -> kNominal; exactly one cycle off (within
///      tolerance, bounded consecutive count) -> kOneCycleSlipCompensated (absorb:
///      measure against the trusted prediction, not the raw jitter); anything else
///      -> kTimingDiscontinuity (genuine device-clock jump; reseeds).
///
///   2. Emission (EmissionEvent) -- a pure lead-HEALTH read, NOT a cadence decision.
///      WHO DECIDES THE WIRE -- AND WHY THE LOOP DOES NOT: PacketAssembler's
///      BlockingCadence48k pattern is the spec-mandated, device-validated CIP/DBC
///      sequencing the DICE box expects bit-for-bit; it is authoritative for wire
///      DATA/NO-DATA, and `dataCandidate` carries that decision in here already
///      made. This loop cannot independently re-decide cadence without corrupting
///      either DBC continuity (the assembler's problem) or its own phase tracking
///      (this loop's problem) -- it must TRACK what was decided, not decide it.
///      So: `emitData` mirrors `dataCandidate` exactly, and the output phase
///      advances in lockstep with `dataCandidate` (never with a derived verdict).
///      EmissionEvent is then computed UNCONDITIONALLY whenever the clock is valid,
///      independent of `dataCandidate` and of which continuity branch fired: it
///      reports whether the lead sits in the safe operating range this cycle
///      (kLeadAcceptedData) or has drifted outside it (kLeadGateNoData) -- a pure
///      measurement that can co-occur with either emitData value ("shipping while
///      thin" and "idle while loose" are both meaningful warning signals, logged
///      and counted, never overriding the assembler's cadence). At 48 kHz / 8-frame
///      packets the assembler's natural 3:1 DATA:NO-DATA cadence keeps the lead
///      oscillating inside the accept window by construction; kLeadGateNoData
///      recurring there is healthy telemetry, not an error.
///
///   Only ContinuityEvent::kInitialSeed and kTimingDiscontinuity set
///   resetPhaseMap/armTransmitAnchor. Every other continuity x emission combination
///   -- including kInitialSeed+kLeadGateNoData and
///   kOneCycleSlipCompensated+kLeadGateNoData -- is normal, recurring, steady-state
///   traffic that must be cheap and silent. Treating recurring outcomes as
///   instability is exactly the bug this replaces ("TX PHASE RESET reason=Glitch"
///   firing on ordinary lead-health NO-DATA reads).
///
///   `outputPhaseTicks_` is an UNWRAPPED, monotonically increasing int64 in the
///   24.576 MHz tick domain. OutputPhaseToAudioMap maps it to absolute audio frames
///   by plain subtraction, so it must never wrap.
///
/// SYT generation lives in Encoding::SYTGenerator, not here.
class TxOutputPhaseLoop {
public:
    static constexpr int64_t kTicksPerCycle  = ASFW::Timing::kTicksPerCycle;   // 3072
    static constexpr int64_t kTicksPerFrame  = ASFW::Timing::kTicksPerSample48k; // 512
    static constexpr int64_t kFramesPerCycle = kTicksPerCycle / kTicksPerFrame;  // 6
    static constexpr int64_t kOneSecondTicks = ASFW::Timing::kTicksPerSecond;    // 24'576'000
    /// `transmitCycle` is the 13-bit OHCI bus-cycle field, 0..kCyclesPerSecond-1,
    /// that wraps every second. The continuity predictor relies on the
    /// mod-kCyclesPerSecond delta (NOT a monotonic uint32 subtraction -- that
    /// would treat the 7999->0 wrap as a +~2^32 jump and manufacture a fake
    /// kTimingDiscontinuity once per second).
    static constexpr uint32_t kCyclesPerSecond = ASFW::Timing::kCyclesPerSecond; // 8000

    /// Operating lead: how far the output phase sits ahead of the recovered device
    /// phase when (re)seeded. 1.5 cycles = 9 frames, mid-window.
    static constexpr int64_t kOperatingLead   = 4608;
    /// Below one cycle of lead is a near-underrun warning (advisory only).
    static constexpr int64_t kLeadTightTicks  = kTicksPerCycle;   // 3072
    /// Lead gate: below this there is room to ship DATA; at/above it, hold (emits SYT=0xFFFF).
    /// Actual Saffire hardware packet rejection begins here.
    static constexpr int64_t kLeadAcceptTicks = 7620;             // ~2.48 cycles
    /// Advisory-only ceiling for the "lead has drifted very far" warning.
    /// Saffire's 12287 check is purely log-escalation (louder warning), not a separate rejection gate.
    static constexpr int64_t kLeadRejectTicks = 12287;            // ~4 cycles

    /// Continuity-check tuning. The tolerance absorbs measurement jitter around an
    /// exact prediction; the slip limit bounds how many *consecutive* one-cycle
    /// slips are absorbed before a persistent slip is treated as a real
    /// discontinuity (mirrors the Saffire "slipCounter <= 2" tolerance).
    static constexpr int64_t kContinuityToleranceTicks = 96;
    static constexpr uint32_t kSlipToleranceLimit = 2;

    /// Continuity classification -- exactly one fires per ProcessCycle call. Chooses
    /// which device-phase value emission measures against, and is the ONLY thing
    /// that may set resetPhaseMap/armTransmitAnchor (kInitialSeed, kTimingDiscontinuity).
    enum class ContinuityEvent : uint8_t {
        kClockInvalid = 0,          // recovered clock not usable this cycle
        kInitialSeed,               // first usable cycle, or first since clock recovery
        kNominal,                   // device phase advanced exactly as predicted
        kOneCycleSlipCompensated,   // device phase exactly one cycle off prediction; absorbed
        kTimingDiscontinuity,       // device phase jumped -- the only "real" glitch; reseeds
    };

    /// Emission classification -- a pure lead-HEALTH read, exactly one fires per
    /// ProcessCycle call, computed UNCONDITIONALLY whenever the clock is valid,
    /// independent of `dataCandidate` and of which ContinuityEvent fired. It does
    /// NOT decide the wire cadence (see class docs, "WHO DECIDES THE WIRE"); it can
    /// freely co-occur with either emitData value.
    enum class EmissionEvent : uint8_t {
        kClockInvalidNoData = 0,    // clock invalid -- no phase to measure lead against
        kLeadAcceptedData,          // lead sits in the safe operating range
        kLeadGateNoData,            // lead has drifted outside it -- WARNING
    };

    [[nodiscard]] static constexpr const char* ContinuityEventName(ContinuityEvent event) noexcept {
        switch (event) {
            case ContinuityEvent::kClockInvalid:            return "ClockInvalid";
            case ContinuityEvent::kInitialSeed:             return "InitialSeed";
            case ContinuityEvent::kNominal:                 return "Nominal";
            case ContinuityEvent::kOneCycleSlipCompensated: return "OneCycleSlipCompensated";
            case ContinuityEvent::kTimingDiscontinuity:     return "TimingDiscontinuity";
        }
        return "ClockInvalid";
    }

    [[nodiscard]] static constexpr const char* EmissionEventName(EmissionEvent event) noexcept {
        switch (event) {
            case EmissionEvent::kClockInvalidNoData: return "ClockInvalidNoData";
            case EmissionEvent::kLeadAcceptedData:   return "LeadAcceptedData";
            case EmissionEvent::kLeadGateNoData:     return "LeadGateNoData";
        }
        return "ClockInvalidNoData";
    }

    struct Decision {
        bool            emitData{false};          // ship a DATA packet this cycle?
        int64_t         outputPhaseTicks{0};      // unwrapped phase for THIS packet (pre-advance)
        int64_t         leadTicks{0};             // advisory lead vs device, 1-second domain
        bool            resetPhaseMap{false};     // caller must reset OutputPhaseToAudioMap
        bool            armTransmitAnchor{false}; // caller must re-arm the SYT transmit-cycle anchor
        ContinuityEvent continuityEvent{ContinuityEvent::kClockInvalid};
        EmissionEvent   emissionEvent{EmissionEvent::kClockInvalidNoData};
        bool            tight{false};             // lead below one cycle (near underrun, advisory)
        bool            tooFar{false};            // lead beyond the advisory reject window
    };

    struct Diagnostics {
        uint64_t initialSeeds{0};
        uint64_t timingDiscontinuities{0};
        uint64_t oneCycleSlipsCompensated{0};
        uint64_t leadAcceptedData{0};
        uint64_t leadGateNoData{0};
        uint64_t clockInvalidNoData{0};
        uint64_t tightWarnings{0};
        uint64_t farWarnings{0};

        [[nodiscard]] uint64_t resets() const noexcept {
            return initialSeeds + timingDiscontinuities;
        }
    };

    void Reset() noexcept;

    /// Process one transmit-cycle callback.
    /// @param transmitCycle      the local TX bus cycle for this callback; a 13-bit
    ///                           OHCI field in [0, kCyclesPerSecond) that wraps
    ///                           every second. The mod-kCyclesPerSecond delta
    ///                           between successive calls is what tells the
    ///                           continuity predictor how many isoch cycles
    ///                           actually elapsed (the loop is not guaranteed
    ///                           to be invoked exactly once per cycle).
    /// @param devicePhaseTicks   recovered device offset (24.576 MHz, 1-second domain).
    /// @param recoveredClockValid whether the recovered device clock is currently usable.
    /// @param dataCandidate      the wire DATA/NO-DATA cadence decision the caller has
    ///                           ALREADY made (e.g. PacketAssembler::nextIsData(), the
    ///                           spec-mandated CIP/DBC sequencing the device expects
    ///                           bit-for-bit). Authoritative: emitData mirrors it
    ///                           exactly and the output phase advances in lockstep
    ///                           with it. The lead gate never overrides this -- it
    ///                           only reports whether the lead was healthy when the
    ///                           caller shipped or withheld (see EmissionEvent).
    /// @param framesPerPacket    audio frames carried by a DATA packet (8 @ 48k blocking).
    [[nodiscard]] Decision ProcessCycle(uint32_t transmitCycle,
                                        int64_t devicePhaseTicks,
                                        bool recoveredClockValid,
                                        bool dataCandidate,
                                        uint32_t framesPerPacket) noexcept;

    [[nodiscard]] const Diagnostics& GetDiagnostics() const noexcept { return diag_; }
    [[nodiscard]] bool Seeded() const noexcept { return seeded_; }
    [[nodiscard]] int64_t OutputPhaseTicks() const noexcept { return outputPhaseTicks_; }

private:
    void Seed(int64_t devicePhaseTicks) noexcept;

    int64_t  outputPhaseTicks_{0};      // unwrapped monotonic accumulator
    int64_t  lastDevicePhaseTicks_{-1}; // continuity predictor input; -1 = none yet
    uint32_t lastTransmitCycle_{0};
    bool     haveLastTransmitCycle_{false};
    uint32_t slipCounter_{0};
    bool     seeded_{false};
    Diagnostics diag_{};
};

} // namespace ASFW::AudioEngine::DirectIsoch
