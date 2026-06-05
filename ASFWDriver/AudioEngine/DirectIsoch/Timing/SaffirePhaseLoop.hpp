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
    // Presentation lead held ahead of the transmit cycle, in 24.576 MHz ticks.
    // Seeds the phase; the SYT then free-runs from here at the sample cadence.
    //
    // This is the AMDTP transfer delay: the SYT presentation time must sit far enough
    // ahead of transmit that the packet lands and the device can buffer it before its
    // DAC presents the sample. One cycle (kTicksPerCycle) is far too tight — the
    // reference Saffire.kext host stream presents at the SAME lead as the device
    // (~3 cycles), while a 1-cycle lead left the device with no time to present →
    // garbage even on silence. kTransferDelayTicks (0x2E00 ≈ 3.83 cycles) is the
    // Linux/FFADO TRANSFER_DELAY and matches the device's observed lead.
    static constexpr int64_t kInitialLeadTicks = ASFW::Timing::kTransferDelayTicks;   // ~3.83 cycles
    // Gross-desync band. The per-packet lead measured against the cycle-QUANTIZED
    // projected naturally swings by up to ~a cycle, so the re-sync band must be much
    // wider than that swing — otherwise we'd snap the phase onto whole-cycle
    // boundaries every packet and the SYT would lose its sub-cycle progression (the
    // exact bug that scrambled the device clock recovery). Only true multi-cycle
    // drift re-syncs.
    static constexpr int64_t kResyncBandTicks = 2 * ASFW::Timing::kTicksPerCycle;     // 6144

    void Reset() noexcept {
        outputPhaseTicks_ = 0;
        phaseValid_ = false;
        seeded_ = false;
        cadenceReadIndex_ = 0;
        minError_ = std::numeric_limits<int64_t>::max();
        maxError_ = std::numeric_limits<int64_t>::min();
    }

    void BeginGroup(const TxPhaseGroupUpdate& update) noexcept {
        // Establish the phase, but DON'T seed outputPhaseTicks_ from here.
        //
        // BeginGroup only sees the coarse group-boundary projected cycle
        // (seedProjectedCycle = completedCycle + packetsAhead), which is offset from
        // the per-packet transmitCycle EmitPacket uses by the refill distance.
        // Seeding/disciplining against it puts the phase behind the cycle its SYT is
        // measured against. So seeding is deferred to the first EmitPacket, against
        // the per-packet reference.
        //
        // The device's recovered clock (update.recoveredDeviceOffsetTicks /
        // recoveredValid) is intentionally NOT used for phase: that accumulator
        // (ExternalSyncBridge) is seeded from an arbitrary device SYT field value and
        // free-runs in the 8-second domain, so it has no common zero with our transmit
        // counter. The device clock reaches the loop only as the per-packet
        // cadenceDelta (rate), never as absolute phase.
        //
        // Option B (deferred — FW-26 cadence-ring work): keep absolute device-phase
        // discipline by converting the device offset INTO the transmit base — capture
        // phaseBias = extOffsetDiff1s(recoveredDeviceOffsetTicks, projectedOffsetTicks)
        // once at establishment, then target (projected + phaseBias + lead). That
        // tracks long-term device drift precisely but needs the 512-entry SYT-delta
        // cadence ring stable first; for bring-up the transmit-domain free-run below
        // is sufficient and matches reference clock-master AMDTP transmitters.
        (void)update;
        phaseValid_ = true;
    }

    [[nodiscard]] TxPhasePacketResult EmitPacket(int64_t projectedOffsetTicks,
                                                 int64_t cadenceDeltaTicks) noexcept {
        TxPhasePacketResult result{};
        result.projectedOffsetTicks = ASFW::Timing::normalizeOffsetDomain(projectedOffsetTicks);
        result.phaseValid = phaseValid_;
        if (!phaseValid_) {
            return result;
        }

        // Seed lazily from the PER-PACKET transmit cycle (not the coarse group cycle),
        // so the presentation phase is anchored to the same reference the SYT is
        // measured against.
        if (!seeded_) {
            outputPhaseTicks_ =
                ASFW::Timing::normalizeOffsetDomain(result.projectedOffsetTicks + kInitialLeadTicks);
            seeded_ = true;
        }

        // The SYT must advance SMOOTHLY by the sample cadence (≈4096 ticks/packet at
        // 48 kHz blocking), carrying sub-cycle offsets — that smooth progression is
        // what the device locks its sample clock to. So the phase free-runs by cadence
        // and we encode it directly. Crucially we do NOT snap it to the transmit cycle
        // per packet: the per-packet lead vs the cycle-QUANTIZED projected swings by up
        // to a cycle, and snapping on that swing forces the SYT onto whole-cycle
        // boundaries (offset always 0, stepping 3072/6144 instead of a clean 4096),
        // which scrambles the device's clock recovery into garbage audio.
        int64_t lead = ASFW::Timing::extOffsetDiff1s(outputPhaseTicks_, result.projectedOffsetTicks);

        // Re-sync only on GROSS desync (real multi-cycle drift), with a band far wider
        // than the per-packet quantization swing, so steady state never snaps.
        const int64_t errTicks = lead - kInitialLeadTicks;
        const int64_t absErr = (errTicks < 0) ? -errTicks : errTicks;
        if (absErr > kResyncBandTicks) {
            outputPhaseTicks_ =
                ASFW::Timing::normalizeOffsetDomain(result.projectedOffsetTicks + kInitialLeadTicks);
            lead = kInitialLeadTicks;
            if (errTicks < minError_) {
                minError_ = errTicks;
            }
            if (errTicks > maxError_) {
                maxError_ = errTicks;
            }
        }

        result.phaseTicks = outputPhaseTicks_;
        result.leadTicks = lead;
        // Once the phase is established we emit a SYT every data packet (smooth
        // cadence). 0xFFFF is reserved for genuinely unestablished phase / no-data.
        result.leadAccepted = true;
        result.syt = EncodeOffsetTicksToSyt(outputPhaseTicks_);

        // Throttled phase loop diagnostic
        static std::atomic<uint64_t> emitCount{0};
        const uint64_t count = emitCount.fetch_add(1, std::memory_order_relaxed);
        if (count <= 256 || (count % 1024) == 0) {
            ASFW_LOG(Isoch,
                     "PHASE emit: count=%llu phaseTicks=%lld projected=%lld lead=%lld accepted=%d syt=0x%04x target=%lld resyncBand=%lld",
                     count,
                     outputPhaseTicks_,
                     result.projectedOffsetTicks,
                     result.leadTicks,
                     result.leadAccepted ? 1 : 0,
                     result.syt,
                     kInitialLeadTicks,
                     kResyncBandTicks);
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
    bool seeded_{false};
    uint32_t cadenceReadIndex_{0};
    int64_t minError_{std::numeric_limits<int64_t>::max()};
    int64_t maxError_{std::numeric_limits<int64_t>::min()};
};

} // namespace ASFW::AudioEngine::DirectIsoch
