#pragma once

#include "../../../AudioWire/AMDTP/TimingUtils.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace ASFW::Isoch::Core {

struct TxPhaseGroupUpdate final {
    int64_t projectedOffsetTicks{0};
    int64_t recoveredDeviceOffsetTicks{0};
    bool recoveredValid{false};
};

struct TxPhasePacketResult final {
    uint16_t syt{0xFFFF};
    int64_t phaseTicks{0};
    int64_t projectedOffsetTicks{0};
    int64_t leadTicks{0};
    bool phaseValid{false};
    bool leadAccepted{false};
};

class SaffireTxPhaseLoop final {
public:
    static constexpr int64_t kInitialLeadTicks = ASFW::Timing::kTicksPerCycle;
    static constexpr int64_t kNearUnderrunLeadTicks = ASFW::Timing::kTicksPerCycle - 1;
    static constexpr int64_t kAcceptedLeadTicks = 7620;
    static constexpr int64_t kTooFarLeadTicks = 12287;
    static constexpr int32_t kDeadband48k = 409;

    void Reset() noexcept {
        outputPhaseTicks_ = 0;
        phaseValid_ = false;
        forceAdjust_ = true;
        cadenceReadIndex_ = 0;
        minError_ = std::numeric_limits<int64_t>::max();
        maxError_ = std::numeric_limits<int64_t>::min();
    }

    void BeginGroup(const TxPhaseGroupUpdate& update) noexcept {
        if (!phaseValid_) {
            outputPhaseTicks_ =
                ASFW::Timing::normalizeOffsetDomain(update.projectedOffsetTicks + kInitialLeadTicks);
            phaseValid_ = true;
            forceAdjust_ = false;
            minError_ = std::numeric_limits<int64_t>::max();
            maxError_ = std::numeric_limits<int64_t>::min();
            return;
        }

        if (!update.recoveredValid) {
            return;
        }

        const int64_t errTicks =
            ASFW::Timing::extOffsetDiff(outputPhaseTicks_, update.recoveredDeviceOffsetTicks);
        if (errTicks < minError_) {
            minError_ = errTicks;
        }
        if (errTicks > maxError_) {
            maxError_ = errTicks;
        }

        const int64_t absErr = (errTicks < 0) ? -errTicks : errTicks;
        if (!forceAdjust_ && absErr <= kDeadband48k) {
            return;
        }

        const int64_t lead =
            ASFW::Timing::extOffsetDiff(outputPhaseTicks_, update.projectedOffsetTicks);
        outputPhaseTicks_ =
            ASFW::Timing::normalizeOffsetDomain(update.recoveredDeviceOffsetTicks + lead);
        forceAdjust_ = false;
        minError_ = errTicks;
        maxError_ = errTicks;
    }

    [[nodiscard]] TxPhasePacketResult EmitPacket(int64_t projectedOffsetTicks,
                                                 int64_t cadenceDeltaTicks) noexcept {
        TxPhasePacketResult result{};
        result.projectedOffsetTicks = ASFW::Timing::normalizeOffsetDomain(projectedOffsetTicks);
        result.phaseValid = phaseValid_;
        if (!phaseValid_) {
            return result;
        }

        result.phaseTicks = outputPhaseTicks_;
        result.leadTicks = ASFW::Timing::extOffsetDiff(outputPhaseTicks_, result.projectedOffsetTicks);
        result.leadAccepted =
            result.leadTicks > kNearUnderrunLeadTicks && result.leadTicks < kAcceptedLeadTicks;
        if (result.leadAccepted) {
            result.syt = EncodeOffsetTicksToSyt(outputPhaseTicks_);
        }

        const int64_t step = cadenceDeltaTicks > 0
            ? cadenceDeltaTicks
            : static_cast<int64_t>(ASFW::Timing::kSytPacketStepTicks48k);
        outputPhaseTicks_ = ASFW::Timing::normalizeOffsetDomain(outputPhaseTicks_ + step);
        cadenceReadIndex_ = (cadenceReadIndex_ + 1) & 0x1FFu;
        return result;
    }

    void SeedCadenceReadIndex(uint32_t rxWriteIndex) noexcept {
        cadenceReadIndex_ = (rxWriteIndex + 256u) & 0x1FFu;
    }

    [[nodiscard]] uint32_t CadenceReadIndex() const noexcept { return cadenceReadIndex_; }
    [[nodiscard]] bool PhaseValid() const noexcept { return phaseValid_; }
    [[nodiscard]] int64_t OutputPhaseTicks() const noexcept { return outputPhaseTicks_; }

    [[nodiscard]] static uint16_t EncodeOffsetTicksToSyt(int64_t offsetTicks) noexcept {
        const int64_t field =
            ASFW::Timing::normalizeOffsetDomain(offsetTicks) % ASFW::Timing::kSytFieldDomainTicks;
        const uint16_t cycle4 =
            static_cast<uint16_t>((field / ASFW::Timing::kTicksPerCycle) & 0x0F);
        const uint16_t ticks12 = static_cast<uint16_t>(field % ASFW::Timing::kTicksPerCycle);
        return static_cast<uint16_t>((cycle4 << 12) | ticks12);
    }

private:
    int64_t outputPhaseTicks_{0};
    bool phaseValid_{false};
    bool forceAdjust_{true};
    uint32_t cadenceReadIndex_{0};
    int64_t minError_{std::numeric_limits<int64_t>::max()};
    int64_t maxError_{std::numeric_limits<int64_t>::min()};
};

} // namespace ASFW::Isoch::Core
