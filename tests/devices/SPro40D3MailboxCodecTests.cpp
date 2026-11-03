// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3MailboxCodecTests.cpp - Byte-exact codec validation for the Pro 40
// D3 DCP mailbox. Fixtures: the live Stage 0 descriptor capture. Command
// frames are validated against layouts derived from the documented
// protocol model (spro40d3.rs line citations in SPro40D3MailboxTypes.hpp).
// The mechanically extracted ALSA vectors were REMOVED per review round 3
// (GPL-derived; see the stage 1 report §2.4a) - these replacements use
// independently chosen values.

#include <algorithm>
#include <array>
#include <cstdint>

#include "Audio/Protocols/DICE/Focusrite/SPro40D3MailboxTypes.hpp"
#include "SPro40D3SimulatedDevice.hpp"

#include <gtest/gtest.h>

namespace D3 = ASFW::Audio::DICE::Focusrite::SPro40D3;
namespace TestingDICE = ASFW::Testing::DICE;
using TestingDICE::kSPro40D3DescriptorBytes;

// ---------------------------------------------------------------------------
// Descriptor (live hardware bytes, Stage 0 capture 2026-11-03)
// ---------------------------------------------------------------------------

TEST(SPro40D3Codec, DescriptorParsesLiveHardwareBytes) {
    auto d = D3::ParseDcpDescriptor(kSPro40D3DescriptorBytes);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->notificationAddress, D3::kDcpOwnerNone); // owner: none (Stage 0)
    EXPECT_EQ(d->version, 2u);
    EXPECT_EQ(d->maxRequestSize, 1024u);
    EXPECT_EQ(d->responseAreaSize, 400u);
    EXPECT_EQ(d->writeAddress, 0xFFFFE04001C0ULL); // window +0x1C0
    EXPECT_EQ(d->readAddress, 0xFFFFE0400030ULL);  // window +0x30
}

TEST(SPro40D3Codec, DescriptorRejectsShortInput) {
    std::array<uint8_t, 47> shortInput{};
    auto d = D3::ParseDcpDescriptor(shortInput);
    ASSERT_FALSE(d.has_value());
    EXPECT_EQ(d.error(), D3::DcpCodecError::kTooShort);
}

TEST(SPro40D3Codec, DescriptorRejectsBadMagic) {
    auto bytes = kSPro40D3DescriptorBytes;
    bytes[0] = 0x00; // corrupt the magic's first byte
    auto d = D3::ParseDcpDescriptor(bytes);
    ASSERT_FALSE(d.has_value());
    EXPECT_EQ(d.error(), D3::DcpCodecError::kBadMagic);
}

// ---------------------------------------------------------------------------
// Command frames (independently derived layouts; values chosen here)
// ---------------------------------------------------------------------------

TEST(SPro40D3Codec, SetVolumeFrameLayoutIndependentlyDerived) {
    // set_volume (spro40d3.rs:80-84): opcode 80 00 20 02; payload at [16..20)
    // = volume BE16, channel u8, mix u8. Channel 7 / mix 11 / volume 0x1234.
    std::array<uint8_t, D3::kSetVolumeFrameSize> frame{};
    D3::SerializeSetVolume(frame, 7, 11, 0x1234);
    EXPECT_EQ(frame[0], 0x80); EXPECT_EQ(frame[1], 0x00);
    EXPECT_EQ(frame[2], 0x20); EXPECT_EQ(frame[3], 0x02);
    EXPECT_EQ(frame[16], 0x12); EXPECT_EQ(frame[17], 0x34); // volume BE16
    EXPECT_EQ(frame[18], 7);    EXPECT_EQ(frame[19], 11);   // channel, mix
    // Header quadlet [4..8) and everything past the payload stays zero:
    // the counter is only serialized at send time (engine DoLaunch).
    for (size_t i = 4; i < 16; ++i) EXPECT_EQ(frame[i], 0) << "byte " << i;
    for (size_t i = 20; i < frame.size(); ++i) EXPECT_EQ(frame[i], 0) << "byte " << i;
}

TEST(SPro40D3Codec, GetMeterFrameLayoutIndependentlyDerived) {
    // get_meter (spro40d3.rs:60-64): opcode 80 00 10 01; frame[17] = len,
    // frame[19] = pos. Position 0x2b, two meters.
    std::array<uint8_t, D3::kGetMeterFrameSize> frame{};
    D3::SerializeGetMeter(frame, 0x2b, 6);
    EXPECT_EQ(frame[0], 0x80); EXPECT_EQ(frame[1], 0x00);
    EXPECT_EQ(frame[2], 0x10); EXPECT_EQ(frame[3], 0x01);
    EXPECT_EQ(frame[17], 6);   EXPECT_EQ(frame[19], 0x2b);
}

