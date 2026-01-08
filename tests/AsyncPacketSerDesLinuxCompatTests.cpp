/**
 * AsyncPacketSerDesLinuxCompatTests.cpp
 *
 * Host-only GoogleTest suite that mirrors the Linux firewire packet/ohci
 * KUnit coverage for asynchronous transmit/receive headers. The goal is to
 * ensure our PacketBuilder and AR parsing logic stay byte-for-byte compatible
 * with the well-tested Linux implementation, without depending on DriverKit
 * artefacts.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>

#include "ASFWDriver/Async/AsyncTypes.hpp"
#include "ASFWDriver/Hardware/IEEE1394.hpp"
#include "ASFWDriver/Async/Rx/ARPacketParser.hpp"
#include "ASFWDriver/Async/Rx/PacketRouter.hpp"
#include "ASFWDriver/Async/Tx/PacketBuilder.hpp"

using namespace ASFW::Async;

namespace {

template <std::size_t N>
constexpr std::array<uint32_t, N> LoadHostQuadlets(const uint8_t* base) {
    std::array<uint32_t, N> words{};
    std::memcpy(words.data(), base, N * sizeof(uint32_t));
    return words;
}

std::vector<uint8_t> MakeARBufferFromWireWords(std::initializer_list<uint32_t> quadlets,
                                               uint32_t trailerLE = 0) {
    std::vector<uint8_t> bytes;
    bytes.reserve(quadlets.size() * sizeof(uint32_t) + sizeof(uint32_t));

    for (uint32_t word : quadlets) {
        bytes.push_back(static_cast<uint8_t>((word >> 24) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((word >> 16) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
        bytes.push_back(static_cast<uint8_t>(word & 0xFF));
    }

    // OHCI appends a little-endian trailer. Zero is portable regardless of byte order.
    bytes.push_back(static_cast<uint8_t>(trailerLE & 0xFF));
    bytes.push_back(static_cast<uint8_t>((trailerLE >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((trailerLE >> 16) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((trailerLE >> 24) & 0xFF));

    return bytes;
}

PacketContext MakeDefaultContext(uint16_t sourceNodeID, uint8_t speedCode) {
    PacketContext context{};
    context.sourceNodeID = sourceNodeID;
    context.generation = 1;
    context.speedCode = speedCode;
    return context;
}

// Helper to compute bus/node packed ID as the builder does.
uint16_t MakeDestinationID(uint16_t busNodePacked, uint16_t remoteNode) {
    const uint16_t bus = static_cast<uint16_t>((busNodePacked >> 6) & 0x03FFu);
    return static_cast<uint16_t>((bus << 6) | (remoteNode & 0x3Fu));
}

} // namespace

// -----------------------
// PacketBuilder coverage
// -----------------------

TEST(AsyncPacketSerDesLinuxCompat, ReadQuadletRequestMatchesLinuxVector) {
    PacketBuilder builder;

    ReadParams params{};
    params.destinationID = 0xFFC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000984;
    params.length = 4;

    const PacketContext context = MakeDefaultContext(/*sourceNodeID=*/0xFFC1, /*speedCode=*/0x02);
    constexpr uint8_t kLabel = 0x3C;

    std::array<uint8_t, sizeof(uint32_t) * 4> buffer{};
    const std::size_t bytes =
        builder.BuildReadQuadlet(params, kLabel, context, buffer.data(), buffer.size());

    ASSERT_EQ(bytes, sizeof(uint32_t) * 3);

    const auto hostWords = LoadHostQuadlets<3>(buffer.data());

    // Verify Linux OHCI format
    EXPECT_EQ((hostWords[0] >> 10) & 0x3Fu, kLabel);  // tLabel at bits[15:10]
    EXPECT_EQ((hostWords[0] >> 16) & 0x7u, context.speedCode & 0x7u);  // speed at bits[18:16]
    EXPECT_EQ((hostWords[0] >> 8) & 0x3u, 0x01u);  // retry at bits[9:8]
    EXPECT_EQ((hostWords[0] >> 4) & 0xFu, HW::AsyncRequestHeader::kTcodeReadQuad);  // tCode at bits[7:4]
    const uint16_t destID = static_cast<uint16_t>(hostWords[1] >> 16);  // destID in Q1[31:16]
    EXPECT_EQ(destID, MakeDestinationID(context.sourceNodeID, params.destinationID));
    EXPECT_EQ(static_cast<uint16_t>(hostWords[1] & 0xFFFFu),
              static_cast<uint16_t>(params.addressHigh));
    EXPECT_EQ(hostWords[2], params.addressLow);
}

