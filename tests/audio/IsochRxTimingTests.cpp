#include <gtest/gtest.h>

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "Audio/Engine/Direct/DirectInputWriter.hpp"
#include "Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.hpp"
#include "Audio/Engine/Direct/Rx/RxAudioPacketProcessor.hpp"
#include "Audio/Wire/AM824/Am824PayloadCodec.hpp"
#include "Audio/Wire/RawPcm24In32/RawPcm24In32PayloadCodec.hpp"
#include "Audio/Engine/Direct/Rx/DirectRxPacketDecoder.hpp"
#include "Audio/Wire/MOTU/MotuPayloadCodec.hpp"
#include "Audio/Wire/MOTU/MotuV3DeviceTiming.hpp"
#include "Audio/Protocols/MOTU/MotuSphClockServo.hpp"
#include "Audio/Wire/AMDTP/MotuV3WireFormat.hpp"
#include "Shared/Isoch/AudioHalBufferProfiles.hpp"
#include "Isoch/Receive/IsochRxTiming.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

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

    void SetGeneration(uint64_t generation) noexcept {
        snapshot_.generation = generation;
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

//==============================================================================
// MOTU protocol-v3 capture (828 Mk3), behind the generic receive seams: the V2
// payload codec decodes the blocks, kMotuV3Header framing owns the header, and
// MotuV3RxTimingObserver gates establishment.
//==============================================================================

namespace {

constexpr std::array<uint8_t, 8> kMotuV3CaptureHeader{
    0x0d, 0x04, 0x04, 0x00, 0x22, 0xff, 0xff, 0xff,
};

}  // namespace

TEST(IsochRxTimingTests, MotuV3FramingDecodesCaptureThroughTheV2Codec) {
    constexpr size_t kFrames = 2;
    constexpr size_t kChannels = 18;
    constexpr size_t kDbs = 16;  // MotuV3Dbs(18) == DataBlockQuadlets(18)
    constexpr size_t kBlockBytes = kDbs * 4;
    alignas(4) std::array<uint8_t, 8 + 8 + kFrames * kBlockBytes> packet{};
    std::copy(kMotuV3CaptureHeader.begin(), kMotuV3CaptureHeader.end(), packet.begin() + 8);
    WriteBE32(packet.data() + 16, 0x12345678u);

    // SPH(4) + two message chunks(6), then signed 24-bit big-endian PCM.
    uint8_t* frame0 = packet.data() + 16 + 10;
    frame0[0] = 0x7f; frame0[1] = 0xff; frame0[2] = 0xff;
    frame0[3] = 0x80; frame0[4] = 0x00; frame0[5] = 0x00;
    uint8_t* frame1 = packet.data() + 16 + kBlockBytes + 10;
    frame1[0] = 0x40; frame1[1] = 0x00; frame1[2] = 0x00;

    std::array<float, kFrames * kChannels> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    const ASFW::Audio::Runtime::AudioGraphBinding binding{
        .sampleRateHz = 48000,
        .memory = ASFW::Audio::Runtime::AudioStreamMemory{
            .inputBase = input.data(),
            .inputFrameCapacity = kFrames,
            .inputChannels = kChannels,
        },
        .control = &control,
        .deviceToHostAm824Slots = kDbs,
    };
    ASFW::AudioEngine::Direct::DirectInputWriter writer;
    writer.Bind(&binding);
    ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor processor(writer);
    const ASFW::Audio::Wire::MotuRxPayloadCodec codec(kChannels);

    const auto result = processor.ProcessPacket(
        packet.data(), packet.size(), 0, kChannels, codec, 0, true, {}, false,
        ASFW::Encoding::AudioPacketFraming::kMotuV3Header);

    EXPECT_EQ(result.status, ASFW::AudioEngine::Direct::Rx::DirectRxWriteStatus::kAvailable);
    EXPECT_TRUE(result.hasValidCip);
    EXPECT_TRUE(result.hasMotuSph);
    EXPECT_EQ(result.firstMotuSph, 0x12345678u);
    EXPECT_EQ(result.syt, 0xffffu);
    EXPECT_EQ(result.fdf, 0x22u);
    EXPECT_EQ(result.strideQuadlets, kDbs);
    EXPECT_EQ(result.framesDecoded, kFrames);
    EXPECT_FLOAT_EQ(input[0], 1.0f);
    EXPECT_FLOAT_EQ(input[1], -1.0f);
    EXPECT_FLOAT_EQ(input[kChannels], static_cast<float>(0x400000) / 8388607.0f);
    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), kFrames);
}

