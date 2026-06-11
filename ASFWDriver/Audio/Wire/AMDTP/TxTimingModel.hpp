#pragma once

#include "RxSytCadence.hpp"

#include <cstdint>

namespace ASFW::Driver {

class TxTimingModel final {
public:
    struct Config final {
        int64_t initialLeadTicks{3072}; // Saffire FillFirewireBuffers 0xec30
        uint32_t sytIntervalFrames{8};
        int32_t phaseDeadband{409};      // AllocateStreams 0x11f01
        int64_t tightLeadTicks{3072};   // below: near-underrun warning
        int64_t acceptLeadTicks{7620};  // at/above: device rejects (gate)
        int64_t escalateLeadTicks{12287}; // advisory log-escalation only
    };

    enum class LeadHealth : uint8_t {
        kNotSeeded = 0,
        kAccepted,     // tight < lead < accept: the safe operating range
        kTightWarn,    // 0 <= lead <= tight: shipping while thin
        kLate,         // lead < 0: shipped by Saffire, but worth diagnosing
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

    [[nodiscard]] bool IsSeeded() const noexcept;

    // The SYT the NEXT data packet would carry. `packetAnchorTicks` is the
    // projected OHCI execution cycle for that packet in the 8-second domain.
    // Before RX cadence lock, or outside Saffire's accepted lead window, the
    // decision remains unseeded and the caller emits SYT=0xffff.
    [[nodiscard]] Decision PeekNextDataSyt(
        int64_t packetAnchorTicks,
        const RxSytCadence& rxCadence) noexcept;

    // The packetizer actually emitted a data packet: advance one step.
    void CommitDataPacket() noexcept;

    [[nodiscard]] int64_t OutputPhaseTicks() const noexcept;

private:
    [[nodiscard]] int64_t AdjustOutputPhase(
        int64_t executionPhaseTicks,
        int64_t candidatePhaseTicks,
        const RxSytCadence::Snapshot& rx) noexcept;
    [[nodiscard]] uint16_t SytForPhase(int64_t phaseTicks) const noexcept;
    [[nodiscard]] LeadHealth HealthForLead(int64_t leadTicks) const noexcept;

    Config config_{};
    int64_t phaseTicks_{0};
    bool seeded_{false};
    bool forceAdjust_{true};
    uint32_t cadenceEpoch_{0};
    uint16_t cadenceReadIndex_{RxSytCadence::kNoInfo};
    uint16_t pendingCadenceTicks_{0};
};

} // namespace ASFW::Driver
