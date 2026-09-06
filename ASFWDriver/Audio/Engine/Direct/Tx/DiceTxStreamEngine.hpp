#pragma once

#include "../../../DriverKit/Config/AudioStreamProfile.hpp"
#include "../../../Ports/IAmdtpTxSlotProvider.hpp"
#include "../../../Ports/ITxPcmSource.hpp"
#include "../../../Shared/AudioHalBufferProfiles.hpp"
#include "../../../Shared/AudioTimingGeometry.hpp"
#include "../../../Wire/AMDTP/AmdtpCadence.hpp"
#include "../../../Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../../../Wire/AMDTP/AmdtpTxPacketizer.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Protocols::Audio::DICE {

struct DiceTxEngineCounters final {
    std::atomic<uint64_t> packetsPrepared{0};
    std::atomic<uint64_t> dataPacketsPrepared{0};
    std::atomic<uint64_t> noDataPacketsPrepared{0};
    std::atomic<uint64_t> slotAcquireFailures{0};
    std::atomic<uint64_t> pcmCopiesReady{0};
    std::atomic<uint64_t> pcmCopiesNotYetPublished{0};
    std::atomic<uint64_t> pcmCopiesExpired{0};
    std::atomic<uint64_t> pcmCopiesWrongEpoch{0};
    std::atomic<uint64_t> pcmCopiesConcurrentRewrite{0};
    std::atomic<uint64_t> pcmCopiesInvalid{0};
    /// DATA packets that reached the wire as silence because no content was
    /// ever accepted for them. Every planned DATA packet is armed with silence,
    /// so this counts arms that no fill replaced -- not encode failures.
    std::atomic<uint64_t> pcmSilenceSubstitutions{0};
    /// Late fills accepted by the producer seam (transport may still bind the
    /// armed image if it maps the slot first; that race is normal).
    std::atomic<uint64_t> lateFillsPublished{0};
    /// Fills refused because the slot was already frozen -- transport had bound
    /// it. This is the distribution that sets the content lead.
    std::atomic<uint64_t> lateFillsTooLate{0};
    /// Fills skipped because the content still was not available.
    std::atomic<uint64_t> lateFillsUnavailable{0};

    /// Clear every counter for a new run.
    ///
    /// These are read beside queue counters that `ResetConsumerForArm` zeroes
    /// on every arm -- `filled` minus transport's `lost` above all, which the
    /// [TxFill] line advertises as the truthful content figure. Two lifetimes
    /// make that subtraction meaningless: a restart that kept `filled` and
    /// zeroed `lost` raises the advertised figure without any new content
    /// reaching the wire. One run, one lifetime, both sides.
    void Reset() noexcept {
        packetsPrepared.store(0, std::memory_order_relaxed);
        dataPacketsPrepared.store(0, std::memory_order_relaxed);
        noDataPacketsPrepared.store(0, std::memory_order_relaxed);
        slotAcquireFailures.store(0, std::memory_order_relaxed);
        pcmCopiesReady.store(0, std::memory_order_relaxed);
        pcmCopiesNotYetPublished.store(0, std::memory_order_relaxed);
        pcmCopiesExpired.store(0, std::memory_order_relaxed);
        pcmCopiesWrongEpoch.store(0, std::memory_order_relaxed);
        pcmCopiesConcurrentRewrite.store(0, std::memory_order_relaxed);
        pcmCopiesInvalid.store(0, std::memory_order_relaxed);
        pcmSilenceSubstitutions.store(0, std::memory_order_relaxed);
        lateFillsPublished.store(0, std::memory_order_relaxed);
        lateFillsTooLate.store(0, std::memory_order_relaxed);
        lateFillsUnavailable.store(0, std::memory_order_relaxed);
    }
};

enum class TxSlotPrepareResult : uint8_t {
    Prepared = 0,
    SlotProviderUnavailable,
    PcmSourceUnavailable,
    PcmNotYetPublished,
    PcmExpired,
    PcmWrongEpoch,
    PcmConcurrentRewrite,
    PcmInvalidRequest,
    SlotAcquireFailed,
    PacketizerRejected,
    SlotPublishFailed,
    CommitRejected,
};

enum class TxSlotFillResult : uint8_t {
    Filled = 0,
    /// Transport already bound the slot: the armed silence transmits.
    TooLate,
    /// Content is not available for this range (yet, or ever).
    ContentUnavailable,
    /// Nothing to do: the packet is cadence NO-DATA, or was never armed.
    NotFillable,
    Rejected,
};

class DiceStreamConfigMapper final {
public:
    [[nodiscard]] static AMDTP::AmdtpStreamConfig ToAmdtpConfig(
        const ASFW::Isoch::Audio::AudioStreamConfig& streamConfig) noexcept;
};

// Backend cadence + wire encoder. The shared HardwareSampleTimeline supplies
// the absolute frame and presentation time for every plan.
class DiceTxStreamEngine final {
public:
    DiceTxStreamEngine() noexcept = default;