TEST(IsochRxTimingTests, MotuV3FramingRejectsAnyOtherHeaderAndCipRejectsV3) {
    constexpr size_t kChannels = 18;
    constexpr size_t kDbs = 16;
    alignas(4) std::array<uint8_t, 8 + 8 + kDbs * 4> packet{};
    std::copy(kMotuV3CaptureHeader.begin(), kMotuV3CaptureHeader.end(), packet.begin() + 8);
    ASFW::AudioEngine::Direct::DirectInputWriter writer;
    ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor processor(writer);
    const ASFW::Audio::Wire::MotuRxPayloadCodec codec(kChannels);

    // The reason the framing exists: EOH1 is clear, so IEC 61883 decoding fails.
    const auto asCip = processor.ProcessPacket(packet.data(), packet.size(), 0, kChannels, codec);
    EXPECT_EQ(asCip.status, ASFW::AudioEngine::Direct::Rx::DirectRxWriteStatus::kInvalidCipHeader);

    for (size_t i = 0; i < kMotuV3CaptureHeader.size(); ++i) {
        auto mutated = packet;
        mutated[8 + i] ^= 0x01;
        const auto result = processor.ProcessPacket(
            mutated.data(), mutated.size(), 0, kChannels, codec, 0, true, {}, false,
            ASFW::Encoding::AudioPacketFraming::kMotuV3Header);
        EXPECT_EQ(result.status, ASFW::AudioEngine::Direct::Rx::DirectRxWriteStatus::kInvalidCipHeader)
            << "header byte " << i;
    }
}

TEST(IsochRxTimingTests, MotuV3FramingRejectsPayloadThatIsNotWholeBlocks) {
    // 19 PCM chunks need 17-quadlet blocks; a packet of 16-quadlet blocks cannot
    // hold them.
    constexpr size_t kDbs = 16;
    std::array<uint8_t, 8 + 8 + kDbs * 4> packet{};
    std::copy(kMotuV3CaptureHeader.begin(), kMotuV3CaptureHeader.end(), packet.begin() + 8);
    ASFW::AudioEngine::Direct::DirectInputWriter writer;
    ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor processor(writer);
    const ASFW::Audio::Wire::MotuRxPayloadCodec codec(19);

    const auto result = processor.ProcessPacket(
        packet.data(), packet.size(), 0, 19, codec, 0, true, {}, false,
        ASFW::Encoding::AudioPacketFraming::kMotuV3Header);

    EXPECT_EQ(result.status, ASFW::AudioEngine::Direct::Rx::DirectRxWriteStatus::kGeometryMismatch);
}

TEST(IsochRxTimingTests, MotuV2CodecDecodesV3BlocksBitForBit) {
    // V3 capture reuses the V2 codec on the grounds that the block layout and
    // DBS formula are identical. Pin it on
    // random PCM for every 828 Mk3 capture width (18 at 1x, 14 at 2x, 10 at 4x).
    std::mt19937 rng(0x828u);
    for (const uint32_t channels : {18u, 14u, 10u}) {
        const ASFW::Audio::Wire::MotuRxPayloadCodec codec(channels);
        const uint32_t strideQuadlets = codec.StrideQuadlets(0);
        ASSERT_EQ(strideQuadlets, 1u + ((2u + channels) * 3u + 3u) / 4u) << channels;
        std::vector<uint8_t> block(static_cast<size_t>(strideQuadlets) * 4u);
        for (int trial = 0; trial < 256; ++trial) {
            for (auto& byte : block) {
                byte = static_cast<uint8_t>(rng());
            }
            std::vector<float> fromCodec(channels, 0.0f);
            std::vector<float> fromLegacy(channels, 0.0f);
            codec.DecodeBlock(block, channels, 0, {}, fromCodec.data(), nullptr);
            ASFW::AudioEngine::Direct::Rx::DecodeMotuV3Frame(block.data() + 10, channels,
                                                             fromLegacy.data());
            for (uint32_t ch = 0; ch < channels; ++ch) {
                ASSERT_EQ(std::memcmp(&fromCodec[ch], &fromLegacy[ch], sizeof(float)), 0)
                    << "channels=" << channels << " ch=" << ch << " trial=" << trial;
            }
        }
    }
}

TEST(IsochRxTimingTests, MotuV3ObserverEstablishesOnTheFirstDataPacketAndResets) {
    ASFW::Audio::Wire::MotuV3RxTimingObserver observer;
    EXPECT_FALSE(observer.IsTimingEstablished());
    observer.ObservePacket({}, 16, 0, 300);
    EXPECT_FALSE(observer.IsTimingEstablished());
    observer.ObservePacket({}, 16, 8, 301);
    EXPECT_TRUE(observer.IsTimingEstablished());
    observer.Reset();
    EXPECT_FALSE(observer.IsTimingEstablished());
}

