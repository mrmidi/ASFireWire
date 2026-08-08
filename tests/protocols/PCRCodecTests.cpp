#include <gtest/gtest.h>

#include "Protocols/AVC/CMP/PCRCodec.hpp"
#include "Protocols/AVC/PCRSpace.hpp"
#include "Protocols/DV/DVConnectionPlan.hpp"

TEST(PCRCodecTests, DecodesAllOutputPlugFields) {
    constexpr uint32_t raw = 0xC53AD955u;
    EXPECT_TRUE(ASFW::CMP::PCRBits::IsOnline(raw));
    EXPECT_TRUE(ASFW::CMP::PCRBits::IsBroadcast(raw));
    EXPECT_EQ(ASFW::CMP::PCRBits::GetP2P(raw), 5u);
    EXPECT_EQ(ASFW::CMP::PCRBits::GetChannel(raw), 58u);
    EXPECT_EQ(ASFW::CMP::PCRBits::GetDataRate(raw), 3u);
    EXPECT_EQ(ASFW::CMP::PCRBits::GetOverhead(raw), 6u);
    EXPECT_EQ(ASFW::CMP::PCRBits::GetPayload(raw), 0x155u);
}

TEST(PCRCodecTests, SettersPreserveUnrelatedFields) {
    constexpr uint32_t original = 0xC53AD955u;
    const uint32_t withP2P = ASFW::CMP::PCRBits::SetP2P(original, 42);
    const uint32_t withChannel = ASFW::CMP::PCRBits::SetChannel(withP2P, 17);

    EXPECT_EQ(ASFW::CMP::PCRBits::GetP2P(withChannel), 42u);
    EXPECT_EQ(ASFW::CMP::PCRBits::GetChannel(withChannel), 17u);
    EXPECT_EQ(withChannel & ~(ASFW::CMP::PCRBits::kP2PMask |
                              ASFW::CMP::PCRBits::kChannelMask),
              original & ~(ASFW::CMP::PCRBits::kP2PMask |
                            ASFW::CMP::PCRBits::kChannelMask));
}

TEST(PCRCodecTests, HigherLevelValueRoundTrips) {
    ASFW::Protocols::AVC::PCRValue value{};
    value.online = true;
    value.broadcastConnection = true;
    value.p2pCount = 37;
    value.channel = 22;
    value.dataRate = ASFW::Protocols::AVC::SpeedCode::kS400;
    value.overhead = 9;
    value.payload = 777;

    const auto decoded =
        ASFW::Protocols::AVC::PCRValue::Decode(value.Encode());
    EXPECT_EQ(decoded.online, value.online);
    EXPECT_EQ(decoded.broadcastConnection, value.broadcastConnection);
    EXPECT_EQ(decoded.p2pCount, value.p2pCount);
    EXPECT_EQ(decoded.channel, value.channel);
    EXPECT_EQ(decoded.dataRate, value.dataRate);
    EXPECT_EQ(decoded.overhead, value.overhead);
    EXPECT_EQ(decoded.payload, value.payload);
}

TEST(PCRCodecTests, DecodesOutputMasterPlugRegister) {
    constexpr uint32_t raw = (2u << 30) | (63u << 24) | 3u;
    EXPECT_EQ(ASFW::CMP::MPRBits::GetDataRate(raw), 2u);
    EXPECT_EQ(ASFW::CMP::MPRBits::GetBroadcastChannel(raw), 63u);
    EXPECT_EQ(ASFW::CMP::MPRBits::GetPlugCount(raw), 3u);
}

TEST(PCRCodecTests, DVBandwidthMatchesReferenceFormula) {
    constexpr uint32_t opcr = (2u << 14) | 120u;
    const auto units =
        ASFW::Protocols::DV::CalculateBandwidthUnits(opcr, 2);
    ASSERT_TRUE(units.has_value());
    EXPECT_EQ(*units, 1004u);
}

TEST(PCRCodecTests, DVChannelSelectionSkipsBusyAndBroadcastChannels) {
    ASFW::IRM::ResourceSnapshot snapshot{};
    snapshot.channelsAvailable31_0 = ASFW::IRM::ChannelToBitMask(7);
    snapshot.channelsAvailable63_32 = ASFW::IRM::ChannelToBitMask(63);

    const auto channel =
        ASFW::Protocols::DV::FirstAvailableChannel(snapshot);
    ASSERT_TRUE(channel.has_value());
    EXPECT_EQ(*channel, 7u);

    snapshot.channelsAvailable31_0 = 0;
    EXPECT_FALSE(
        ASFW::Protocols::DV::FirstAvailableChannel(snapshot).has_value());
}
