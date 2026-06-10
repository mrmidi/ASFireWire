#include "DiceTxStreamEngine.hpp"

namespace ASFW::Protocols::Audio::DICE {

// Design decisions (see ../../../README.md, Step 5):
//
// 1. The engine is composition glue only: profile quirks → AmdtpTxPolicy,
//    DiceStreamConfig → AmdtpStreamConfig (mapper), and the
//    packetizer / payload-writer / timeline triple bound to one slot
//    provider. It owns no wire logic of its own.
// 2. Publish-at-prepare: PrepareNextTransmitSlot exposes the packet on the
//    timeline and immediately publishes the wire image to the provider. The
//    backing bytes stay live (the timeline slot remains ExposedForAudio), so
//    host PCM written later through WriteHostOutputFloat32 lands in the same
//    storage until the ring position is reused — the Saffire model: a
//    structurally valid packet whose payload is silence until audio arrives.
// 3. Failure contract mirrors the packetizer: a failed prepare advances no
//    cadence/DBC/frame state and publishes nothing, so the call is retryable.
// 4. Counters are sticky diagnostics; ResetForStart does not clear them.

bool DiceTxStreamEngine::Configure(const IDiceDeviceProfile& profile,
                                   const DiceStreamConfig& txConfig) noexcept {
    if (txConfig.direction != DiceStreamDirection::HostToDevice) {
        return false;
    }

    const DiceDeviceQuirks quirks = profile.Quirks();
    const AMDTP::AmdtpStreamConfig amdtpConfig =
        DiceStreamConfigMapper::ToAmdtpConfig(txConfig);
    const AMDTP::AmdtpTxPolicy policy = BuildTxPolicy(quirks);

    const uint32_t slotCount =
        static_cast<uint32_t>(sizeof(timelineSlots_) / sizeof(timelineSlots_[0]));
    if (!timeline_.AttachSlots(timelineSlots_, slotCount)) {
        return false;
    }

    packetizer_.BindTimeline(&timeline_);
    if (!packetizer_.Configure(amdtpConfig, policy)) {
        return false;
    }
    payloadWriter_.Configure(packetizer_.StreamConfig(), policy);
    payloadWriter_.BindTimeline(&timeline_);

    profile_ = &profile;
    diceConfig_ = txConfig;
    quirks_ = quirks;
    return true;
}

void DiceTxStreamEngine::BindSlotProvider(
    AMDTP::IAmdtpTxSlotProvider* slotProvider) noexcept {
    slotProvider_ = slotProvider;
}

void DiceTxStreamEngine::ResetForStart(uint8_t initialDbc,
                                       uint64_t initialAudioFrame) noexcept {
    timeline_.Reset();
    packetizer_.Reset(initialDbc, initialAudioFrame);
}

bool DiceTxStreamEngine::PrepareNextTransmitSlot(
    uint32_t packetIndex, const AMDTP::AmdtpTimingState& timing) noexcept {
    if (slotProvider_ == nullptr) {
        return false;
    }

    AMDTP::TxPacketSlotView slot{};
    if (!slotProvider_->AcquireWritableSlot(packetIndex, slot)) {
        counters_.slotAcquireFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    AMDTP::PreparedTxPacket packet{};
    if (!packetizer_.PrepareNextPacket(slot, timing, packet)) {
        return false;
    }

    slotProvider_->PublishSlot(packet);

    counters_.packetsPrepared.fetch_add(1, std::memory_order_relaxed);
    if (packet.isData) {
        counters_.dataPacketsPrepared.fetch_add(1, std::memory_order_relaxed);
    } else {
        counters_.noDataPacketsPrepared.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void DiceTxStreamEngine::WriteHostOutputFloat32(
    const AMDTP::HostAudioBufferView& hostBuffer) noexcept {
    payloadWriter_.WriteFloat32Interleaved(hostBuffer);
}

AMDTP::AmdtpPacketTimeline& DiceTxStreamEngine::Timeline() noexcept {
    return timeline_;
}

const AMDTP::AmdtpPacketTimeline& DiceTxStreamEngine::Timeline() const noexcept {
    return timeline_;
}

const DiceTxEngineCounters& DiceTxStreamEngine::Counters() const noexcept {
    return counters_;
}

AMDTP::AmdtpTxPolicy DiceTxStreamEngine::BuildTxPolicy(
    const DiceDeviceQuirks& quirks) const noexcept {
    AMDTP::AmdtpTxPolicy policy{};
    policy.hostToDevicePcmEncoding = quirks.tx.hostToDevicePcmEncoding;
    policy.dbsPolicy = quirks.tx.dbsPolicy;
    policy.defaultNonAudioSlotWord = quirks.tx.defaultNonAudioSlotWord;
    policy.initializeNonAudioSlots = quirks.tx.initializeNonAudioSlots;
    policy.clearPayloadBeforeExposure = true;
    return policy;
}

} // namespace ASFW::Protocols::Audio::DICE