TEST(IsochRxTimingTests, MotuV3CaptureEstablishesZtsAndReleasesDeferredTx) {
    constexpr size_t kFramesPerPacket = 8;
    constexpr size_t kChannels = 18;
    constexpr size_t kDbs = 16;
    constexpr size_t kBlockBytes = kDbs * 4;
    constexpr size_t kMaxPackets = 256;  // input ring depth, in packets
    alignas(4) std::array<uint8_t, 8 + 8 + kFramesPerPacket * kBlockBytes> packet{};
    std::copy(kMotuV3CaptureHeader.begin(), kMotuV3CaptureHeader.end(), packet.begin() + 8);

    std::array<float, kMaxPackets * kFramesPerPacket * kChannels> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = kMaxPackets * kFramesPerPacket,
        .inputChannels = kChannels,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source,
        {
            .wireFormat = ASFW::Encoding::AudioWireFormat::kMotuV3Packed,
            .framing = ASFW::Encoding::AudioPacketFraming::kMotuV3Header,
            .am824Slots = kDbs,
            .streamChannels = kChannels,
        });
    const ASFW::Audio::Wire::MotuRxPayloadCodec codec(kChannels);
    ASFW::Audio::Wire::MotuV3RxTimingObserver observer;
    consumer.SetPayloadCodec(&codec);
    consumer.SetTimingObserver(&observer);

    // The consumer releases the deferred transmit start as soon as the observer
    // gate opens, and marks the replay ring established only once it holds
    // kReadDelay entries. V3 transmit does not read the replay ring, so the
    // earlier release is harmless here, but the order is pinned so a change to
    // it is a visible decision.
    constexpr uint32_t kPackets = 300;
    bool replayReady = false;
    uint32_t replayReadyAtPacket = UINT32_MAX;
    uint32_t establishedAtPacket = UINT32_MAX;
    consumer.SetReplayReadyCallback([&replayReady] { replayReady = true; });
    consumer.OnReceiveActivated();

    for (uint32_t i = 0; i < kPackets; ++i) {
        const uint32_t cycle = 300u + i;
        const uint16_t rawTimestamp = static_cast<uint16_t>((5u << 13) | cycle);
        packet[0] = static_cast<uint8_t>(rawTimestamp & 0xffu);
        packet[1] = static_cast<uint8_t>(rawTimestamp >> 8);
        const ASFW::Isoch::IsochReceiveBatch batch{
            .drainCycleTimer = EncodeCycleTimer(13, cycle, 64),
            // Eight 48 kHz frames span about 166.667 us; keep the synthetic host
            // clock on that cadence so the anchor plausibility filter is not
            // what is being tested.
            .drainHostTicks = 1'000'000 + i * 166'667,
        };
        const ASFW::Isoch::IsochReceivePacket isochPacket{
            .descriptorIndex = i,
            .payload = packet,
        };
        consumer.BeginReceiveBatch(batch);
        consumer.ConsumePacket(batch, isochPacket);
        if (replayReady && replayReadyAtPacket == UINT32_MAX) {
            replayReadyAtPacket = i;
        }
        if (control.rxSequenceReplay.IsEstablished() && establishedAtPacket == UINT32_MAX) {
            establishedAtPacket = i;
        }
    }

    // V3 sends SYT NO_INFO, so an SYT cadence never establishes; the observer
    // gate alone must release the deferred transmit start and the ZTS anchor.
    EXPECT_EQ(replayReadyAtPacket, 0u);
    ASSERT_NE(establishedAtPacket, UINT32_MAX);
    EXPECT_GT(establishedAtPacket, replayReadyAtPacket);
    EXPECT_GE(control.counters.ztsRxPublished.load(std::memory_order_acquire), 1u);
}

