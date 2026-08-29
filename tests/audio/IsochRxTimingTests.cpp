#include <gtest/gtest.h>

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "Audio/Engine/Direct/DirectInputWriter.hpp"
#include "Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.hpp"
#include "Audio/Engine/Direct/Rx/RxAudioPacketProcessor.hpp"
#include "Isoch/Receive/IsochRxTiming.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace {

uint32_t EncodeCycleTimer(uint32_t seconds,
                          uint32_t cycle,
                          uint32_t offset) {
    return (seconds << ASFW::Timing::kCycleTimerSecondsShift) |
           (cycle << ASFW::Timing::kCycleTimerCyclesShift) |
           offset;
}

void WriteBE32(uint8_t* dest, uint32_t value) {
    dest[0] = static_cast<uint8_t>(value >> 24);
    dest[1] = static_cast<uint8_t>(value >> 16);
    dest[2] = static_cast<uint8_t>(value >> 8);
    dest[3] = static_cast<uint8_t>(value);
}

class FixedDirectAudioBindingSource final
    : public ASFW::Audio::Runtime::IDirectAudioBindingSource {
  public:
    explicit FixedDirectAudioBindingSource(
        ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot) noexcept
        : snapshot_(snapshot) {}

    bool CopyDirectAudioBinding(
        ASFW::Audio::Runtime::DirectAudioBindingSnapshot& out) noexcept override {
        out = snapshot_;
        return true;
    }

  private:
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot_{};
};

template <size_t PacketSize>
void FillTwoChannelAmdtpPacket(std::array<uint8_t, PacketSize>& packet,
                               uint32_t slot0,
                               uint32_t slot1) {
    static_assert(PacketSize >= 8 + 8 + 8);
    WriteBE32(packet.data() + 8, 0x02020000u);
    WriteBE32(packet.data() + 12, 0x9002FFFFu);
    WriteBE32(packet.data() + 16, slot0);
    WriteBE32(packet.data() + 20, slot1);
}

} // namespace

TEST(IsochRxTimingTests, DecodesOhciTimestampFromReceivePrefix) {
    std::array<uint8_t, 16> packet{
        0x23, 0xA1, 0x00, 0x00, // LE OHCI timestamp quadlet.
        0x00, 0x00, 0x00, 0x00, // Isochronous packet header.
        0x02, 0x11, 0x00, 0xC8, // CIP Q0.
        0x90, 0x02, 0x40, 0xB0, // CIP Q1.
    };

    uint16_t timestamp = 0;
    ASSERT_TRUE(ASFW::Isoch::Rx::DecodeReceiveTimestamp(
        packet.data(), packet.size(), timestamp));
    EXPECT_EQ(timestamp, 0xA123u);
}

TEST(IsochRxTimingTests, ExpandsTimestampWithinCurrentEightSecondWindow) {
    const uint16_t timestamp =
        static_cast<uint16_t>((5u << 13) | 100u);
    const uint32_t reference = EncodeCycleTimer(13, 200, 64);

    ASFW::Isoch::Rx::ExpandedReceiveTimestamp expanded{};
    ASSERT_TRUE(ASFW::Isoch::Rx::ExpandReceiveTimestamp(
        timestamp, reference, expanded));

    const auto fields =
        ASFW::Timing::decodeCycleTimer(expanded.cycleTimer);
    EXPECT_EQ(fields.seconds, 13u);
    EXPECT_EQ(fields.cycle, 100u);
    EXPECT_EQ(fields.offset, 0u);
    EXPECT_EQ(
        expanded.ageTicks,
        100LL * ASFW::Timing::kTicksPerCycle + 64);
}

TEST(IsochRxTimingTests, ExpandsTimestampAcrossEightSecondBoundary) {
    const uint16_t timestamp =
        static_cast<uint16_t>((7u << 13) | 7990u);
    const uint32_t reference = EncodeCycleTimer(16, 10, 32);

    ASFW::Isoch::Rx::ExpandedReceiveTimestamp expanded{};
    ASSERT_TRUE(ASFW::Isoch::Rx::ExpandReceiveTimestamp(
        timestamp, reference, expanded));

    const auto fields =
        ASFW::Timing::decodeCycleTimer(expanded.cycleTimer);
    EXPECT_EQ(fields.seconds, 15u);
    EXPECT_EQ(fields.cycle, 7990u);
    EXPECT_EQ(fields.offset, 0u);
    EXPECT_EQ(
        expanded.ageTicks,
        20LL * ASFW::Timing::kTicksPerCycle + 32);
}

