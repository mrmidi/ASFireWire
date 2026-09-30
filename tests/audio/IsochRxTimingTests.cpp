#include <gtest/gtest.h>

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "Audio/Engine/Direct/DirectInputWriter.hpp"
#include "Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.hpp"
#include "Audio/Engine/Direct/Rx/RxAudioPacketProcessor.hpp"
#include "Audio/Wire/AM824/Am824PayloadCodec.hpp"
#include "Audio/Wire/RawPcm24In32/RawPcm24In32PayloadCodec.hpp"
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
    ASFW::Audio::Wire::Am824RxPayloadCodec codec(kDbs);
    const auto result = processor.ProcessPacket(
        packet.data(),
        packet.size(),
        0,
        2,
        codec);

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

    ASFW::Audio::Wire::Am824RxPayloadCodec codec(kDbs);
    const auto result = processor.ProcessPacket(
        packet.data(),
        packet.size(),
        0,
        2,
        codec);

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

namespace {

// Two consecutive DATA packets (valid SYT) of one 2-channel frame each, one
// isochronous cycle apart, drained together.
struct TwoDataPackets {
    alignas(4) std::array<uint8_t, 24> first{};
    alignas(4) std::array<uint8_t, 24> second{};

    TwoDataPackets() {
        for (auto* packet : {&first, &second}) {
            WriteBE32(packet->data() + 8, 0x02020000u);
            WriteBE32(packet->data() + 12, 0x90020123u); // FDF 48 kHz, SYT 0x0123
            WriteBE32(packet->data() + 16, 0x40000000u);
            WriteBE32(packet->data() + 20, 0x407FFFFFu);
        }
        first[0] = 0x23;  first[1] = 0xA1;   // cycle 291, seconds 5 (mod 8)
        second[0] = 0x24; second[1] = 0xA1;  // cycle 292
    }
};

constexpr uint64_t kAnchorSampleFrame = 1000;
constexpr uint32_t kNanosPerSampleQ8 = static_cast<uint32_t>((1'000'000'000ULL << 8) / 48000);

// The frame the driver must put the first decoded frame at: the anchor's frame
// plus the arrival's distance from the anchor, in samples.
uint64_t ExpectedFirstFrame(uint64_t anchorHostTicks, uint64_t packetHostTicks) {
    const int64_t deltaTicks =
        static_cast<int64_t>(packetHostTicks) - static_cast<int64_t>(anchorHostTicks);
    const int64_t deltaNanos = deltaTicks >= 0
        ? static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(static_cast<uint64_t>(deltaTicks)))
        : -static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(static_cast<uint64_t>(-deltaTicks)));
    return static_cast<uint64_t>(static_cast<int64_t>(kAnchorSampleFrame) +
                                 (deltaNanos << 8) / static_cast<int64_t>(kNanosPerSampleQ8));
}

} // namespace

// M-Audio special firmware: a Transmit epoch owns the HAL clock and RX publishes
// no anchor. The receive cursor used to start at 0 while the HAL read at the
// TX-published frame for "now" (hundreds of ms apart), so every capture frame
// was zero-filled while all counters stayed healthy (midi 6af4809a; ProjectMix
// I/O silent on 0.3.1). The first DATA packet must give the cursor the anchor's
// origin, once, and the next packet must land there.
TEST(IsochRxTimingTests, TransmitOwnedClockGivesReceiveCursorTheHalOrigin) {
    TwoDataPackets packets;
    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    const uint64_t epoch = control.hardwareTimeline.BeginEpoch(
        ASFW::Audio::Runtime::HardwareTimelineSource::Transmit,
        ASFW::Audio::Runtime::HardwareTimelineDiscontinuity::StartIO, 48000, 0);
    ASSERT_NE(epoch, 0u);
    constexpr uint64_t kDrainHostTicks = 2'000'000'000ULL;
    const uint64_t anchorHostTicks = kDrainHostTicks - 10'000'000ULL;
    ASSERT_TRUE(control.PublishHostClockAnchor(kAnchorSampleFrame, anchorHostTicks,
                                               kNanosPerSampleQ8, epoch).accepted);

    FixedDirectAudioBindingSource source({
        .generation = 1, .inputBase = input.data(), .inputBytes = sizeof(input),
        .inputFrames = 4, .inputChannels = 2, .control = &control,
        .sampleRateHz = 48000, .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source, {.am824Slots = 2, .streamChannels = 2});
    uint32_t timingLosses = 0;
    consumer.SetTimingLossCallback([&] { ++timingLosses; });
    const ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 300, 0), .drainHostTicks = kDrainHostTicks,
    };

    consumer.OnReceiveActivated();
    consumer.BeginReceiveBatch(batch);
    consumer.ConsumePacket(batch, {.descriptorIndex = 1, .payload = packets.first});
    consumer.ConsumePacket(batch, {.descriptorIndex = 2, .payload = packets.second});

    // Packet 1 anchors (its own PCM is orphaned at the old origin); packet 2 is
    // the first frame written at the HAL's origin.
    const uint64_t firstFrame = ExpectedFirstFrame(
        anchorHostTicks,
        kDrainHostTicks - ASFW::Timing::nanosToHostTicks(ASFW::Isoch::Rx::FireWireTicksToNanos(
                              9ULL * ASFW::Timing::kTicksPerCycle)));
    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), firstFrame + 2u);
    EXPECT_EQ(timingLosses, 0u) << "a rebase is not a timing loss";
}

