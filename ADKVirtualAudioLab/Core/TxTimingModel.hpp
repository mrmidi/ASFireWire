#pragma once

#include "../Ports/ICycleTimeline.hpp"

#include <cstdint>

namespace ASFW::Driver {

// Frame -> cycle -> SYT mapping: the unit under research (README, target
// layout). Mirrors the contract of ASFW's Encoding::SYTGenerator and the
// authoritative reference model (tools/debug/tx_phase_loop_model.py in the
// DICE tree), constants from the Saffire decompile:
//
//   - output phase is an UNWRAPPED int64 in the 24.576 MHz tick domain
//   - seed: phase = now + presentation delay, then the constant device
//     sub-cycle graft: phase = phase - (phase % 3072) + 0x0B0
//     (keep the cycle, replace the sub-cycle offset — Saffire Pro24's
//     recovered constant)
//   - data packets advance the phase by the fixed blocking step
//     8 frames x 512 ticks = 4096; no-data packets advance nothing
//   - SYT = cycle[3:0] ‖ offset[11:0] of the phase in the 16-cycle
//     (49152-tick) SYT domain
//   - transmit-lead health (P5): lead = phase - now; warn at <= 3071 (under
//     one cycle, near-underrun), healthy while < 7620, gate at >= 7620 (the
//     device's actual rejection threshold), >= 12287 is log-escalation only
//
// WHO DECIDES THE WIRE: not this model. The cadence's DATA/NO-DATA pattern is
// the device-validated sequencing (see TxOutputPhaseLoop's contract in the
// DICE tree); lead health here is pure telemetry that may freely co-occur
// with any cadence outcome. Callers Peek the SYT a data packet would carry,
// let the packetizer decide the wire, then Commit only what was actually
// emitted — the model tracks, it never decides.
class TxTimingModel final {
public:
    struct Config final {
        int64_t presentationDelayTicks{4096}; // SYTGenerator's anchor lead
        uint16_t deviceSubCycleTicks{0x0B0};  // Saffire Pro24 constant graft
        bool graftEnabled{true};
        int64_t tightLeadTicks{3072};   // below: near-underrun warning
        int64_t acceptLeadTicks{7620};  // at/above: device rejects (gate)
        int64_t escalateLeadTicks{12287}; // advisory log-escalation only
    };

    enum class LeadHealth : uint8_t {
        kNotSeeded = 0,
        kAccepted,     // tight < lead < accept: the safe operating range
        kTightWarn,    // 0 <= lead <= tight: shipping while thin
        kLate,         // lead < 0: presentation time already passed
        kGate,         // accept <= lead < escalate: device would reject
        kEscalate,     // lead >= escalate: drifted very far (advisory)
    };

    struct Decision final {
        uint16_t syt{0xFFFF};
        int64_t leadTicks{0};
        LeadHealth health{LeadHealth::kNotSeeded};
        bool seededThisCall{false};
    };

    static constexpr int64_t kTicksPerCycle = 3072;
    static constexpr int64_t kTicksPerFrame48k = 512;
    static constexpr int64_t kPacketStepTicks = 4096;  // 8 frames x 512
    static constexpr int64_t kSytDomainTicks = 49152;  // 16 cycles

    TxTimingModel() noexcept = default;

    void Configure(const Config& config) noexcept;
    [[nodiscard]] const Config& GetConfig() const noexcept;

    // Un-seeds and re-arms the transmit anchor (call on stream start).
    void Reset() noexcept;

    void ArmTransmitCycleAnchor() noexcept;
    [[nodiscard]] bool IsSeeded() const noexcept;

    // The SYT the NEXT data packet would carry. Seeds from the timeline on
    // the first call after arming; otherwise pure (no state advances until
    // CommitDataPacket). Lead/health are measured against timeline now.
    [[nodiscard]] Decision PeekNextDataSyt(
        const Ports::ICycleTimeline& timeline) noexcept;

    // The packetizer actually emitted a data packet: advance one step.
    void CommitDataPacket() noexcept;

    // Discipline rehearsal parity with SYTGenerator::nudgeOffsetTicks.
    void NudgeOffsetTicks(int32_t deltaTicks) noexcept;

    [[nodiscard]] int64_t OutputPhaseTicks() const noexcept;

private:
    [[nodiscard]] uint16_t SytForPhase(int64_t phaseTicks) const noexcept;
    [[nodiscard]] LeadHealth HealthForLead(int64_t leadTicks) const noexcept;

    Config config_{};
    int64_t phaseTicks_{0};
    bool seeded_{false};
    bool anchorArmed_{true};
};

} // namespace ASFW::Driver