TEST(IsochRxTimingTests, AcceptsPacketCompletedAfterPreDrainReference) {
    const uint16_t timestamp =
        static_cast<uint16_t>((5u << 13) | 204u);
    const uint32_t reference = EncodeCycleTimer(13, 200, 64);

    ASFW::Isoch::Rx::ExpandedReceiveTimestamp expanded{};
    ASSERT_TRUE(ASFW::Isoch::Rx::ExpandReceiveTimestamp(
        timestamp, reference, expanded));

    const auto fields =
        ASFW::Timing::decodeCycleTimer(expanded.cycleTimer);
    EXPECT_EQ(fields.seconds, 13u);
    EXPECT_EQ(fields.cycle, 204u);
    EXPECT_EQ(fields.offset, 0u);
    EXPECT_EQ(
        expanded.ageTicks,
        -(4LL * ASFW::Timing::kTicksPerCycle - 64));
}

TEST(IsochRxTimingTests, PacketProcessorReturnsReceiveTimestamp) {
    constexpr size_t kFrames = 1;
    constexpr size_t kDbs = 17;
    std::array<uint8_t, 8 + 8 + (kFrames * kDbs * 4)> packet{};
    packet[0] = 0x23;
    packet[1] = 0xA1;
    packet[8] = 0x02;
    packet[9] = 0x11;
    packet[10] = 0x00;
    packet[11] = 0xC8;
    packet[12] = 0x90;
    packet[13] = 0x02;
    packet[14] = 0x40;
    packet[15] = 0xB0;

    ASFW::AudioEngine::Direct::DirectInputWriter writer;
    ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor processor(
        writer);
    const auto result = processor.ProcessPacket(
        packet.data(),
        packet.size(),
        0,
        2,
        kDbs,
        ASFW::Encoding::AudioWireFormat::kAM824);

    EXPECT_TRUE(result.hasValidCip);
    EXPECT_TRUE(result.hasReceiveCycleTimestamp);
    EXPECT_EQ(result.receiveCycleTimestamp, 0xA123u);
    EXPECT_EQ(result.syt, 0x40B0u);
    EXPECT_EQ(result.framesDecoded, 1u);
}

TEST(IsochRxTimingTests, PacketProcessorWritesAM824CaptureAsFloat32) {
    constexpr size_t kFrames = 1;
    constexpr size_t kDbs = 2;
    alignas(4) std::array<uint8_t, 8 + 8 + (kFrames * kDbs * 4)> packet{};
    FillTwoChannelAmdtpPacket(packet, 0x40000000u, 0x407FFFFFu);

    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    const ASFW::Audio::Runtime::AudioGraphBinding binding{
        .sampleRateHz = 48000,
        .memory = ASFW::Audio::Runtime::AudioStreamMemory{
            .inputBase = input.data(),
            .inputFrameCapacity = 4,
            .inputChannels = 2,
        },
        .control = &control,
        .deviceToHostAm824Slots = kDbs,
    };

    ASFW::AudioEngine::Direct::DirectInputWriter writer;
    writer.Bind(&binding);
    ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor processor(
        writer);

    const auto result = processor.ProcessPacket(
        packet.data(),
        packet.size(),
        0,
        2,
        kDbs,
        ASFW::Encoding::AudioWireFormat::kAM824);

    EXPECT_EQ(result.status,
              ASFW::AudioEngine::Direct::Rx::DirectRxWriteStatus::kAvailable);
    EXPECT_EQ(result.framesDecoded, 1u);
    EXPECT_FLOAT_EQ(input[0], 0.0f);
    EXPECT_FLOAT_EQ(input[1], 1.0f);
    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire),
              1u);
}

