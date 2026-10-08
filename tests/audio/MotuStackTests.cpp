// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "ASFWDriver/Audio/Wire/MOTU/MotuModel.hpp"
#include "ASFWDriver/Audio/Wire/MOTU/MotuSphSynthesizer.hpp"
#include "ASFWDriver/Audio/Wire/MOTU/MotuPayloadCodec.hpp"
#include "ASFWDriver/Audio/Protocols/MOTU/MotuRegisters.hpp"
#include "ASFWDriver/Audio/Runtime/ResolvedAudioConfiguration.hpp"
#include <cmath>
#include <thread>
#include "ASFWDriver/Audio/Protocols/MOTU/MotuNotificationMailbox.hpp"
using namespace ASFW::Encoding::Motu;
using namespace ASFW::Audio::Motu;

TEST(MotuStack, EightPreAdatDoesNotHalveAtDoubleRateAndIsAsymmetric) {
    // Linux motu-protocol-v2.c:250-269,315-320; vendor model table corroborates.
    const auto one = ResolvePcmChunks(0x500, 0, 15);
    const auto two = ResolvePcmChunks(0x500, 1, 15);
    EXPECT_EQ(one.tx, 18U); EXPECT_EQ(one.rx, 14U);
    EXPECT_EQ(two.tx, 18U); EXPECT_EQ(two.rx, 14U);
    EXPECT_FALSE(SupportsRate(*FindModel(15), 192000));
    EXPECT_EQ(ResolvePcmChunks(0, 2, 9).tx, 8U);
}
TEST(MotuStack, V3OpticalBanksWidenDirectionsIndependently) {
    EXPECT_EQ(ResolvePcmChunks(0x101, 0, 21).tx, 26U);
    EXPECT_EQ(ResolvePcmChunks(0x101, 0, 21).rx, 22U);
    EXPECT_EQ(ResolvePcmChunks(0x101, 1, 21).tx, 22U);
    EXPECT_EQ(ResolvePcmChunks(0x101, 2, 21).tx, 14U);
    EXPECT_FALSE(SupportsRate(*FindModel(53), 192000)); // unresolved hybrid padding
    EXPECT_EQ(ResolvePcmChunks(0x10001, 0, 53).tx, 0U); // unresolved hybrid SPDIF
}
TEST(MotuStack, TravelerSetsItsOwnFetchVariantOnInternalClock) {
    EXPECT_EQ(EncodeFetchingMode(8, true, false, true), 0x06000008U);
    EXPECT_EQ(EncodeFetchingMode(8, false, false, true), 0x04000008U);
    EXPECT_EQ(EncodeFetchingMode(8, true, true), 0x02000008U);
}
TEST(MotuStack, PackedPcmFormationDoesNotPretendChannelsAreQuadletSlots) {
    using namespace ASFW::Audio::Runtime;
    RateFormation f{};
    f.sampleRateHz = 48000; f.protocolSupported = true; f.packedPcm = true;
    f.playback = {{14, 13}}; f.capture = {{18, 16}};
    const auto capacity = MaximumFormationAllocation(std::span(&f, 1), {16384, 14, 18, 0});
    ASSERT_TRUE(capacity);
    const auto resolved = ResolveAudioConfiguration(48000, std::span(&f, 1), {64,64,128,128}, *capacity, 0,
        ConfigurationValidationPolicy::HardwareBatch);
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->captureChannels, 18U);
    f.capture[0].dataBlockSize = 15; // plausible but wrong MOTU stride must fail
    EXPECT_FALSE(ResolveAudioConfiguration(48000, std::span(&f, 1), {64,64,128,128}, *capacity, 0,
        ConfigurationValidationPolicy::HardwareBatch));
    f.capture[0].dataBlockSize = 16; f.packedPcm = false;
    EXPECT_FALSE(ResolveAudioConfiguration(48000, std::span(&f, 1), {64,64,128,128}, *capacity, 0,
        ConfigurationValidationPolicy::HardwareBatch));
}
TEST(MotuStack, ProprietaryReceivePrefixIsScopedToV3Codec) {
    ASFW::Audio::Wire::MotuRxPayloadCodec v2(18), v3(18, {}, true);
    const auto q0 = OSSwapHostToBigInt32(0x0d040400);
    const auto q1 = OSSwapHostToBigInt32(0x22ffffff);
    EXPECT_FALSE(v2.DecodeHeader(q0, q1));
    ASSERT_TRUE(v3.DecodeHeader(q0, q1));
    EXPECT_EQ(v3.StrideQuadlets(4), 16U); // captured DBS is not the packed stride
    EXPECT_FALSE(v3.DecodeHeader(OSSwapHostToBigInt32(0x8d040400), q1));
    EXPECT_FALSE(v3.DecodeHeader(q0, OSSwapHostToBigInt32(0x12345678)));
}
TEST(MotuSynthesis, FractionalAccumulatorTracksAllSixRatesAcrossSecondWrap) {
    for (auto rate : kClockRates) {
        SCOPED_TRACE(rate);
        SphSynthesizer synth; synth.Configure(rate);
        const uint32_t seed = kTicksPerSecond - 100;
        ASSERT_TRUE(synth.Observe(seed));
        // Avoid feeding the same single seed repeatedly: verify the accumulator
        // itself retains fractions for more than one second at every rate.
        for (uint64_t frame = 0; frame < rate + 100U; ++frame) {
            const auto sph = synth.NextSph();
            const auto tick = ((sph >> 12) & 0x1fff) * 3072 + (sph & 0xfff);
            const auto expected = (seed + frame * uint64_t{kTicksPerSecond} / rate) % kTicksPerSecond;
            const int64_t difference = static_cast<int64_t>(tick) - static_cast<int64_t>(expected);
            EXPECT_LE(std::abs(difference), 1) << frame;
        }
    }
}
TEST(MotuSynthesis, AcquiresDeviceDriftWithoutReplayingIntraPacketNoise) {
    SphSynthesizer synth; synth.Configure(48000);
    for (uint32_t frame = 0; frame < 10000; frame += 8) {
        const auto ideal = static_cast<uint32_t>(20000 + frame * 512.0 / 1.0001);
        ASSERT_TRUE(synth.Observe(ideal));
        const auto first = synth.NextSph();
        const auto tick = ((first >> 12) & 0x1fff) * 3072 + (first & 0xfff);
        if (frame > 4096) EXPECT_LE(std::abs(static_cast<int64_t>(tick) - ideal), 3);
        for (int i = 1; i < 8; ++i) (void)synth.NextSph();
    }
    EXPECT_TRUE(synth.Current().locked);
    EXPECT_LT(synth.Current().stepQ32, (int64_t{512} << 32));
}
TEST(MotuSynthesis, FourCycleDiscontinuityRequestsRecoveryInsteadOfAStampJump) {
    SphSynthesizer synth; synth.Configure(48000);
    ASSERT_TRUE(synth.Observe(10000));
    for (int i = 0; i < 8; ++i) (void)synth.NextSph();
    EXPECT_FALSE(synth.Observe(10000 + 4096 + 4 * 3072 + 1));
    EXPECT_TRUE(synth.Current().discontinuity);
    EXPECT_FALSE(synth.Current().ready);
    synth.Reset();
    EXPECT_FALSE(synth.Current().discontinuity);
    EXPECT_TRUE(synth.Observe(100));
}

