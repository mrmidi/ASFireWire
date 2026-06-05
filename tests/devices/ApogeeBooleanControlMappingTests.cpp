#include <gtest/gtest.h>

#include "Protocols/Audio/Oxford/Apogee/ApogeeDuetProtocol.hpp"

namespace {

using ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol;

TEST(ApogeeBooleanControlMappingTests, MapsPhantomElementOneToChannelZero) {
    uint8_t channel = 0xFF;
    const bool mapped = ApogeeDuetProtocol::TryMapBooleanControl(static_cast<uint32_t>('phan'),
                                                                  1u,
                                                                  channel);
    EXPECT_TRUE(mapped);
    EXPECT_EQ(channel, 0u);
}

TEST(ApogeeBooleanControlMappingTests, MapsPhantomElementTwoToChannelOne) {
    uint8_t channel = 0xFF;
    const bool mapped = ApogeeDuetProtocol::TryMapBooleanControl(static_cast<uint32_t>('phan'),
                                                                  2u,
                                                                  channel);
    EXPECT_TRUE(mapped);
    EXPECT_EQ(channel, 1u);
}

TEST(ApogeeBooleanControlMappingTests, MapsPhaseInvertElementsToInputChannels) {
    uint8_t channel = 0xFF;
    EXPECT_TRUE(ApogeeDuetProtocol::TryMapBooleanControl(static_cast<uint32_t>('phsi'),
                                                         1u,
                                                         channel));
    EXPECT_EQ(channel, 0u);
    EXPECT_TRUE(ApogeeDuetProtocol::TryMapBooleanControl(static_cast<uint32_t>('phsi'),
                                                         2u,
                                                         channel));
    EXPECT_EQ(channel, 1u);
}

TEST(ApogeeBooleanControlMappingTests, RejectsUnsupportedBooleanControlMappings) {
    uint8_t channel = 0xFF;
    EXPECT_FALSE(ApogeeDuetProtocol::TryMapBooleanControl(static_cast<uint32_t>('mute'),
                                                          1u,
                                                          channel));
    EXPECT_FALSE(ApogeeDuetProtocol::TryMapBooleanControl(static_cast<uint32_t>('phan'),
                                                          3u,
                                                          channel));
}

} // namespace