TEST(IsochRxTimingTests, CaptureMailboxWrapIsNotAnOverrunUntilCoreAudioReads) {
    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    const ASFW::Audio::Runtime::AudioGraphBinding binding{
        .sampleRateHz = 48000,
        .memory = ASFW::Audio::Runtime::AudioStreamMemory{
            .inputBase = input.data(),
            .inputFrameCapacity = 4,
            .inputChannels = 2,
        },
        .control = &control,
        .deviceToHostAm824Slots = 2,
    };

    ASFW::AudioEngine::Direct::DirectInputWriter writer;
    writer.Bind(&binding);

    writer.PublishProducedEnd(5);
    EXPECT_EQ(control.captureRingOverruns.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.rxCaptureBufferTelemetry.totalOverwrittenFrames.load(
                  std::memory_order_relaxed),
              0U);

    control.client.PublishBeginRead(5, 1, 1);
    control.counters.CountBeginRead();
    control.captureRingReadFrame.store(5, std::memory_order_release);
    writer.PublishProducedEnd(10);

    EXPECT_EQ(control.captureRingOverruns.load(std::memory_order_relaxed), 1U);
    EXPECT_EQ(control.rxCaptureBufferTelemetry.totalOverwrittenFrames.load(
                  std::memory_order_relaxed),
              1U);
}

TEST(IsochRxTimingTests, DirectReceiveConsumerOwnsDecodeAcrossOpaqueIsochSeam) {
    constexpr size_t kFrames = 1;
    constexpr size_t kDbs = 2;
    alignas(4) std::array<uint8_t, 8 + 8 + (kFrames * kDbs * 4)> packet{};
    packet[0] = 0x23;
    packet[1] = 0xA1;
    FillTwoChannelAmdtpPacket(packet, 0x40000000u, 0x407FFFFFu);

    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = 4,
        .inputChannels = 2,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source, {.am824Slots = kDbs, .streamChannels = kDbs});
    const ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 300, 0),
        .drainHostTicks = 1'000'000,
    };
    const ASFW::Isoch::IsochReceivePacket isochPacket{
        .descriptorIndex = 7,
        .payload = packet,
    };

    consumer.OnReceiveActivated();
    consumer.BeginReceiveBatch(batch);
    consumer.ConsumePacket(batch, isochPacket);

    EXPECT_FLOAT_EQ(input[0], 0.0f);
    EXPECT_FLOAT_EQ(input[1], 1.0f);
    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(control.rxReplayEpochResets.load(std::memory_order_acquire), 1u);

    consumer.OnReceiveQuiesced();
    consumer.ConsumePacket(batch, isochPacket);
    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), 1u);
}

// A stop/start of the same endpoint republishes the *same* binding generation:
// the audio side bumps it when the binding changes, not per stream start. The
// consumer must still rebind, because OnReceiveQuiesced() dropped the view.
// Regression: the cached generation used to survive the quiesce, so
// BeginReceiveBatch() took its "nothing changed" early return, the writer was
// never rebound, and every packet decoded down the kInvalidBinding path —
// dropping PCM while advancing the cursor and incrementing no reject counter.
// Capture went silent on the second start with entirely healthy telemetry.
TEST(IsochRxTimingTests, DirectReceiveConsumerRebindsAfterRestartAtSameGeneration) {
    constexpr size_t kFrames = 1;
    constexpr size_t kDbs = 2;
    alignas(4) std::array<uint8_t, 8 + 8 + (kFrames * kDbs * 4)> packet{};
    packet[0] = 0x23;
    packet[1] = 0xA1;
    FillTwoChannelAmdtpPacket(packet, 0x40000000u, 0x407FFFFFu);

    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = 4,
        .inputChannels = 2,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source, {.am824Slots = kDbs, .streamChannels = kDbs});
    const ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 300, 0),
        .drainHostTicks = 1'000'000,
    };
    const ASFW::Isoch::IsochReceivePacket isochPacket{
        .descriptorIndex = 7,
        .payload = packet,
    };

    consumer.OnReceiveActivated();
    consumer.BeginReceiveBatch(batch);
    consumer.ConsumePacket(batch, isochPacket);
    ASSERT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), 1u);

    // Stop, then start again with the binding generation unchanged.
    consumer.OnReceiveQuiesced();
    input.fill(0.0f);
    consumer.OnReceiveActivated();
    consumer.BeginReceiveBatch(batch);
    consumer.ConsumePacket(batch, isochPacket);

    EXPECT_FLOAT_EQ(input[0], 0.0f);
    EXPECT_FLOAT_EQ(input[1], 1.0f);
    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), 1u);
}