// The RX half of the oracle capture, recorded by the consumer. Every packet is
// recorded -- NO-DATA and rejected ones too, because the reference capture of
// the official driver counted them -- numbered by the consumer from zero.
TEST(IsochRxTimingTests, MotuV3ConsumerRecordsTheOracleRxHalfForEveryPacket) {
    constexpr size_t kFramesPerPacket = 8;
    constexpr size_t kChannels = 18;
    constexpr size_t kDbs = 16;
    constexpr size_t kBlockBytes = kDbs * 4;
    constexpr size_t kMaxPackets = 16;
    std::array<float, kMaxPackets * kFramesPerPacket * kChannels> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = kMaxPackets * kFramesPerPacket,
        .inputChannels = kChannels,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source,
        {
            .wireFormat = ASFW::Encoding::AudioWireFormat::kMotuV3Packed,
            .framing = ASFW::Encoding::AudioPacketFraming::kMotuV3Header,
            .am824Slots = kDbs,
            .streamChannels = kChannels,
        });
    const ASFW::Audio::Wire::MotuRxPayloadCodec codec(kChannels);
    consumer.SetPayloadCodec(&codec);
    consumer.OnReceiveActivated();

    alignas(4) std::array<uint8_t, 8 + 8 + kFramesPerPacket * kBlockBytes> data{};
    std::copy(kMotuV3CaptureHeader.begin(), kMotuV3CaptureHeader.end(), data.begin() + 8);
    alignas(4) std::array<uint8_t, 8 + 8> noData{};
    std::copy(kMotuV3CaptureHeader.begin(), kMotuV3CaptureHeader.end(), noData.begin() + 8);
    alignas(4) std::array<uint8_t, 8 + 8> rejected = noData;
    rejected[8 + 4] = 0x02; // FDF 0x02 is not the V3 capture header

    const auto consume = [&](auto& bytes, uint32_t cycle, uint32_t sph, uint16_t status,
                             uint16_t residual) {
        uint8_t* writable = bytes.data();
        const uint16_t rawTimestamp = static_cast<uint16_t>((5u << 13) | cycle);
        writable[0] = static_cast<uint8_t>(rawTimestamp & 0xffu);
        writable[1] = static_cast<uint8_t>(rawTimestamp >> 8);
        if (bytes.size() > 16) {
            writable[16] = static_cast<uint8_t>(sph >> 24);
            writable[17] = static_cast<uint8_t>(sph >> 16);
            writable[18] = static_cast<uint8_t>(sph >> 8);
            writable[19] = static_cast<uint8_t>(sph);
        }
        const ASFW::Isoch::IsochReceiveBatch batch{
            .drainCycleTimer = EncodeCycleTimer(13, cycle, 64),
            .drainHostTicks = 1'000'000 + cycle * 125'000ULL,
        };
        const ASFW::Isoch::IsochReceivePacket packet{
            .descriptorIndex = cycle,
            .transferStatus = status,
            .residualCount = residual,
            .payload = std::span<const uint8_t>(bytes.data(), bytes.size()),
        };
        consumer.BeginReceiveBatch(batch);
        consumer.ConsumePacket(batch, packet);
    };
    consume(data, 300, 0x0012c345u, 0x8451, 3568);
    consume(noData, 301, 0, 0x8451, 4080);
    consume(rejected, 302, 0, 0x8451, 4080);
    consume(data, 303, 0x0013f200u, 0x8451, 3568);

    const auto& rx = control.isochOracleCapture.Rx();
    ASSERT_EQ(rx.Count(), 4u);
    ASFW::Audio::Runtime::IsochOracleRecord record{};
    using ASFW::Audio::Runtime::kIsochOracleFlagData;
    using ASFW::Audio::Runtime::kIsochOracleFlagHasCycle;
    using ASFW::Audio::Runtime::kIsochOracleFlagHasSph;
    using ASFW::Audio::Runtime::kIsochOracleFlagValidCip;

    ASSERT_TRUE(rx.ReadRecord(0, record));
    EXPECT_EQ(record.packetIndex, 0u);
    EXPECT_EQ(record.wireLengthBytes, data.size());
    EXPECT_EQ(record.transferStatus, 0x8451);
    EXPECT_EQ(record.residualCount, 3568);
    EXPECT_EQ(record.firstSph, 0x0012c345u);
    EXPECT_EQ(record.cycleTimestamp, (5u << 13) | 300u);
    EXPECT_EQ(record.flags & (kIsochOracleFlagData | kIsochOracleFlagHasSph |
                              kIsochOracleFlagHasCycle | kIsochOracleFlagValidCip),
              kIsochOracleFlagData | kIsochOracleFlagHasSph | kIsochOracleFlagHasCycle |
                  kIsochOracleFlagValidCip);

    ASSERT_TRUE(rx.ReadRecord(1, record));
    EXPECT_EQ(record.packetIndex, 1u);
    EXPECT_EQ(record.wireLengthBytes, noData.size());
    EXPECT_EQ(record.flags & (kIsochOracleFlagData | kIsochOracleFlagHasSph), 0);
    EXPECT_NE(record.flags & kIsochOracleFlagValidCip, 0);

    ASSERT_TRUE(rx.ReadRecord(2, record));
    EXPECT_EQ(record.packetIndex, 2u);
    EXPECT_EQ(record.flags & (kIsochOracleFlagData | kIsochOracleFlagValidCip), 0);

    ASSERT_TRUE(rx.ReadRecord(3, record));
    EXPECT_EQ(record.packetIndex, 3u);
    EXPECT_EQ(record.firstSph, 0x0013f200u);

    // Health attribution counts DATA by data blocks, not by SYT: every V3
    // header says SYT 0xFFFF. The rejected header is not a valid CIP at all.
    EXPECT_EQ(control.rxPacketsSeen.load(), 4u);
    EXPECT_EQ(control.rxDataPackets.load(), 2u);
    EXPECT_EQ(control.rxNoDataPackets.load(), 1u);
    EXPECT_EQ(control.rxInvalidCipHeaders.load(), 1u);
}