TEST(MotuStack, OriginalLayoutsAndFireWire896Mk3Geometry) {
    EXPECT_EQ(ResolvePcmChunks(0, 0, 1).tx, 18U);
    EXPECT_EQ(ResolvePcmChunks(0xc000, 0, 1).rx, 10U);
    EXPECT_EQ(ResolvePcmChunks(0, 1, 2).tx, 18U);
    EXPECT_EQ(ResolvePcmChunks(0, 0, 0x17).rx, 18U); // provider "1394"
    EXPECT_EQ(DataBlockQuadlets(18, MessageChunks(1, true)), 16U);
    EXPECT_EQ(DataBlockQuadlets(18, MessageChunks(1, false)), 15U);
    EXPECT_EQ(EncodeFetchingV1(0xffff00ff, false, 1), 0x77U);
    EXPECT_EQ(EncodeFetchingV1(0xf3000018, false, 2), 0x03000018U);
    EXPECT_EQ(EncodeFetchingV1(0x18, true, 2), 0x23000018U);
}
TEST(MotuStack, V1CaptureReadsPcmImmediatelyAfterSphAndIgnoresTrailingStatus) {
    ASFW::Audio::Wire::MotuRxPayloadCodec codec(10, {}, false, 2, 4);
    std::array<uint8_t,40> block{};
    WritePcmSample(std::span(block).subspan(4,3), 0x40000000);
    block[34] = 0x7f; block[38] = 0x06; // trailing status, never first-channel PCM
    float output[10]{};
    ASFW::AudioEngine::Direct::Rx::RxCaptureChannelMap map{};
    ASSERT_TRUE(codec.ValidateGeometry(10,0,10,10));
    codec.DecodeBlock(block,10,0,map,output,nullptr);
    EXPECT_NEAR(output[0], 0.5f, 0.000001f);
    for (size_t i = 1; i < 10; ++i) EXPECT_EQ(output[i], 0.0f);
}

