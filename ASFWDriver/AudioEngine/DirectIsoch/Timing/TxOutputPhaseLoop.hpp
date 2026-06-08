// TxOutputPhaseLoop.hpp
// ASFW - Phase-driven TX output timing derived from the Saffire reference model.

#pragma once

#include <cstdint>
#include "../../../AudioWire/AMDTP/TimingUtils.hpp"

namespace ASFW::AudioEngine::DirectIsoch {

/// Free-running TX output-phase accumulator + device-drift monitor.
///
/// Design (see tools/debug/asfw_saffire_tx_sim.py for the contract):
///   - The AMDTP assembler owns the wire DATA/NO-DATA cadence. This loop does NOT
///     decide cadence; the caller passes `isData` (the assembler's decision) and the
///     loop advances its phase by one DATA-packet step on DATA, holds on NO-DATA.
///   - `outputPhaseTicks_` is an UNWRAPPED, monotonically increasing int64 in the
///     24.576 MHz tick domain. OutputPhaseToAudioMap maps it to absolute audio frames
///     by plain subtraction, so it must never wrap.
///   - The recovered device phase is used only as a drift/discontinuity monitor: the
///     advisory `lead` (output ahead of device) and the glitch detector are computed
///     in the 1-second offset domain (`extOffsetDiff1s`), because the device phase the
///     pipeline reconstructs from the transmit cycle wraps once per second.
///   - On a genuine discontinuity (device glitch, or lead leaving the accepted window)
///     the phase is re-seeded and `rebased` is set so the caller re-anchors the map and
///     re-seeds the SYT generator.
///
/// SYT generation lives in Encoding::SYTGenerator, not here.
class TxOutputPhaseLoop {
public:
    static constexpr int64_t kTicksPerCycle  = ASFW::Timing::kTicksPerCycle;   // 3072
    static constexpr int64_t kTicksPerFrame  = ASFW::Timing::kTicksPerSample48k; // 512
    static constexpr int64_t kFramesPerCycle = kTicksPerCycle / kTicksPerFrame;  // 6
    static constexpr int64_t kOneSecondTicks = ASFW::Timing::kTicksPerSecond;    // 24'576'000

    /// Operating lead: how far the output phase sits ahead of the recovered device
    /// phase when (re)seeded. 1.5 cycles = 9 frames, mid-window.
    static constexpr int64_t kOperatingLead   = 4608;
    /// Below one cycle of lead is a near-underrun warning.
    static constexpr int64_t kLeadTightTicks  = kTicksPerCycle;   // 3072
    /// Beyond this the output has drifted too far ahead of the device -> re-seed.
    static constexpr int64_t kLeadRejectTicks = 12287;            // ~4 cycles
    /// A device-phase step that deviates from the predicted +1 cycle by more than
    /// this (in the 1-second domain) is treated as a discontinuity.
    static constexpr int64_t kGlitchThresholdTicks = kTicksPerCycle / 2;  // 1536

    /// Why ProcessCycle (re)seeded the phase and asked the caller to re-anchor.
    /// Distinguishes the expected one-shot `kInitialSeed` from the steady-state
    /// instability signals `kGlitch` / `kLeadReject` -- if either of the latter
    /// keeps recurring, the cadence/device-clock relationship is not converging.
    enum class RebaseReason : uint8_t {
        kNone = 0,        // not rebased this cycle
        kInitialSeed,     // first seed since construction or caller Reset()
        kGlitch,          // recovered device phase jumped (discontinuity)
        kLeadReject,      // output lead drifted outside the accepted window
    };

    [[nodiscard]] static constexpr const char* RebaseReasonName(RebaseReason reason) noexcept {
        switch (reason) {
            case RebaseReason::kInitialSeed: return "InitialSeed";
            case RebaseReason::kGlitch:      return "Glitch";
            case RebaseReason::kLeadReject:  return "LeadReject";
            case RebaseReason::kNone:        return "None";
        }
        return "None";
    }

    struct CycleResult {
        int64_t outputPhaseTicks{0};  // unwrapped phase for THIS packet (pre-advance)
        int64_t leadTicks{0};         // advisory lead vs device, 1-second domain
        bool    isData{false};        // echoes the caller's cadence decision
        bool    rebased{false};       // phase (re)seeded -> caller must re-anchor map/SYT
        RebaseReason rebaseReason{RebaseReason::kNone};  // why, when rebased is true
        bool    tight{false};         // lead below one cycle (near underrun)
        bool    tooFar{false};        // lead beyond the reject window
    };

    struct Diagnostics {
        uint64_t dataPackets{0};
        uint64_t noDataPackets{0};
        uint64_t rebases{0};            // == rebasesInitialSeed + rebasesGlitch + rebasesLeadReject
        uint64_t rebasesInitialSeed{0};
        uint64_t rebasesGlitch{0};
        uint64_t rebasesLeadReject{0};
        uint64_t tightWarnings{0};
        uint64_t farWarnings{0};
        uint64_t glitches{0};
    };

    void Reset() noexcept;

    /// Process one isoch cycle.
    /// @param devicePhaseTicks recovered device offset (24.576 MHz, 1-second domain).
    /// @param framesPerPacket  audio frames carried by a DATA packet (8 @ 48k blocking).
    /// @param isData           the assembler's DATA/NO-DATA decision for this cycle.
    [[nodiscard]] CycleResult ProcessCycle(int64_t devicePhaseTicks,
                                           uint32_t framesPerPacket,
                                           bool isData) noexcept;

    [[nodiscard]] const Diagnostics& GetDiagnostics() const noexcept { return diag_; }
    [[nodiscard]] bool Seeded() const noexcept { return seeded_; }
    [[nodiscard]] int64_t OutputPhaseTicks() const noexcept { return outputPhaseTicks_; }

private:
    int64_t outputPhaseTicks_{0};     // unwrapped monotonic accumulator
    int64_t predictedNextDevice_{-1}; // glitch detector (1-second domain); -1 = none
    bool    seeded_{false};
    Diagnostics diag_{};
};

} // namespace ASFW::AudioEngine::DirectIsoch