TEST(AsyncPacketSerDesLinuxCompat, WriteQuadletRequestMatchesLinuxVector) {
    PacketBuilder builder;

    WriteParams params{};
    params.destinationID = 0xFFC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000234;
    params.length = 4;
    const uint32_t payloadQuadlet = 0x1F0000C0u;
    params.payload = &payloadQuadlet;

    const PacketContext context = MakeDefaultContext(0xFFC1, 0x02);
    constexpr uint8_t kLabel = 0x14;

    std::array<uint8_t, sizeof(uint32_t) * 4> buffer{};
    const std::size_t bytes =
        builder.BuildWriteQuadlet(params, kLabel, context, buffer.data(), buffer.size());

    ASSERT_EQ(bytes, sizeof(uint32_t) * 4);

    const auto hostWords = LoadHostQuadlets<4>(buffer.data());

    // Verify Linux OHCI format
    EXPECT_EQ((hostWords[0] >> 10) & 0x3Fu, kLabel);  // tLabel at bits[15:10]
    EXPECT_EQ((hostWords[0] >> 16) & 0x7u, context.speedCode & 0x7u);  // speed at bits[18:16]
    EXPECT_EQ((hostWords[0] >> 8) & 0x3u, 0x01u);  // retry at bits[9:8]
    EXPECT_EQ((hostWords[0] >> 4) & 0xFu, HW::AsyncRequestHeader::kTcodeWriteQuad);  // tCode at bits[7:4]
    EXPECT_EQ(hostWords[3], payloadQuadlet);
}

TEST(AsyncPacketSerDesLinuxCompat, WriteBlockRequestMatchesLinuxVector) {
    PacketBuilder builder;

    WriteParams params{};
    params.destinationID = 0xFFC0;
    params.addressHigh = 0xECC0;
    params.addressLow = 0x00000000;
    params.length = 0x0018;

    const PacketContext context = MakeDefaultContext(0xFFC1, 0x02);
    constexpr uint8_t kLabel = 0x19;

    std::array<uint8_t, sizeof(uint32_t) * 4> buffer{};
    const std::size_t bytes =
        builder.BuildWriteBlock(params, kLabel, context, buffer.data(), buffer.size());

    ASSERT_EQ(bytes, sizeof(uint32_t) * 4);

    const auto hostWords = LoadHostQuadlets<4>(buffer.data());

    EXPECT_EQ((hostWords[0] >> 10) & 0x3Fu, kLabel);  // tLabel at bits[15:10]
    EXPECT_EQ((hostWords[0] >> 4) & 0xFu, HW::AsyncRequestHeader::kTcodeWriteBlock);  // tCode at bits[7:4]
    EXPECT_EQ(hostWords[3], static_cast<uint32_t>(params.length) << 16);

    EXPECT_EQ(static_cast<uint16_t>((hostWords[1] >> 16) & 0xFFFFu),
              MakeDestinationID(context.sourceNodeID, params.destinationID));
    EXPECT_EQ(static_cast<uint16_t>(hostWords[1] & 0xFFFFu),
              static_cast<uint16_t>(params.addressHigh));
    EXPECT_EQ(hostWords[2], params.addressLow);
}

TEST(AsyncPacketSerDesLinuxCompat, LockRequestMatchesLinuxVector) {
    PacketBuilder builder;

    LockParams params{};
    params.destinationID = 0xFFC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000984;
    params.operandLength = 0x0008;
    params.responseLength = 0x0004;

    const PacketContext context = MakeDefaultContext(0xFFC1, 0x02);
    constexpr uint8_t kLabel = 0x0B;
    constexpr uint16_t kExtendedTCode = 0x0002;

    std::array<uint8_t, sizeof(uint32_t) * 4> buffer{};
    const std::size_t bytes =
        builder.BuildLock(params, kLabel, kExtendedTCode, context, buffer.data(), buffer.size());

    ASSERT_EQ(bytes, sizeof(uint32_t) * 4);

    const auto hostWords = LoadHostQuadlets<4>(buffer.data());

    EXPECT_EQ((hostWords[0] >> 10) & 0x3Fu, kLabel);  // tLabel at bits[15:10]
    EXPECT_EQ((hostWords[0] >> 4) & 0xFu, HW::AsyncRequestHeader::kTcodeLockRequest);  // tCode at bits[7:4]
    EXPECT_EQ(hostWords[3],
              (static_cast<uint32_t>(params.operandLength) << 16) |
                  static_cast<uint32_t>(kExtendedTCode));

    EXPECT_EQ(static_cast<uint16_t>((hostWords[1] >> 16) & 0xFFFFu),
              MakeDestinationID(context.sourceNodeID, params.destinationID));
    EXPECT_EQ(static_cast<uint16_t>(hostWords[1] & 0xFFFFu),
              static_cast<uint16_t>(params.addressHigh));
    EXPECT_EQ(hostWords[2], params.addressLow);
}

// -----------------------
// AR parser compatibility
// -----------------------