    [[nodiscard]] bool Configure(
        const ASFW::Isoch::Audio::IAudioStreamProfile& profile,
        const ASFW::Isoch::Audio::AudioStreamConfig& txConfig) noexcept;
    void BindSlotProvider(AMDTP::IAmdtpTxSlotProvider* slotProvider) noexcept;
    void BindPcmSource(ASFW::Audio::Ports::ITxPcmSource* pcmSource) noexcept;
    void ResetForStart(uint8_t initialDbc) noexcept;

    // Preview never consumes cadence. A successful PrepareTransmitSlot commits
    // cadence/DBC only after the transport slot has been published.
    [[nodiscard]] bool PreviewPresentationPlan(
        uint64_t epoch,
        uint64_t cycleOrdinal,
        uint64_t firstAudioFrame,
        uint64_t presentationBusTicks,
        const AMDTP::AmdtpTimingState& timing,
        AMDTP::TxPresentationPlan& outPlan,
        uint8_t& outWireDataBlocks,
        uint16_t& outSyt) const noexcept;
    /// Arm a packet: fix cadence, DBC, SYT and disposition, and publish a
    /// complete silent image so transport always has something to transmit.
    /// Content is not consulted here -- see FillTransmitSlot.
    [[nodiscard]] TxSlotPrepareResult PrepareTransmitSlot(
        uint32_t packetIndex,
        const AMDTP::TxPresentationPlan& plan,
        uint8_t wireDataBlocks,
        uint16_t syt) noexcept;

    /// Encode real PCM for an already-armed packet into its late image.
    /// Succeeds only while the slot is still writable. The image is NOT offered
    /// to transport until CommitFill, so a caller driving several streams can
    /// encode them all and then publish only if every one succeeded -- a device
    /// whose streams share one presentation plan must not put content on one
    /// stream and silence on its sibling for the same frame range.
    [[nodiscard]] TxSlotFillResult FillTransmitSlot(uint32_t packetIndex) noexcept;

    /// Offer the encoded late image to transport. Transport still takes it only
    /// if it maps the slot afterwards; losing that race is normal.
    [[nodiscard]] bool CommitFill(uint32_t packetIndex) noexcept;

    /// Lowest packet index that may still be worth filling.
    [[nodiscard]] uint64_t FreezeFrontier() const noexcept;

    /// Record that an armed DATA packet reached freeze with no content, so it
    /// transmits the silence it was armed with. Only the caller knows this: it
    /// happens when the freeze frontier moves past a packet the fill pass never
    /// reached, and transport cannot report it without learning what silence
    /// means.
    void NoteFrozenWithoutContent(uint32_t packetIndex) noexcept;

    [[nodiscard]] AMDTP::AmdtpPacketTimeline& Timeline() noexcept;
    [[nodiscard]] const AMDTP::AmdtpPacketTimeline& Timeline() const noexcept;
    [[nodiscard]] const AMDTP::AmdtpStreamConfig& StreamConfig() const noexcept;
    [[nodiscard]] const DiceTxEngineCounters& Counters() const noexcept;

private:
    static constexpr uint32_t kMaxPcmSnapshotSamples =
        ASFW::Encoding::kMaxPcmChannels *
        ASFW::Audio::Shared::kMaxBlockingFramesPerDataPacket;
    [[nodiscard]] AMDTP::AmdtpTxPolicy BuildTxPolicy(
        const ASFW::Isoch::Audio::AudioStreamTxPolicy& policy) const noexcept;

    ASFW::Isoch::Audio::AudioStreamConfig streamConfig_{};
    ASFW::Isoch::Audio::AudioStreamTxPolicy txPolicy_{};
    AMDTP::AmdtpTxPacketizer packetizer_{};
    AMDTP::BlockingCadence blockingCadence_{};
    AMDTP::NonBlocking48kCadence nonBlockingCadence_{};
    AMDTP::IAmdtpCadence* cadence_{nullptr};
    AMDTP::PacketTimelineSlot timelineSlots_[
        ASFW::Audio::Shared::AudioTimingGeometry::kTimelineSlots]{};
    AMDTP::AmdtpPacketTimeline timeline_{};
    AMDTP::IAmdtpTxSlotProvider* slotProvider_{nullptr};
    ASFW::Audio::Ports::ITxPcmSource* pcmSource_{nullptr};
    std::array<float, kMaxPcmSnapshotSamples> pcmScratch_{};
    /// Always zero. Arming encodes from this so the armed image is a valid
    /// silent packet without consulting the content source at all.
    std::array<float, kMaxPcmSnapshotSamples> silenceScratch_{};
    /// The armed packet for each producer slot, so a later fill can reproduce
    /// its exact wire geometry -- DBC and SYT above all, which the live counter
    /// can no longer supply by the time content arrives.
    AMDTP::PreparedTxPacket armedPackets_[
        ASFW::Audio::Shared::AudioTimingGeometry::kTimelineSlots]{};
    bool armedFilled_[
        ASFW::Audio::Shared::AudioTimingGeometry::kTimelineSlots]{};
    DiceTxEngineCounters counters_{};
};

} // namespace ASFW::Protocols::Audio::DICE