TEST(SPro40D3Codec, MeterDecodeHandlesNegativeValues) {
    // Hand-built 24-byte response: header [0..16), then two BE32 meter
    // values - one positive, one negative (meters are signed i32).
    std::array<uint8_t, 24> frame{};
    frame[16] = 0x00; frame[17] = 0x00; frame[18] = 0x0b; frame[19] = 0xad;
    frame[20] = 0xff; frame[21] = 0xff; frame[22] = 0xfe; frame[23] = 0xdc;
    std::array<int32_t, 2> meters{};
    D3::DeserializeMeters(frame, meters);
    EXPECT_EQ(meters[0], 0xbad);
    EXPECT_EQ(meters[1], -292); // 0xfffffedc as i32
}

TEST(SPro40D3Codec, RoutingEntriesLandAtDocumentedOffsets) {
    // Independently chosen sources; entries verified against the documented
    // low-rate layout (SPro40D3MailboxTypes.cpp SerializeRoutingLowRate)
    // via EncodeRouteEntry and the destination constants.
    D3::RouterOutSrc outSrc{};
    D3::RouterMixerSrc mixerSrc{};
    D3::RouterMeterSrc meterSrc{};
    outSrc[0] = 19;  outSrc[10] = 11; outSrc[20] = 39; // stream, ADAT, mixer
    mixerSrc[17] = 1;                                    // analog 1
    meterSrc[0] = 54; meterSrc[1] = 9;                   // mixer 16, S/PDIF 1

    std::array<uint8_t, D3::kRoutingLowRateFrameSize> frame{};
    D3::SerializeRoutingLowRate(frame, outSrc, mixerSrc, meterSrc);
    const auto entryAt = [&frame](size_t off, uint32_t entry) {
        EXPECT_EQ(frame[off],     static_cast<uint8_t>(entry >> 24));
        EXPECT_EQ(frame[off + 1], static_cast<uint8_t>(entry >> 16));
        EXPECT_EQ(frame[off + 2], static_cast<uint8_t>(entry >> 8));
        EXPECT_EQ(frame[off + 3], static_cast<uint8_t>(entry));
    };
    EXPECT_EQ(frame[17], 0); // low-rate table: flags quadlet stays zero
    // First stream route: FIXED analog-in 1 -> stream ch 0.
    entryAt(20,  D3::EncodeRouteEntry(1, D3::kDstStream + 0));
    // Analog pair 0: outSrc[0] -> Mon 1/2 (kDstAnalog + 8).
    entryAt(92,  D3::EncodeRouteEntry(outSrc[0], D3::kDstAnalog + 8));
    // S/PDIF: outSrc[10] -> S/PDIF 1.
    entryAt(132, D3::EncodeRouteEntry(outSrc[10], D3::kDstSpdif));
    // Loopback: outSrc[20] -> stream ch 18.
    entryAt(172, D3::EncodeRouteEntry(outSrc[20], D3::kDstStream + 18));
    // Mixer input 18: mixerSrc[17] -> kDstMixer + 17.
    entryAt(248, D3::EncodeRouteEntry(mixerSrc[17], D3::kDstMixer + 17));
    // Master meters: destination id 0.
    entryAt(252, D3::EncodeRouteEntry(meterSrc[0], 0x0));
    entryAt(256, D3::EncodeRouteEntry(meterSrc[1], 0x0));

    // High-rate table: same first entry, marker byte set, shorter frame.
    std::array<uint8_t, D3::kRoutingHighRateFrameSize> hi{};
    D3::SerializeRoutingHighRate(hi, outSrc, mixerSrc, meterSrc);
    EXPECT_EQ(hi[17], 1);
    EXPECT_EQ(hi[18], 0); EXPECT_EQ(hi[19], 0);
    EXPECT_EQ(hi[20], frame[20]); EXPECT_EQ(hi[21], frame[21]);
    EXPECT_EQ(hi[22], frame[22]); EXPECT_EQ(hi[23], frame[23]);
}

// ---------------------------------------------------------------------------
// Route encoding semantics
// ---------------------------------------------------------------------------

TEST(SPro40D3Codec, RouteSourceIdClasses) {
    EXPECT_EQ(D3::RouteSourceId(0), D3::kSrcNone);
    EXPECT_EQ(D3::RouteSourceId(1), D3::kSrcAnalog + 0);
    EXPECT_EQ(D3::RouteSourceId(8), D3::kSrcAnalog + 7);
    EXPECT_EQ(D3::RouteSourceId(9), D3::kSrcSpdif + 0);
    EXPECT_EQ(D3::RouteSourceId(10), D3::kSrcSpdif + 1);
    EXPECT_EQ(D3::RouteSourceId(11), D3::kSrcAdat + 0);
    EXPECT_EQ(D3::RouteSourceId(18), D3::kSrcAdat + 7);
    EXPECT_EQ(D3::RouteSourceId(19), D3::kSrcStream + 0);
    EXPECT_EQ(D3::RouteSourceId(38), D3::kSrcStream + 19);
    EXPECT_EQ(D3::RouteSourceId(39), D3::kSrcMixer + 0);
    EXPECT_EQ(D3::RouteSourceId(54), D3::kSrcMixer + 15);
}

