// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>
#include "Audio/Wire/AM824/Am824PayloadCodec.hpp"
#include "Audio/Wire/RawPcm24In32/RawPcm24In32PayloadCodec.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"

#include <array>
#include <vector>

using namespace ASFW::Audio::Wire;
using namespace ASFW::AudioEngine::Direct::Rx;
using namespace ASFW::Protocols::Audio::AMDTP;

TEST(Am824PayloadCodecTests, StrideAndGeometryValidation) {
    Am824RxPayloadCodec codec(8, false);

    // Stride reflects CIP DBS when not trusting configured stride
    EXPECT_EQ(codec.StrideQuadlets(8), 8U);
    EXPECT_EQ(codec.StrideQuadlets(10), 10U);

    // Valid geometry: stride >= channels and dbs == am824Slots
    EXPECT_TRUE(codec.ValidateGeometry(8, 0, 8, 8));
    EXPECT_FALSE(codec.ValidateGeometry(0, 0, 8, 8)); // 0 channels invalid
    EXPECT_FALSE(codec.ValidateGeometry(9, 0, 8, 8)); // channels > stride invalid
    EXPECT_FALSE(codec.ValidateGeometry(8, 0, 8, 7)); // dbs != am824Slots invalid

    // Trusting configured stride allows CIP DBS mismatch
    codec.Configure(8, true);
    EXPECT_EQ(codec.StrideQuadlets(5), 8U);
    EXPECT_TRUE(codec.ValidateGeometry(8, 0, 8, 5));
}

TEST(Am824PayloadCodecTests, DecodeBlockExtractsFloatSamples) {
    Am824RxPayloadCodec codec(2, false);

    // Prepare 2 channels encoded via PcmSlotCodec, stored on wire as BE32
    const uint32_t sample0 = OSSwapHostToBigInt32(
        PcmSlotCodec::EncodeFloat32(0.5f, PcmSlotEncoding::Am824MBLA));
    const uint32_t sample1 = OSSwapHostToBigInt32(
        PcmSlotCodec::EncodeFloat32(-0.5f, PcmSlotEncoding::Am824MBLA));

    std::array<uint32_t, 2> wireQuadlets = {sample0, sample1};
    std::span<const uint8_t> blockBytes(
        reinterpret_cast<const uint8_t*>(wireQuadlets.data()), wireQuadlets.size() * 4);

    std::array<float, 2> frameOut{};
    RxCaptureChannelMap map{}; // Identity

    codec.DecodeBlock(blockBytes, 2, 0, map, frameOut.data(), nullptr);

    EXPECT_NEAR(frameOut[0], 0.5f, 0.0001f);
    EXPECT_NEAR(frameOut[1], -0.5f, 0.0001f);
}

TEST(Am824PayloadCodecTests, DecodeBlockWithDelayAndPermutation) {
    Am824RxPayloadCodec codec(2, false);

    const uint32_t sample0 = OSSwapHostToBigInt32(
        PcmSlotCodec::EncodeFloat32(0.75f, PcmSlotEncoding::Am824MBLA));
    const uint32_t sample1 = OSSwapHostToBigInt32(
        PcmSlotCodec::EncodeFloat32(-0.25f, PcmSlotEncoding::Am824MBLA));

    std::array<uint32_t, 2> wireQuadlets = {sample0, sample1};
    std::span<const uint8_t> blockBytes(
        reinterpret_cast<const uint8_t*>(wireQuadlets.data()), wireQuadlets.size() * 4);

    std::array<float, 2> frameOut{};
    std::array<float, 2> delayedOut{};

    // Slot 0 -> Ch 1, Slot 1 -> Ch 0 (permuted), and Ch 1 delayed
    RxCaptureChannelMap map{};
    (void)map.SetSlots(std::array<uint8_t, 2>{1, 0});
    map.delayedChannelMask = (1U << 1);
    map.delayFrames = 1;

    codec.DecodeBlock(blockBytes, 2, 0, map, frameOut.data(), delayedOut.data());

    // Ch 0 is from slot 1 (-0.25f), not delayed -> frameOut[0]
    EXPECT_NEAR(frameOut[0], -0.25f, 0.0001f);
    // Ch 1 is from slot 0 (0.75f), delayed -> delayedOut[1]
    EXPECT_NEAR(delayedOut[1], 0.75f, 0.0001f);
}