// Every other family: RX publishes the anchor from its own cursor, so the two
// share an origin by construction and the cursor must not be moved.
TEST(IsochRxTimingTests, ReceiveOwnedClockKeepsItsOwnCursorOrigin) {
    for (const bool withEpoch : {false, true}) {
        TwoDataPackets packets;
        std::array<float, 8> input{};
        ASFW::Audio::Runtime::AudioTransportControlBlock control{};
        uint64_t epoch = 0;
        if (withEpoch) {
            epoch = control.hardwareTimeline.BeginEpoch(
                ASFW::Audio::Runtime::HardwareTimelineSource::Receive,
                ASFW::Audio::Runtime::HardwareTimelineDiscontinuity::StartIO, 48000, 0);
        }
        ASSERT_TRUE(control.PublishHostClockAnchor(kAnchorSampleFrame, 1'990'000'000ULL,
                                                   kNanosPerSampleQ8, epoch).accepted);
        FixedDirectAudioBindingSource source({
            .generation = 1, .inputBase = input.data(), .inputBytes = sizeof(input),
            .inputFrames = 4, .inputChannels = 2, .control = &control,
            .sampleRateHz = 48000, .valid = true,
        });
        ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
            &source, {.am824Slots = 2, .streamChannels = 2});
        const ASFW::Isoch::IsochReceiveBatch batch{
            .drainCycleTimer = EncodeCycleTimer(13, 300, 0), .drainHostTicks = 2'000'000'000ULL,
        };
        consumer.OnReceiveActivated();
        consumer.BeginReceiveBatch(batch);
        consumer.ConsumePacket(batch, {.descriptorIndex = 1, .payload = packets.first});
        consumer.ConsumePacket(batch, {.descriptorIndex = 2, .payload = packets.second});
        EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), 2u)
            << "withEpoch=" << withEpoch;
    }
}

// No anchor yet, or one from an epoch that has ended: stay unanchored rather
// than commit to an origin the HAL would refuse, and anchor once a live one
// exists.
TEST(IsochRxTimingTests, TransmitOwnedClockWaitsForALiveAnchor) {
    TwoDataPackets packets;
    std::array<float, 8> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    const uint64_t epoch = control.hardwareTimeline.BeginEpoch(
        ASFW::Audio::Runtime::HardwareTimelineSource::Transmit,
        ASFW::Audio::Runtime::HardwareTimelineDiscontinuity::StartIO, 48000, 0);
    FixedDirectAudioBindingSource source({
        .generation = 1, .inputBase = input.data(), .inputBytes = sizeof(input),
        .inputFrames = 4, .inputChannels = 2, .control = &control,
        .sampleRateHz = 48000, .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source, {.am824Slots = 2, .streamChannels = 2});
    const ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 300, 0), .drainHostTicks = 2'000'000'000ULL,
    };
    consumer.OnReceiveActivated();
    consumer.BeginReceiveBatch(batch);

    consumer.ConsumePacket(batch, {.descriptorIndex = 1, .payload = packets.first});
    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), 1u)
        << "no anchor published: cursor keeps its own origin";

    ASSERT_TRUE(control.PublishHostClockAnchor(kAnchorSampleFrame, 1'990'000'000ULL,
                                               kNanosPerSampleQ8, epoch + 7).accepted);
    consumer.ConsumePacket(batch, {.descriptorIndex = 2, .payload = packets.second});
    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), 2u)
        << "anchor from another epoch: refused";
}

