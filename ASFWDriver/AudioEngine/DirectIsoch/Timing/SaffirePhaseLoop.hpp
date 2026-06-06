#pragma once

#include "../../../AudioWire/AMDTP/TimingUtils.hpp"
#include "../../../Logging/Logging.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace ASFW::AudioEngine::DirectIsoch {

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
    // Presentation lead, in 24.576 MHz ticks: how far ahead of the transmit cycle the
    // SYT presentation time sits, so the packet arrives and is buffered before play-out.
    // kTransferDelayTicks (0x2E00 ≈ 3.83 cycles) is the Linux/FFADO TRANSFER_DELAY.
    static constexpr int64_t kInitialLeadTicks = ASFW::Timing::kTransferDelayTicks;   // ~3.83 cycles

    void Reset() noexcept {
        outputPhaseTicks_ = 0;
        phaseValid_ = false;
        deviceTargetValid_ = false;
        deviceTargetTicks_ = 0;
        cadenceReadIndex_ = 0;
    }

    void BeginGroup(const TxPhaseGroupUpdate& update) noexcept {
        // We need exactly ONE thing from the device clock: its constant sub-cycle
        // presentation phase (the fixed 0x0b0 the device's sample-clock PLL holds). We
        // do NOT use the device's absolute phase — recoveredDeviceOffsetTicks is seeded
        // from a raw SYT field with an arbitrary origin that has no meaning in our bus
        // cycle frame, so its absolute cycle nibble can't time a presentation. The cycle
        // alignment comes from OUR transmit cycle (EmitPacket); only the sub-cycle phase
        // comes from the device. Same FireWire bus / cycle master → that sub-cycle phase
        // shares our cycle timer and is genuinely stable.
        phaseValid_ = true;
        if (update.recoveredValid) {
            deviceTargetTicks_ =
                ASFW::Timing::normalizeOffsetDomain(update.recoveredDeviceOffsetTicks);
            deviceTargetValid_ = true;
        }
    }

    [[nodiscard]] TxPhasePacketResult EmitPacket(int64_t projectedOffsetTicks,
                                                 int64_t cadenceDeltaTicks) noexcept {
        (void)cadenceDeltaTicks;  // rate comes from the transmit cycle itself (same bus)
        TxPhasePacketResult result{};
        result.projectedOffsetTicks = ASFW::Timing::normalizeOffsetDomain(projectedOffsetTicks);
        result.phaseValid = phaseValid_;
        if (!phaseValid_) {
            return result;  // SYT 0xFFFF until the first hardware group establishes phase
        }

        // Presentation time in OUR bus frame: transmit cycle + a fixed presentation
        // lead. The cycle nibble therefore tracks the real bus cycle — correct arrival
        // timing — and steps with the actual blocking cadence (3072 per data packet,
        // +6144 across a nodata gap), exactly like the reference on the wire.
        int64_t phase =
            ASFW::Timing::normalizeOffsetDomain(result.projectedOffsetTicks + kInitialLeadTicks);

        // Graft the device's CONSTANT recovered sub-cycle phase onto that cycle. This is
        // what fixes the original wire bug: a bare cycle-quantized SYT carries offset
        // 0x000, but the device holds 0x0b0 and its clock recovery locks to that fixed
        // sub-cycle. We replace only the sub-cycle (low part), keeping our bus cycle nibble.
        if (deviceTargetValid_) {
            const int64_t cyc = static_cast<int64_t>(ASFW::Timing::kTicksPerCycle);
            const int64_t subCycle = ((deviceTargetTicks_ % cyc) + cyc) % cyc;
            phase = ASFW::Timing::normalizeOffsetDomain(
                phase - (((phase % cyc) + cyc) % cyc) + subCycle);
            result.leadTicks = subCycle;  // diagnostic: the grafted device sub-cycle offset
        }

        outputPhaseTicks_ = phase;
        result.phaseTicks = phase;
        result.leadAccepted = true;
        result.syt = EncodeOffsetTicksToSyt(phase);

        // Throttled phase loop diagnostic
        static std::atomic<uint64_t> emitCount{0};
        const uint64_t count = emitCount.fetch_add(1, std::memory_order_relaxed);
        if (count <= 256 || (count % 1024) == 0) {
            ASFW_LOG(Isoch,
                     "PHASE emit: count=%llu phaseTicks=%lld projected=%lld subCycle=%lld dev=%d syt=0x%04x",
                     count,
                     phase,
                     result.projectedOffsetTicks,
                     result.leadTicks,
                     deviceTargetValid_ ? 1 : 0,
                     result.syt);
        }

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
    int64_t outputPhaseTicks_{0};        // last emitted presentation phase (diagnostic)
    bool phaseValid_{false};             // a hardware group has established phase
    bool deviceTargetValid_{false};      // a recovered device sub-cycle phase is available
    int64_t deviceTargetTicks_{0};       // last recovered device phase; only its sub-cycle is used
    uint32_t cadenceReadIndex_{0};
};

} // namespace ASFW::AudioEngine::DirectIsoch