TEST(IsochRxTimingTests,
     EmptyCompletionIsCountedAndInvalidatesReplayExactlyOnce) {
    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = 4,
        .inputChannels = 2,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source, {.am824Slots = 2, .streamChannels = 2});
    const ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 300, 0),
        .drainHostTicks = 1'000'000,
    };
    const ASFW::Isoch::IsochReceivePacket empty{
        .descriptorIndex = 9,
        .transferStatus = 0x11,
        .residualCount = 4096,
        .payload = {},
    };

    consumer.OnReceiveActivated();
    consumer.BeginReceiveBatch(batch);
    ASSERT_EQ(control.rxReplayEpochResets.load(std::memory_order_acquire), 1U);
    consumer.ConsumePacket(batch, empty);

    EXPECT_EQ(control.rxPacketsSeen.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(control.rxEmptyCompletions.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(control.rxShortPackets.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.rxReplayEpochResets.load(std::memory_order_acquire), 2U);
}

TEST(IsochRxTimingTests, ZeroDbsPacketIsAttributedAndInvalidatesReplay) {
    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = 4,
        .inputChannels = 2,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source, {.am824Slots = 2, .streamChannels = 2});
    const ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 300, 0),
        .drainHostTicks = 1'000'000,
    };
    // The valid CIP envelope has a zero data-block-size field. The consumer
    // must keep attributing it as a distinct RX anomaly while preserving the
    // existing replay-reset behavior.
    alignas(4) std::array<uint8_t, 16> payload{};
    payload[0] = 0x23;
    payload[1] = 0xA1;
    WriteBE32(payload.data() + 8, 0x02000000u);
    WriteBE32(payload.data() + 12, 0x9002FFFFu);
    const ASFW::Isoch::IsochReceivePacket packet{
        .descriptorIndex = 101,
        .transferStatus = 0x11,
        .residualCount = 4080,
        .payload = payload,
    };

    consumer.OnReceiveActivated();
    consumer.BeginReceiveBatch(batch);
    ASSERT_EQ(control.rxReplayEpochResets.load(std::memory_order_acquire), 1U);
    consumer.ConsumePacket(batch, packet);

    EXPECT_EQ(control.rxPacketsSeen.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(control.rxZeroDataBlockSize.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(control.rxReplayEpochResets.load(std::memory_order_acquire), 2U);
}

TEST(IsochRxTimingTests,
     MAudioHeaderOnlyNoDataTransitionIsAttributedWithoutResettingReplay) {
    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = 4,
        .inputChannels = 2,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source,
        {.am824Slots = 2,
         .streamChannels = 2,
         .acceptHeaderOnlyNoDataTransition = true});
    const ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 300, 0),
        .drainHostTicks = 1'000'000,
    };
    // Exact M-Audio transition envelope: 8-byte OHCI receive prefix followed
    // by an otherwise-valid 8-byte CIP header with DBS=0 and SYT=ffff.
    alignas(4) std::array<uint8_t, 16> payload{};
    payload[0] = 0x23;
    payload[1] = 0xA1;
    WriteBE32(payload.data() + 8, 0x02000000u);
    WriteBE32(payload.data() + 12, 0x9002FFFFu);
    const ASFW::Isoch::IsochReceivePacket packet{
        .descriptorIndex = 101,
        .transferStatus = 0x11,
        .residualCount = 4080,
        .payload = payload,
    };

    consumer.OnReceiveActivated();
    consumer.BeginReceiveBatch(batch);
    ASSERT_EQ(control.rxReplayEpochResets.load(std::memory_order_acquire), 1U);
    consumer.ConsumePacket(batch, packet);

    EXPECT_EQ(control.rxPacketsSeen.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(control.rxZeroDataBlockSize.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(control.rxNoDataPackets.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(control.rxReplayEpochResets.load(std::memory_order_acquire), 1U);
}

TEST(IsochRxTimingTests, PacketProcessorCanDecodeRawPcm24In32CaptureWhenExplicitlySelected) {
    constexpr size_t kFrames = 1;
    constexpr size_t kDbs = 2;
    alignas(4) std::array<uint8_t, 8 + 8 + (kFrames * kDbs * 4)> packet{};
    FillTwoChannelAmdtpPacket(packet, 0x007FFFFFu, 0xFF800000u);

    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    const ASFW::Audio::Runtime::AudioGraphBinding binding{
        .sampleRateHz = 48000,
        .memory = ASFW::Audio::Runtime::AudioStreamMemory{
            .inputBase = input.data(),
            .inputFrameCapacity = 4,
            .inputChannels = 2,
        },
        .control = &control,
        .deviceToHostAm824Slots = kDbs,
    };

    ASFW::AudioEngine::Direct::DirectInputWriter writer;
    writer.Bind(&binding);
    ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor processor(
        writer);

    const auto result = processor.ProcessPacket(
        packet.data(),
        packet.size(),
        0,
        2,
        kDbs,
        ASFW::Encoding::AudioWireFormat::kRawPcm24In32);

    EXPECT_EQ(result.status,
              ASFW::AudioEngine::Direct::Rx::DirectRxWriteStatus::kAvailable);
    EXPECT_EQ(result.framesDecoded, 1u);
    EXPECT_FLOAT_EQ(input[0], 1.0f);
    EXPECT_FLOAT_EQ(input[1], -1.0f);
}

// RX replay/capture never owns or rebases the HAL coordinate, including on the
// M-Audio path where qualified TX observations publish the hardware timeline.
// The backend's explicit channel/skew policy handles capture placement.
TEST(IsochRxTimingTests, TxDerivedClockDoesNotRebaseReceiveCursor) {
    constexpr size_t kFrames = 1;
    constexpr size_t kDbs = 2;
    constexpr uint64_t kAnchorSampleFrame = 20'000;
    constexpr uint64_t kDrainHostTicks = 1'000'000;

    alignas(4) std::array<uint8_t, 8 + 8 + (kFrames * kDbs * 4)> packet{};
    // Receive timestamp chosen so ExpandReceiveTimestamp yields ageTicks == 0
    // against the drain reference below, making the packet's host time exactly
    // drainHostTicks and the expected projection arithmetic exact.
    const uint16_t receiveTimestamp = static_cast<uint16_t>((5u << 13) | 200u);
    packet[0] = static_cast<uint8_t>(receiveTimestamp & 0xFFu);
    packet[1] = static_cast<uint8_t>(receiveTimestamp >> 8);
    FillTwoChannelAmdtpPacket(packet, 0x40000000u, 0x407FFFFFu);
    // A real SYT: RX may establish replay/capture timing, but cannot move the
    // TX-owned absolute HAL coordinate.
    WriteBE32(packet.data() + 12, 0x90021000u);

    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    // Stand in for the TX-side publisher that owns the HAL timeline here.
    const uint32_t nanosPerSampleQ8 =
        static_cast<uint32_t>((1'000'000'000ULL << 8) / 48'000ULL);
    ASSERT_TRUE(control.PublishHostClockAnchor(
        kAnchorSampleFrame, kDrainHostTicks, nanosPerSampleQ8).accepted);

    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = 4,
        .inputChannels = 2,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source,
        {
            .am824Slots = kDbs,
            .streamChannels = kDbs,
            .useTxDerivedPlaybackClock = true,
        });
    const ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 200, 0),
        .drainHostTicks = kDrainHostTicks,
    };
    const ASFW::Isoch::IsochReceivePacket isochPacket{
        .descriptorIndex = 7,
        .payload = packet,
    };

    consumer.OnReceiveActivated();
    consumer.BeginReceiveBatch(batch);

    consumer.ConsumePacket(batch, isochPacket);
    consumer.ConsumePacket(batch, isochPacket);

    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire),
              kFrames + kFrames);
    EXPECT_EQ(control.captureRingWriteFrame.load(std::memory_order_acquire),
              kFrames + kFrames);
}