// The oracle is MOTU protocol-v3's: other families' packet paths stay free of it.
TEST(IsochRxTimingTests, NonMotuConsumerRecordsNoOracleRxHalf) {
    constexpr size_t kChannels = 2;
    constexpr size_t kMaxFrames = 64;
    std::array<float, kMaxFrames * kChannels> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = kMaxFrames,
        .inputChannels = kChannels,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source,
        {
            .wireFormat = ASFW::Encoding::AudioWireFormat::kAM824,
            .am824Slots = kChannels,
            .streamChannels = kChannels,
        });
    consumer.OnReceiveActivated();
    // A header-only AM824 CIP packet: valid, NO-DATA.
    alignas(4) std::array<uint8_t, 8 + 8> packet{};
    constexpr std::array<uint8_t, 8> kHeader{0x00, 0x02, 0x00, 0x00, 0x90, 0x02, 0xff, 0xff};
    std::copy(kHeader.begin(), kHeader.end(), packet.begin() + 8);
    const ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 300, 64),
        .drainHostTicks = 1'000'000,
    };
    consumer.BeginReceiveBatch(batch);
    consumer.ConsumePacket(batch, {.descriptorIndex = 0, .payload = packet});
    EXPECT_GE(control.rxPacketsSeen.load(), 1u);
    EXPECT_EQ(control.rxNoDataPackets.load(), 1u);
    EXPECT_EQ(control.rxDataPackets.load(), 0u);
    EXPECT_EQ(control.isochOracleCapture.Rx().Count(), 0u);
}

// The cumulative clock comes from MotuV3RxTimingObserver, bound behind the
// consumer's seam, and the consumer's Reset() on quiesce/activation must not
// break it -- only a new control generation does.
TEST(IsochRxTimingTests,
     MotuReceiveReactivationKeepsClockBridgeMonotonicWithinStartGeneration) {
    constexpr size_t kFramesPerPacket = 8;
    constexpr size_t kChannels = 18;
    constexpr size_t kDbs = 16;
    constexpr size_t kBlockBytes = kDbs * 4;
    alignas(4) std::array<uint8_t,
                          8 + 8 + kFramesPerPacket * kBlockBytes> packet{};
    std::copy(kMotuV3CaptureHeader.begin(), kMotuV3CaptureHeader.end(), packet.begin() + 8);

    std::array<float, 4 * kFramesPerPacket * kChannels> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    const uint64_t streamGeneration =
        control.generation.load(std::memory_order_acquire);
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = 4 * kFramesPerPacket,
        .inputChannels = kChannels,
        .control = &control,
        .sampleRateHz = 48000,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source,
        {
            .wireFormat = ASFW::Encoding::AudioWireFormat::kMotuV3Packed,
            .framing = ASFW::Encoding::AudioPacketFraming::kMotuV3Header,
            .am824Slots = kDbs,
            .streamChannels = kChannels,
        });
    const ASFW::Audio::Wire::MotuRxPayloadCodec codec(kChannels);
    ASFW::Audio::Wire::MotuV3RxTimingObserver observer(&source);
    consumer.SetPayloadCodec(&codec);
    consumer.SetTimingObserver(&observer);

    uint32_t packetOrdinal = 0;
    const auto consume = [&](int64_t motuTicks) {
        WriteBE32(packet.data() + 16,
                  ASFW::Protocols::Audio::AMDTP::MotuV3Wire::EncodeSph(motuTicks));
        const uint32_t cycle = 300u + packetOrdinal;
        const uint16_t rawTimestamp = static_cast<uint16_t>((5u << 13) | cycle);
        packet[0] = static_cast<uint8_t>(rawTimestamp & 0xffu);
        packet[1] = static_cast<uint8_t>(rawTimestamp >> 8);
        const ASFW::Isoch::IsochReceiveBatch batch{
            .drainCycleTimer = EncodeCycleTimer(13, cycle, 64),
            .drainHostTicks = 1'000'000 + packetOrdinal * 166'667,
        };
        const ASFW::Isoch::IsochReceivePacket isochPacket{
            .descriptorIndex = packetOrdinal++,
            .payload = packet,
        };
        consumer.BeginReceiveBatch(batch);
        consumer.ConsumePacket(batch, isochPacket);
    };

    consumer.OnReceiveActivated();
    consume(0);
    consume(4096);
    consume(8192);

    ASFW::Audio::Runtime::MotuRxSphClockSample observed{};
    uint64_t updates = 0;
    ASSERT_TRUE(control.motuRxSphClock.ReadLatest(observed, updates));
    ASSERT_EQ(observed.streamGeneration, streamGeneration);
    ASSERT_EQ(observed.rxFrames, 16u);
    ASSERT_EQ(observed.rxTicks, 8192);

    // Internal IR recovery rebuilds the binding but does not run StartIO and
    // therefore must keep the same control generation and cumulative clock.
    consumer.OnReceiveQuiesced();
    source.SetGeneration(2);
    consumer.OnReceiveActivated();
    consume(1048576);
    consume(1052672);

    ASSERT_TRUE(control.motuRxSphClock.ReadLatest(observed, updates));
    EXPECT_EQ(updates, 3u);
    EXPECT_EQ(observed.streamGeneration, streamGeneration);
    EXPECT_EQ(observed.rxFrames, 24u);
    EXPECT_EQ(observed.rxTicks, 12288);

    // A real StartIO boundary is different: even if mapped memory and binding
    // generation are reused, the control generation resets the RX cumulative
    // clock together with the bridge.
    control.ResetForStart();
    const uint64_t nextStreamGeneration =
        control.generation.load(std::memory_order_acquire);
    ASSERT_EQ(nextStreamGeneration, streamGeneration + 1);
    consumer.OnReceiveActivated();
    consume(2097152);
    consume(2101248);

    ASSERT_TRUE(control.motuRxSphClock.ReadLatest(observed, updates));
    EXPECT_EQ(updates, 1u);
    EXPECT_EQ(observed.streamGeneration, nextStreamGeneration);
    EXPECT_EQ(observed.rxFrames, 8u);
    EXPECT_EQ(observed.rxTicks, 4096);
}