TEST(IsochRxTimingTests, SyntheticHeaderlessReplayHandlesWrapOneGapAndLargeLoss) {
    std::array<float, 4096> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = 2048,
        .inputChannels = 2,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source, {.wireFormat = ASFW::Encoding::AudioWireFormat::kRawPcm24Upper24In32LE,
                 .framing = ASFW::Encoding::AudioPacketFraming::kHeaderless,
                 .am824Slots = 2,
                 .streamChannels = 2});
    uint32_t recoveries = 0;
    consumer.SetTimingLossCallback([&] { ++recoveries; });
    consumer.OnReceiveActivated();
    ASFW::Isoch::IsochReceivePacket packet{.descriptorIndex = 0};
    std::array<uint8_t, 24> bytes{}; // 8-byte OHCI/isoch prefix + two PCM frames
    packet.payload = bytes;

    auto deliver = [&](uint32_t seconds, uint32_t cycle) {
        const uint32_t timer = EncodeCycleTimer(seconds, cycle, 0);
        const uint16_t raw = static_cast<uint16_t>(((seconds & 7u) << 13) | cycle);
        bytes[0] = static_cast<uint8_t>(raw);
        bytes[1] = static_cast<uint8_t>(raw >> 8);
        const ASFW::Isoch::IsochReceiveBatch batch{
            .drainCycleTimer = timer,
            .drainHostTicks = uint64_t{seconds} * 1'000'000'000ULL + uint64_t{cycle} * 125'000ULL,
        };
        consumer.BeginReceiveBatch(batch);
        consumer.ConsumePacket(batch, packet);
    };

    // Cross the 128-second hardware timestamp wrap with consecutive cycles.
    uint32_t seconds = 127;
    uint32_t cycle = 7743;
    for (uint32_t i = 0;
         i < ASFW::Audio::Runtime::RxSequenceReplayState::kReadDelay;
         ++i) {
        deliver(seconds, cycle);
        if (++cycle == 8000) { cycle = 0; seconds = (seconds + 1) % 128; }
    }
    ASSERT_TRUE(control.rxSequenceReplay.IsEstablished());
    const uint64_t beforeSingleGap = control.rxSequenceReplay.ProducerCursor();
    uint32_t missingSeconds = seconds;
    uint32_t missingCycle = cycle;
    cycle = missingCycle + 1; // one absent RX cycle; represented as zero blocks
    if (cycle == 8000) { cycle = 0; seconds = (seconds + 1) % 128; }
    deliver(seconds, cycle);
    EXPECT_EQ(control.rxSequenceReplay.ProducerCursor(), beforeSingleGap + 2);
    EXPECT_EQ(recoveries, 0U);
    ASFW::Audio::Runtime::RxSequenceEntry missing{};
    ASFW::Audio::Runtime::RxSequenceEntry observed{};
    ASSERT_TRUE(control.rxSequenceReplay.Read(
        beforeSingleGap, control.rxSequenceReplay.Epoch(), missing));
    ASSERT_TRUE(control.rxSequenceReplay.Read(
        beforeSingleGap + 1, control.rxSequenceReplay.Epoch(), observed));
    EXPECT_EQ(missing.dataBlocks, 0U);
    EXPECT_EQ(missing.sourceCycleTimer, EncodeCycleTimer(missingSeconds, missingCycle, 0));
    EXPECT_EQ(missing.firstAudioFrame, observed.firstAudioFrame);

    const uint64_t afterTwoMissing = uint64_t{seconds} * 8000 + cycle + 3;
    seconds = static_cast<uint32_t>((afterTwoMissing / 8000) % 128);
    cycle = static_cast<uint32_t>(afterTwoMissing % 8000);
    deliver(seconds, cycle);
    EXPECT_EQ(recoveries, 1U);
    EXPECT_FALSE(control.rxSequenceReplay.IsEstablished());
}

TEST(IsochRxTimingTests, PacketProcessorAddsAM824LabelForRawSaffireCapture) {
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

    ASFW::Audio::Wire::RawPcm24In32RxPayloadCodec codec(kDbs);
    const auto result = processor.ProcessPacket(
        packet.data(),
        packet.size(),
        0,
        2,
        codec);

    EXPECT_EQ(result.status,
              ASFW::AudioEngine::Direct::Rx::DirectRxWriteStatus::kAvailable);
    EXPECT_EQ(result.framesDecoded, 1u);
    EXPECT_FLOAT_EQ(input[0], 1.0f);
    EXPECT_FLOAT_EQ(input[1], -1.0f);
}