// The RX-anchored families publish the host clock anchor from this same cursor,
// so the HAL timeline is defined by it. Re-basing there would be circular and
// must not happen.
TEST(IsochRxTimingTests, RxAnchoredClockLeavesReceiveCursorAtItsOwnOrigin) {
    constexpr size_t kFrames = 1;
    constexpr size_t kDbs = 2;
    alignas(4) std::array<uint8_t, 8 + 8 + (kFrames * kDbs * 4)> packet{};
    const uint16_t receiveTimestamp = static_cast<uint16_t>((5u << 13) | 200u);
    packet[0] = static_cast<uint8_t>(receiveTimestamp & 0xFFu);
    packet[1] = static_cast<uint8_t>(receiveTimestamp >> 8);
    FillTwoChannelAmdtpPacket(packet, 0x40000000u, 0x407FFFFFu);
    WriteBE32(packet.data() + 12, 0x90021000u);

    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    const uint32_t nanosPerSampleQ8 =
        static_cast<uint32_t>((1'000'000'000ULL << 8) / 48'000ULL);
    ASSERT_TRUE(control.PublishHostClockAnchor(
        20'000, 1'000'000, nanosPerSampleQ8).accepted);

    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = 4,
        .inputChannels = 2,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source,
        {
            .am824Slots = kDbs,
            .streamChannels = kDbs,
            .useTxDerivedPlaybackClock = false,
        });
    const ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 200, 0),
        .drainHostTicks = 1'000'000,
    };
    const ASFW::Isoch::IsochReceivePacket isochPacket{
        .descriptorIndex = 7,
        .payload = packet,
    };

    consumer.OnReceiveActivated();
    consumer.BeginReceiveBatch(batch);
    consumer.ConsumePacket(batch, isochPacket);
    consumer.ConsumePacket(batch, isochPacket);

    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire),
              2u * kFrames);
}