// The consumer also calls the observer's Reset() from
// ResetReplayEpochForDiscontinuity. A receive-cycle gap in an established V3 stream must re-anchor the SPH meter
// like an internal IR recovery does: the gap is measured by neither frames nor
// ticks, the bridge stays monotonic in the same control generation, and the TX
// servo keeps its reference with no phase error and no hard resync.
TEST(IsochRxTimingTests,
     MotuReceiveCycleGapReanchorsWithoutDisturbingTheTxServo) {
    using ASFW::Audio::MOTU::MotuSphClockServo;
    using ASFW::Audio::MOTU::MotuSphReferenceCause;
    using ASFW::Audio::MOTU::MotuSphServoDecision;
    constexpr uint32_t kRate = 48000;
    constexpr size_t kFramesPerPacket = 8;
    constexpr int64_t kTicksPerPacket = 4096;  // exactly nominal: 512 ticks per frame
    constexpr size_t kChannels = 18;
    constexpr size_t kDbs = 16;
    constexpr size_t kBlockBytes = kDbs * 4;
    alignas(4) std::array<uint8_t,
                          8 + 8 + kFramesPerPacket * kBlockBytes> packet{};
    std::copy(kMotuV3CaptureHeader.begin(), kMotuV3CaptureHeader.end(), packet.begin() + 8);

    std::array<float, 4 * kFramesPerPacket * kChannels> input{};
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    const uint64_t streamGeneration =
        control.generation.load(std::memory_order_acquire);
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = 4 * kFramesPerPacket,
        .inputChannels = kChannels,
        .control = &control,
        .sampleRateHz = kRate,
        .valid = true,
    });
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer(
        &source,
        {
            .wireFormat = ASFW::Encoding::AudioWireFormat::kMotuV3Packed,
            .framing = ASFW::Encoding::AudioPacketFraming::kMotuV3Header,
            .am824Slots = kDbs,
            .streamChannels = kChannels,
        });
    const ASFW::Audio::Wire::MotuRxPayloadCodec codec(kChannels);
    ASFW::Audio::Wire::MotuV3RxTimingObserver observer(&source);
    consumer.SetPayloadCodec(&codec);
    consumer.SetTimingObserver(&observer);

    uint32_t descriptor = 0;
    const auto consume = [&](uint32_t cycle, int64_t motuTicks) {
        WriteBE32(packet.data() + 16,
                  ASFW::Protocols::Audio::AMDTP::MotuV3Wire::EncodeSph(motuTicks));
        const uint16_t rawTimestamp = static_cast<uint16_t>((5u << 13) | cycle);
        packet[0] = static_cast<uint8_t>(rawTimestamp & 0xffu);
        packet[1] = static_cast<uint8_t>(rawTimestamp >> 8);
        const ASFW::Isoch::IsochReceiveBatch batch{
            .drainCycleTimer = EncodeCycleTimer(13, cycle, 64),
            .drainHostTicks = 1'000'000 + descriptor * 166'667,
        };
        const ASFW::Isoch::IsochReceivePacket isochPacket{
            .descriptorIndex = descriptor++,
            .payload = packet,
        };
        consumer.BeginReceiveBatch(batch);
        consumer.ConsumePacket(batch, isochPacket);
    };

    // The TX side as the stamper drives it: one servo update per bridge
    // publication (a repeated publication is a dropped observation), with the
    // packetizer following nominal exactly, so the true phase error is zero.
    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure({
        .sampleRateHz = kRate,
        .phaseCorrectionHorizonFrames = kRate * 8,
        .minimumStepQ32 = MotuSphClockServo::NominalStepQ32(kRate) - (1LL << 32),
        .maximumStepQ32 = MotuSphClockServo::NominalStepQ32(kRate) + (1LL << 32),
    }));
    uint64_t servoUpdates = 0;
    ASFW::Audio::Runtime::MotuRxSphClockSample observed{};
    const auto updateServo = [&](uint64_t newPublications) -> MotuSphServoDecision {
        uint64_t updates = 0;
        EXPECT_TRUE(control.motuRxSphClock.ReadLatest(observed, updates));
        EXPECT_EQ(updates, servoUpdates + newPublications);
        servoUpdates = updates;
        return servo.Update({
            .valid = true,
            .generation = observed.streamGeneration,
            .rxFrames = observed.rxFrames,
            .rxTicks = observed.rxTicks,
        });
    };

    // Long enough for the replay ring to establish (kReadDelay entries), so
    // the gap hits an established stream -- the production case.
    constexpr uint32_t kPreGapPackets =
        ASFW::Audio::Runtime::RxSequenceReplayState::kReadDelay + 4;
    consumer.OnReceiveActivated();
    consume(300, 0);
    consume(301, kTicksPerPacket);
    EXPECT_EQ(updateServo(1).referenceCause, MotuSphReferenceCause::kBootstrap);
    for (uint32_t i = 2; i < kPreGapPackets; ++i) {
        consume(300 + i, i * kTicksPerPacket);
    }
    // The stamper reads only the latest publication.
    const MotuSphServoDecision before = updateServo(kPreGapPackets - 2);
    ASSERT_EQ(before.referenceCause, MotuSphReferenceCause::kNone);
    ASSERT_TRUE(before.feedbackUpdated);
    ASSERT_EQ(before.phaseErrorTicks, 0);
    ASSERT_TRUE(control.rxSequenceReplay.IsEstablished());
    ASSERT_EQ(observed.rxFrames, (kPreGapPackets - 1) * kFramesPerPacket);
    ASSERT_EQ(observed.rxTicks, (kPreGapPackets - 1) * kTicksPerPacket);
    const uint64_t epochResets = control.rxReplayEpochResets.load();

    // Seven cycles lost. The device kept running, so its SPH advanced across
    // them; the first packet after the gap enters through the discontinuity
    // path and only re-anchors the meter -- no new publication.
    constexpr uint32_t kLostCycles = 7;
    uint32_t packetIndex = kPreGapPackets + kLostCycles;
    consume(300 + packetIndex, packetIndex * kTicksPerPacket);
    EXPECT_EQ(control.rxReplayEpochResets.load(), epochResets + 1)
        << "the gap must take ResetReplayEpochForDiscontinuity, not activation";
    uint64_t updatesAfterGap = 0;
    ASSERT_TRUE(control.motuRxSphClock.ReadLatest(observed, updatesAfterGap));
    EXPECT_EQ(updatesAfterGap, servoUpdates);

    // The next packet measures one step from the new anchor: cumulative frames
    // and ticks continue from the pre-gap high-water mark, the gap counted by
    // neither, in the same control generation.
    ++packetIndex;
    consume(300 + packetIndex, packetIndex * kTicksPerPacket);
    const MotuSphServoDecision after = updateServo(1);
    EXPECT_EQ(observed.streamGeneration, streamGeneration);
    EXPECT_EQ(observed.rxFrames, kPreGapPackets * kFramesPerPacket);
    EXPECT_EQ(observed.rxTicks, kPreGapPackets * kTicksPerPacket);
    EXPECT_EQ(after.referenceCause, MotuSphReferenceCause::kNone);
    EXPECT_FALSE(after.bridgeStalled);
    EXPECT_TRUE(after.feedbackUpdated);
    EXPECT_FALSE(after.hardResyncRequired);
    EXPECT_EQ(after.phaseErrorTicks, 0);
    EXPECT_EQ(after.measuredStepQ32, MotuSphClockServo::NominalStepQ32(kRate));
}