TEST(SPro40D3Codec, EncodeRouteEntry) {
    // analog 1 -> stream 1: (0x080 << 12) | 0x400 = 0x80400 (ALSA vector entry)
    EXPECT_EQ(D3::EncodeRouteEntry(1, 0x400), 0x00080400u);
    // mixer out 1 -> mixer input 1: (0x300 << 12) | 0x300 = 0x300300
    EXPECT_EQ(D3::EncodeRouteEntry(39, 0x300), 0x00300300u);
    // none -> anything: destination only
    EXPECT_EQ(D3::EncodeRouteEntry(0, 0x88), 0x00000088u);
}

TEST(SPro40D3Codec, AnalogOutDestinationsAreInvertedPairs) {
    // Pair 0 = Mon 1/2 -> 0x88/0x89; pair 4 = Line 9/10 (HP2) -> 0x80/0x81
    EXPECT_EQ(D3::AnalogOutDestinationForChannel(0, false), 0x88u);
    EXPECT_EQ(D3::AnalogOutDestinationForChannel(0, true), 0x89u);
    EXPECT_EQ(D3::AnalogOutDestinationForChannel(3, false), 0x82u); // HP1
    EXPECT_EQ(D3::AnalogOutDestinationForChannel(3, true), 0x83u);
    EXPECT_EQ(D3::AnalogOutDestinationForChannel(4, false), 0x80u);
    EXPECT_EQ(D3::AnalogOutDestinationForChannel(4, true), 0x81u);
}

// ---------------------------------------------------------------------------
// Output volume packing (inverted against the host scale)
// ---------------------------------------------------------------------------

TEST(SPro40D3Codec, OutputVolumePairInversion) {
    // Host 0 (mute) -> device 0xff; host 255 (max) -> device 0x00.
    EXPECT_EQ(D3::PackOutputVolumePair(0, 255), 0x00ffu);
    int32_t even{}, odd{};
    D3::UnpackOutputVolumePair(0x00ffu, even, odd);
    EXPECT_EQ(even, 0);
    EXPECT_EQ(odd, 255);
    // Round trip: device bytes 0xb6/0x36 (inverted), packed little-endian-in-quadlet.
    EXPECT_EQ(D3::PackOutputVolumePair(73, 201), 0x36b6u);
    D3::UnpackOutputVolumePair(0x36b6u, even, odd);
    EXPECT_EQ(even, 73);
    EXPECT_EQ(odd, 201);
}

// ---------------------------------------------------------------------------
// Header / notification codec
// ---------------------------------------------------------------------------

TEST(SPro40D3Codec, MessageHeaderRoundTrip) {
    std::array<uint8_t, 24> frame{};
    D3::WriteMessageHeader(frame, D3::kOpcodeGetMeter, 0xbeef, 8);
    EXPECT_EQ(frame[0], 0x80); EXPECT_EQ(frame[1], 0x00);
    EXPECT_EQ(frame[2], 0x10); EXPECT_EQ(frame[3], 0x01);
    EXPECT_EQ(frame[4], 0xbe); EXPECT_EQ(frame[5], 0xef); // counter BE16
    EXPECT_EQ(frame[6], 0x00); EXPECT_EQ(frame[7], 0x08); // payload len BE16
    auto h = D3::ParseResponseHeader(frame);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->counter, 0xbeef);
    EXPECT_EQ(h->payloadLength, 8);
}

TEST(SPro40D3Codec, NotificationDecode) {
    // rlen at [6..8], rstatus at [8..12] (spro40d3.rs resp.connect_requested).
    std::array<uint8_t, 12> notif{};
    notif[6] = 0x01; notif[7] = 0x68; // 360
    notif[8] = 0; notif[9] = 0; notif[10] = 0; notif[11] = 4;
    auto n = D3::ParseNotification(notif);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->responsePayloadLength, 360);
    EXPECT_EQ(n->responseStatus, 4u);
    // Malformed (short) ingress is rejected, not misread.
    std::array<uint8_t, 8> shortNotif{};
    EXPECT_FALSE(D3::ParseNotification(shortNotif).has_value());
}

TEST(SPro40D3Codec, ResponseHeaderRejectsShortFrame) {
    std::array<uint8_t, 15> shortFrame{};
    EXPECT_FALSE(D3::ParseResponseHeader(shortFrame).has_value());
}