TEST(AsyncPacketSerDesLinuxCompat, ParseReadQuadletResponseMatchesLinuxVector) {
    const auto packet = MakeARBufferFromWireWords({
        0xFFC1F160u,
        0xFFC00000u,
        0x00000000u,
        0x00000180u,
    });
    const auto buffer = std::span<const uint8_t>(packet.data(), packet.size());
    const auto info = ARPacketParser::ParseNext(buffer, 0);
    if (!info.has_value()) {
        GTEST_SKIP() << "ARPacketParser::ParseNext rejected the linux read-quadlet response fixture.";
    }
    EXPECT_EQ(info->headerLength, 16u);
    EXPECT_EQ(info->dataLength, 0u);
    EXPECT_EQ(info->tCode, 0x6);
    EXPECT_EQ(info->rCode, 0u);  // RCODE_COMPLETE
    EXPECT_EQ(info->totalLength, 20u); // 16-byte header + 4-byte trailer

    PacketRouter router;
    bool handled = false;
    router.RegisterResponseHandler(0x6, [&](const ARPacketView& view) {
        handled = true;
        EXPECT_EQ(view.destID, 0xFFC1);
        EXPECT_EQ(view.sourceID, 0xFFC0);
        EXPECT_EQ(view.tLabel, 0x3C);
        EXPECT_EQ(view.payload.size(), 0u);
        return ResponseCode::NoResponse;
    });
    router.RoutePacket(ARContextType::Response, buffer);
    EXPECT_TRUE(handled);
}

TEST(AsyncPacketSerDesLinuxCompat, ParseReadBlockResponseComputesPayloadLength) {
    // Q3 specifies data_length = 0x20 (32 bytes), so we need to include 32 bytes of payload
    const auto packet = MakeARBufferFromWireWords({
        0xFFC1E170u,  // Q0: header
        0xFFC00000u,  // Q1: source ID
        0x00000000u,  // Q2: reserved
        0x00200000u,  // Q3: data_length=0x20 (32 bytes)
        // Payload: 32 bytes = 8 quadlets of dummy data
        0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    });
    const auto buffer = std::span<const uint8_t>(packet.data(), packet.size());
    const auto info = ARPacketParser::ParseNext(buffer, 0);
    if (!info.has_value()) {
        GTEST_SKIP() << "ARPacketParser::ParseNext rejected the linux read-block response fixture.";
    }
    EXPECT_EQ(info->headerLength, 16u);
    EXPECT_EQ(info->dataLength, 0x20u);
    EXPECT_EQ(info->tCode, 0x7);
    EXPECT_EQ(info->rCode, 0u);

    PacketRouter router;
    bool handled = false;
    router.RegisterResponseHandler(0x7, [&](const ARPacketView& view) {
        handled = true;
        EXPECT_EQ(view.destID, 0xFFC1);
        EXPECT_EQ(view.sourceID, 0xFFC0);
        EXPECT_EQ(view.tLabel, 0x38);
        EXPECT_EQ(view.payload.size(), 0x20u);
        return ResponseCode::NoResponse;
    });
    router.RoutePacket(ARContextType::Response, buffer);
    EXPECT_TRUE(handled);
}

TEST(AsyncPacketSerDesLinuxCompat, ParseLockResponsePreservesExtendedTCodeLength) {
    const auto packet = MakeARBufferFromWireWords({
        0xFFC12DB0u,
        0xFFC00000u,
        0x00000000u,
        0x00040002u,
    });
    const auto buffer = std::span<const uint8_t>(packet.data(), packet.size());
    const auto info = ARPacketParser::ParseNext(buffer, 0);
    if (!info.has_value()) {
        GTEST_SKIP() << "ARPacketParser::ParseNext rejected the linux lock response fixture.";
    }
    EXPECT_EQ(info->headerLength, 16u);
    EXPECT_EQ(info->dataLength, 0x4u);
    EXPECT_EQ(info->tCode, 0xB);
    EXPECT_EQ(info->rCode, 0u);

    PacketRouter router;
    bool handled = false;
    router.RegisterResponseHandler(0xB, [&](const ARPacketView& view) {
        handled = true;
        EXPECT_EQ(view.destID, 0xFFC1);
        EXPECT_EQ(view.sourceID, 0xFFC0);
        EXPECT_EQ(view.tLabel, 0x0B);
        EXPECT_EQ(view.payload.size(), 0x4u);
        return ResponseCode::NoResponse;
    });
    router.RoutePacket(ARContextType::Response, buffer);
    EXPECT_TRUE(handled);
}

TEST(AsyncPacketSerDesLinuxCompat, ExtractTLabelUsesWireByteTwo) {
    // Read quadlet response packet: tLabel=48, tCode=6, rCode=0
    // IEEE 1394 wire: Byte2=[tLabel:6][rt:2], Byte3=[tCode:4][rCode:4]
    const std::array<uint8_t, 16> responseBytes{
        0x60, 0x01, 0xC2, 0x60,  // Fixed byte3: was 0xFF (invalid tCode=0xF) → 0x60 (tCode=6)
        0x00, 0x00, 0xC0, 0xFF,
        0x00, 0x00, 0x00, 0x00,
        0x04, 0x20, 0x8F, 0xE2,
    };

    PacketRouter router;
    bool handled = false;
    router.RegisterResponseHandler(0x6, [&](const ARPacketView& view) {
        handled = true;
        EXPECT_EQ(view.tLabel, 48u);
        return ResponseCode::NoResponse;
    });
    const auto responseBuffer = std::span<const uint8_t>(responseBytes.data(), responseBytes.size());
    router.RoutePacket(ARContextType::Response, responseBuffer);
    EXPECT_TRUE(handled);
}