// The servo's phase input end to end on the observer: absolute phase sampled
// once per HAL period on the packet path, the four-second window closed in
// DrainTelemetry, and `rel` conditioned and published over `motuRelPhase`
// against TX's presentation residual. Six frames per packet keeps the device
// clock exactly one bus cycle per packet, so the absolute phase is a constant.
TEST(IsochRxTimingTests, MotuV3ObserverPublishesConditionedRelPhaseFromItsDrain) {
    namespace MotuV3Wire = ASFW::Protocols::Audio::AMDTP::MotuV3Wire;
    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kFramesPerPacket = 6;
    constexpr int64_t kAbsPhaseTicks = 9216;  // three cycles, the official setpoint
    constexpr uint32_t kCyclesPerSecond = 8000;
    constexpr int64_t kTicksPerCycle = 3072;

    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    const uint64_t generation = control.generation.load(std::memory_order_acquire);
    std::array<float, 64> input{};
    FixedDirectAudioBindingSource source({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = sizeof(input),
        .inputFrames = 4,
        .inputChannels = 2,
        .control = &control,
        .sampleRateHz = kRate,
        .valid = true,
    });

    // TX side: a full cadence period whose residual the observer will read.
    ASFW::Audio::Runtime::MotuPhaseTraceCadenceSample period{};
    period.outputLastPacketIndex = 100;
    period.outputLastCycleTimer = EncodeCycleTimer(0, 10, 0);
    period.hasOutputLast = true;
    for (uint64_t packetIndex = 101; packetIndex <= 103; ++packetIndex) {
        ASFW::Audio::Runtime::PushMotuPhaseTraceCadencePacket(
            period, packetIndex, MotuV3Wire::EncodeSph(5 * kTicksPerCycle + 700));
    }
    control.motuPhaseTrace.Publish(period);
    const auto residual =
        ASFW::Audio::Runtime::ComputeMotuTxPhaseResidualCadenceMean(period);
    ASSERT_TRUE(residual.valid);
    const int64_t expectedRel =
        ASFW::Audio::Runtime::MotuShortestTickDifference(residual.residualTicks, kAbsPhaseTicks);

    ASFW::Audio::Wire::MotuV3RxTimingObserver observer(&source);
    observer.OnBatchBegin(nullptr);
    observer.Reset();

    std::array<uint8_t, 20> payload{};
    const uint32_t periodFrames =
        ASFW::IsochTransport::HalBufferProfileForRate(kRate).zeroTimestampPeriodFrames;
    // Three four-second windows fill the conditioner's warmup; a little more
    // so the third window closes on a sample after its boundary.
    const uint32_t packets = 13 * kCyclesPerSecond;
    for (uint32_t i = 0; i < packets; ++i) {
        const uint32_t cycle = i % kCyclesPerSecond;
        WriteBE32(payload.data() + 16,
                  MotuV3Wire::EncodeSph(cycle * kTicksPerCycle + kAbsPhaseTicks));
        observer.ObservePacket(payload, 16, kFramesPerPacket, static_cast<uint16_t>(cycle));
        if ((i + 1) % (periodFrames / kFramesPerPacket) == 0) {
            observer.DrainTelemetry(8);
        }
    }

    ASFW::Audio::Runtime::MotuRxSphClockSample clock{};
    uint64_t clockUpdates = 0;
    ASSERT_TRUE(control.motuRxSphClock.ReadLatest(clock, clockUpdates));
    EXPECT_EQ(clock.rxFrames, static_cast<uint64_t>(packets - 1) * kFramesPerPacket);
    EXPECT_EQ(clock.rxTicks, static_cast<int64_t>(packets - 1) * kTicksPerCycle);

    ASFW::Audio::Runtime::MotuRelPhaseSampleRecord rel{};
    uint64_t relUpdates = 0;
    // The window closes on the seed sample and then once per four seconds of
    // device frames (HAL-period boundaries 0, 196608, 393216, 589824 in 13 s);
    // the conditioner publishes from its third sample on, so two publishes.
    ASSERT_TRUE(control.motuRelPhase.ReadLatest(rel, relUpdates));
    EXPECT_EQ(relUpdates, 2u);
    EXPECT_EQ(rel.streamGeneration, generation);
    EXPECT_EQ(rel.ticks, expectedRel);
    EXPECT_EQ(rel.centerTicks, expectedRel);

    // A new control generation is a full reset: the conditioner warms up again,
    // so two more windows publish nothing even with the TX term present.
    control.ResetForStart();
    control.motuPhaseTrace.Publish(period);
    observer.OnBatchBegin(nullptr);
    for (uint32_t i = 0; i < 5 * kCyclesPerSecond; ++i) {
        const uint32_t cycle = i % kCyclesPerSecond;
        WriteBE32(payload.data() + 16,
                  MotuV3Wire::EncodeSph(cycle * kTicksPerCycle + kAbsPhaseTicks));
        observer.ObservePacket(payload, 16, kFramesPerPacket, static_cast<uint16_t>(cycle));
        if ((i + 1) % (periodFrames / kFramesPerPacket) == 0) {
            observer.DrainTelemetry(8);
        }
    }
    EXPECT_FALSE(control.motuRelPhase.ReadLatest(rel, relUpdates) && relUpdates != 0);
}