// The delay half of a capture map is implemented by writing a channel to a
// later absolute frame rather than by holding a history buffer. This exercises
// that through the real writer: the permuted undelayed channels must land in
// the current frame, the delayed one exactly `delayFrames` further on, and the
// head of the delay line must be silenced rather than inheriting whatever the
// shared input buffer already held.
TEST(IsochRxTimingTests, CaptureChannelMapPermutesAndDefersDelayedChannels) {
    constexpr uint32_t kChannels = 4;
    constexpr uint32_t kDbs = 4;
    constexpr uint32_t kDelayFrames = 2;
    // channel -> slot, with channel 1 fed by slot 2 and deferred.
    static constexpr std::array<uint8_t, kChannels> kSlots{0, 2, 1, 3};

    alignas(4) std::array<uint8_t, 8 + 8 + (kDbs * 4)> packet{};
    const uint16_t receiveTimestamp = static_cast<uint16_t>((5u << 13) | 200u);
    packet[0] = static_cast<uint8_t>(receiveTimestamp & 0xFFu);
    packet[1] = static_cast<uint8_t>(receiveTimestamp >> 8);
    WriteBE32(packet.data() + 8, 0x02040000u);   // SID 2, DBS 4, DBC 0
    WriteBE32(packet.data() + 12, 0x90021000u);  // AM824, 48k, real SYT
    for (uint32_t slot = 0; slot < kDbs; ++slot) {
        WriteBE32(packet.data() + 16 + (slot * 4), 0x40000000u | ((slot + 1u) << 8));
    }

    constexpr uint32_t kFrames = 16;
    std::array<float, kFrames * kChannels> input{};
    // Poison the buffer so an unprimed delay head is visible as a failure.
    input.fill(-1.0f);

    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = kFrames,
        .inputChannels = kChannels,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });

    ASFW::AudioEngine::Direct::Rx::RxCaptureChannelMap map{};
    ASSERT_TRUE(map.SetSlots(kSlots));
    map.channelCount = kChannels;
    map.delayFrames = kDelayFrames;
    map.delayedChannelMask = 1u << 1;

    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source,
        {
            .am824Slots = kDbs,
            .streamChannels = kChannels,
            .captureChannelMap = map,
        });
    const ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 200, 0),
        .drainHostTicks = 1'000'000,
    };
    const ASFW::Isoch::IsochReceivePacket isochPacket{
        .descriptorIndex = 7,
        .payload = packet,
    };

    consumer.OnReceiveActivated();
    consumer.BeginReceiveBatch(batch);
    consumer.ConsumePacket(batch, isochPacket);

    const auto expectedFor = [](uint32_t slot) {
        return static_cast<float>((slot + 1u) << 8) / 8388607.0f;
    };

    // Frame 0 carries the undelayed channels, permuted.
    EXPECT_FLOAT_EQ(input[0 * kChannels + 0], expectedFor(kSlots[0]));
    EXPECT_FLOAT_EQ(input[0 * kChannels + 2], expectedFor(kSlots[2]));
    EXPECT_FLOAT_EQ(input[0 * kChannels + 3], expectedFor(kSlots[3]));
    // Its delayed channel has no predecessor, so priming must have zeroed it.
    EXPECT_FLOAT_EQ(input[0 * kChannels + 1], 0.0f);
    EXPECT_FLOAT_EQ(input[1 * kChannels + 1], 0.0f);
    // ...and the sample itself lands `delayFrames` later.
    EXPECT_FLOAT_EQ(input[kDelayFrames * kChannels + 1], expectedFor(kSlots[1]));

    // The producer cursor still ends at the undelayed frontier.
    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), 1u);
}