TEST(RawPcm24In32PayloadCodecTests, StrideAndGeometryValidation) {
    RawPcm24In32RxPayloadCodec codec(8, false);

    EXPECT_EQ(codec.StrideQuadlets(8), 8U);
    EXPECT_TRUE(codec.ValidateGeometry(8, 0, 8, 8));
    EXPECT_FALSE(codec.ValidateGeometry(0, 0, 8, 8));
    EXPECT_FALSE(codec.ValidateGeometry(9, 0, 8, 8));
    EXPECT_FALSE(codec.ValidateGeometry(8, 0, 8, 7));

    codec.Configure(8, true);
    EXPECT_EQ(codec.StrideQuadlets(4), 8U);
    EXPECT_TRUE(codec.ValidateGeometry(8, 0, 8, 4));
}

TEST(RawPcm24In32PayloadCodecTests, DecodeBlockExtractsFloatSamples) {
    RawPcm24In32RxPayloadCodec codec(2, false);

    // Encode float with RawSigned24In32BE, stored on wire as BE32
    const uint32_t sample0 = OSSwapHostToBigInt32(
        PcmSlotCodec::EncodeFloat32(0.5f, PcmSlotEncoding::RawSigned24In32BE));
    const uint32_t sample1 = OSSwapHostToBigInt32(
        PcmSlotCodec::EncodeFloat32(-0.5f, PcmSlotEncoding::RawSigned24In32BE));

    std::array<uint32_t, 2> wireQuadlets = {sample0, sample1};
    std::span<const uint8_t> blockBytes(
        reinterpret_cast<const uint8_t*>(wireQuadlets.data()), wireQuadlets.size() * 4);

    std::array<float, 2> frameOut{};
    RxCaptureChannelMap map{};

    codec.DecodeBlock(blockBytes, 2, 0, map, frameOut.data(), nullptr);

    EXPECT_NEAR(frameOut[0], 0.5f, 0.0001f);
    EXPECT_NEAR(frameOut[1], -0.5f, 0.0001f);
}

// DICE capture: the top byte is ignored whatever it holds. The Saffire sends
// MBLA 0x40, the StudioLive AI 0x00 (studiolive-firewire-mac slfw.c:428), raw
// 24-in-32 a sign extension; each is the same sample. The MIDI slot after the
// PCM slots is never read as audio, whatever its label.
TEST(RawPcm24In32PayloadCodecTests, TopByteIsIgnoredAndMidiSlotIsNeverDecoded) {
    RawPcm24In32RxPayloadCodec codec(7, false);
    std::array<uint32_t, 7> wireQuadlets = {
        OSSwapHostToBigInt32(0x40400000u),  // MBLA +0.5
        OSSwapHostToBigInt32(0x00400000u),  // label 0x00, +0.5
        OSSwapHostToBigInt32(0x00400000u),  // sign-extended positive, +0.5
        OSSwapHostToBigInt32(0x40C00000u),  // MBLA -0.5
        OSSwapHostToBigInt32(0x00C00000u),  // label 0x00, -0.5
        OSSwapHostToBigInt32(0xFFC00000u),  // sign-extended negative, -0.5
        OSSwapHostToBigInt32(0x81123456u),  // MIDI slot
    };
    std::span<const uint8_t> blockBytes(
        reinterpret_cast<const uint8_t*>(wireQuadlets.data()), wireQuadlets.size() * 4);
    std::array<float, 7> frameOut{};
    frameOut.fill(99.0f);

    codec.DecodeBlock(blockBytes, 6, 0, RxCaptureChannelMap{}, frameOut.data(), nullptr);

    for (size_t ch = 0; ch < 3; ++ch) EXPECT_NEAR(frameOut[ch], 0.5f, 0.0001f) << ch;
    for (size_t ch = 3; ch < 6; ++ch) EXPECT_NEAR(frameOut[ch], -0.5f, 0.0001f) << ch;
    EXPECT_EQ(frameOut[6], 99.0f);  // six PCM channels: slot 6 untouched
}