TEST(MotuNotifications, ConcurrentAckNotificationAndTimeoutCompleteExactlyOnce) {
    for (int i = 0; i < 100; ++i) {
        std::atomic<int> calls{0};
        auto pending = std::make_shared<ClockChangeWait>([&](IOReturn) { ++calls; });
        ASSERT_TRUE(pending->Valid());
        std::thread ack([&] { pending->Ack(kIOReturnSuccess); });
        std::thread notify([&] { pending->Notify(); });
        std::thread timeout([&] { pending->Fail(kIOReturnTimeout); });
        ack.join(); notify.join(); timeout.join();
        EXPECT_EQ(calls.load(),1); EXPECT_TRUE(pending->Done());
    }
}
TEST(MotuNotifications, ExpiredEndpointAndMalformedMessagesCannotCompleteAnOperation) {
    auto mailbox = std::make_shared<NotificationMailbox>(ASFW::Discovery::DeviceRouteToken{
        .guid=1, .deviceIncarnation=1, .routeEpoch=1, .generation=ASFW::FW::Generation{1}, .nodeId=1});
    const auto address = Notifications::Register(mailbox); ASSERT_NE(address,0U);
    int completions = 0;
    auto wait = mailbox->Begin([&](IOReturn) { ++completions; }); ASSERT_TRUE(wait);
    wait->Ack(kIOReturnSuccess);
    const std::array<uint8_t,3> shortPayload{0,0,2};
    EXPECT_EQ(Notifications::Handle({.destOffset=address,.sourceID=1,.generation=1,.writePayload=shortPayload}).rcode,
        ASFW::Async::ResponseCode::TypeError);
    EXPECT_EQ(completions,0);
    mailbox.reset();
    const std::array<uint8_t,4> validPayload{0,0,0,2};
    EXPECT_EQ(Notifications::Handle({.destOffset=address,.sourceID=1,.generation=1,.writePayload=validPayload}).rcode,
        ASFW::Async::ResponseCode::Complete);
    EXPECT_EQ(completions,0);
    wait->Fail(kIOReturnAborted); EXPECT_EQ(completions,1);
}

TEST(MotuStack, DualAdatCaptureKeepsAll34ChannelsAndRejectsOversizedFormats) {
    using namespace ASFW::Audio::Runtime;
    RateFormation f{};
    f.sampleRateHz = 48000; f.protocolSupported = true; f.packedPcm = true;
    const auto chunks = ResolvePcmChunks(0x303, 0, 0x15);
    ASSERT_EQ(chunks.tx, 34U);
    f.playback = {{chunks.rx, DataBlockQuadlets(chunks.rx)}};
    f.capture = {{chunks.tx, DataBlockQuadlets(chunks.tx)}};
    const auto capacity = MaximumFormationAllocation(std::span(&f,1), {16384,0,0,0});
    ASSERT_TRUE(capacity); EXPECT_EQ(capacity->captureChannelCapacity,34U);
    const auto resolved = ResolveAudioConfiguration(48000,std::span(&f,1),{64,64,128,128},*capacity,0,
        ConfigurationValidationPolicy::HardwareBatch);
    ASSERT_TRUE(resolved); EXPECT_EQ(resolved->captureChannels,34U);
    ASFW::Audio::Wire::MotuRxPayloadCodec codec(34,{},true);
    std::array<uint8_t,112> block{};
    WritePcmSample(std::span(block).subspan(10 + 33 * 3,3),0x40000000);
    std::array<float,34> output{};
    ASSERT_TRUE(codec.ValidateGeometry(34,0,28,28));
    codec.DecodeBlock(block,34,0,{},output.data(),nullptr);
    EXPECT_NEAR(output[33],0.5f,0.000001f);
    f.capture[0] = {35,DataBlockQuadlets(35)};
    EXPECT_FALSE(MaximumFormationAllocation(std::span(&f,1), {16384,0,0,0}));
    f.capture[0] = {34,28}; f.packedPcm = false;
    EXPECT_FALSE(MaximumFormationAllocation(std::span(&f,1), {16384,0,0,0}));
}
