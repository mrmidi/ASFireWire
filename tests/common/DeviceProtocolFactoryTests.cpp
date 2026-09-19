// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "Audio/Protocols/DeviceProtocolFactory.hpp"

namespace {

using ASFW::Audio::DeviceIntegrationMode;
using ASFW::Audio::DeviceProtocolFactory;

constexpr uint64_t MakeFocusriteGuidWithModelField(uint32_t modelField) {
    return (static_cast<uint64_t>(ASFW::DeviceProfiles::Audio::kFocusriteVendorId) << 40U) |
           (static_cast<uint64_t>(modelField & 0x3FU) << 22U);
}

TEST(DeviceProtocolFactoryTests, SelectsIntegrationModeForKnownDevices) {
    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
                  ASFW::DeviceProfiles::Audio::kSPro14ModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
                  ASFW::DeviceProfiles::Audio::kSPro24ModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
                  ASFW::DeviceProfiles::Audio::kSPro24DspModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
                  ASFW::DeviceProfiles::Audio::kSPro40ModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
                  ASFW::DeviceProfiles::Audio::kLiquidS56ModelId),
              DeviceIntegrationMode::kNone);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
                  ASFW::DeviceProfiles::Audio::kSPro26ModelId),
              DeviceIntegrationMode::kNone);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
                  ASFW::DeviceProfiles::Audio::kSPro40Tcd3070ModelId),
              DeviceIntegrationMode::kNone);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kApogeeVendorId,
                  ASFW::DeviceProfiles::Audio::kApogeeDuetModelId),
              DeviceIntegrationMode::kAVCDriven);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kAlesisVendorId,
                  ASFW::DeviceProfiles::Audio::kAlesisMultiMixModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kMidasVendorId,
                  ASFW::DeviceProfiles::Audio::kMidasVeniceModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kPreSonusVendorId,
                  ASFW::DeviceProfiles::Audio::kStudioLive1602ModelId),
              DeviceIntegrationMode::kHardcodedNub);
}

TEST(DeviceProtocolFactoryTests, RejectsUnknownDevices) {
    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(0x00ABCDEF, 0x00001234),
              DeviceIntegrationMode::kNone);
    EXPECT_FALSE(DeviceProtocolFactory::IsKnownDevice(0x00ABCDEF, 0x00001234));
}

TEST(DeviceProtocolFactoryTests, RecognizesKnownVendorModelPairs) {
    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
        ASFW::DeviceProfiles::Audio::kSPro14ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
        ASFW::DeviceProfiles::Audio::kSPro24ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
        ASFW::DeviceProfiles::Audio::kSPro24DspModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
        ASFW::DeviceProfiles::Audio::kSPro40ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
        ASFW::DeviceProfiles::Audio::kLiquidS56ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
        ASFW::DeviceProfiles::Audio::kSPro26ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
        ASFW::DeviceProfiles::Audio::kSPro40Tcd3070ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        ASFW::DeviceProfiles::Audio::kApogeeVendorId,
        ASFW::DeviceProfiles::Audio::kApogeeDuetModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        ASFW::DeviceProfiles::Audio::kAlesisVendorId,
        ASFW::DeviceProfiles::Audio::kAlesisMultiMixModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        ASFW::DeviceProfiles::Audio::kMidasVendorId,
        ASFW::DeviceProfiles::Audio::kMidasVeniceModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        ASFW::DeviceProfiles::Audio::kPreSonusVendorId,
        ASFW::DeviceProfiles::Audio::kStudioLive1602ModelId));
}

TEST(DeviceProtocolFactoryTests, InfersFocusriteIdentityFromGuid) {
    constexpr uint64_t guid =
        MakeFocusriteGuidWithModelField(ASFW::DeviceProfiles::Audio::kSPro24DspModelId);

    const auto known = DeviceProtocolFactory::LookupKnownIdentityByGuid(guid);
    ASSERT_TRUE(known.has_value());
    EXPECT_EQ(known->vendorId, ASFW::DeviceProfiles::Audio::kFocusriteVendorId);
    EXPECT_EQ(known->modelId, ASFW::DeviceProfiles::Audio::kSPro24DspModelId);
    EXPECT_EQ(known->integrationMode, DeviceIntegrationMode::kHardcodedNub);
}

TEST(DeviceProtocolFactoryTests, MapsFocusritePro40Tcd3070GuidQuirk) {
    constexpr uint64_t guid = MakeFocusriteGuidWithModelField(
        ASFW::DeviceProfiles::Audio::kFocusriteGuidModelSPro40Tcd3070);

    const auto known = DeviceProtocolFactory::LookupKnownIdentityByGuid(guid);
    ASSERT_TRUE(known.has_value());
    EXPECT_EQ(known->vendorId, ASFW::DeviceProfiles::Audio::kFocusriteVendorId);
    EXPECT_EQ(known->modelId, ASFW::DeviceProfiles::Audio::kSPro40Tcd3070ModelId);
    EXPECT_EQ(known->integrationMode, DeviceIntegrationMode::kNone);
    EXPECT_STREQ(known->modelName, ASFW::DeviceProfiles::Audio::kSPro40Tcd3070ModelName);
}

TEST(DeviceProtocolFactoryTests, KeepsOtherMultistreamFocusriteModelsRecognizedButDisabled) {
    const auto liquid56 = DeviceProtocolFactory::LookupKnownIdentity(
        ASFW::DeviceProfiles::Audio::kFocusriteVendorId, ASFW::DeviceProfiles::Audio::kLiquidS56ModelId);
    ASSERT_TRUE(liquid56.has_value());
    EXPECT_EQ(liquid56->integrationMode, DeviceIntegrationMode::kNone);
    EXPECT_STREQ(liquid56->modelName, ASFW::DeviceProfiles::Audio::kLiquidS56ModelName);

    const auto spro26 = DeviceProtocolFactory::LookupKnownIdentity(
        ASFW::DeviceProfiles::Audio::kFocusriteVendorId, ASFW::DeviceProfiles::Audio::kSPro26ModelId);
    ASSERT_TRUE(spro26.has_value());
    EXPECT_EQ(spro26->integrationMode, DeviceIntegrationMode::kNone);
    EXPECT_STREQ(spro26->modelName, ASFW::DeviceProfiles::Audio::kSPro26ModelName);
}

TEST(DeviceProtocolFactoryTests, RecognizesAlesisMultiMixDiceProfile) {
    const auto multiMix = DeviceProtocolFactory::LookupKnownIdentity(
        ASFW::DeviceProfiles::Audio::kAlesisVendorId, ASFW::DeviceProfiles::Audio::kAlesisMultiMixModelId);
    ASSERT_TRUE(multiMix.has_value());
    EXPECT_EQ(multiMix->integrationMode, DeviceIntegrationMode::kHardcodedNub);
    EXPECT_STREQ(multiMix->vendorName, ASFW::DeviceProfiles::Audio::kAlesisVendorName);
    EXPECT_STREQ(multiMix->modelName, ASFW::DeviceProfiles::Audio::kAlesisMultiMixModelName);
}

TEST(DeviceProtocolFactoryTests, RecognizesMidasVeniceDiceProfile) {
    const auto venice = DeviceProtocolFactory::LookupKnownIdentity(
        ASFW::DeviceProfiles::Audio::kMidasVendorId, ASFW::DeviceProfiles::Audio::kMidasVeniceModelId);
    ASSERT_TRUE(venice.has_value());
    EXPECT_EQ(venice->integrationMode, DeviceIntegrationMode::kHardcodedNub);
    EXPECT_STREQ(venice->vendorName, ASFW::DeviceProfiles::Audio::kMidasVendorName);
    EXPECT_STREQ(venice->modelName, ASFW::DeviceProfiles::Audio::kMidasVeniceModelName);
}

TEST(DeviceProtocolFactoryTests, RecognizesPreSonusStudioLive1602DiceProfile) {
    const auto studioLive = DeviceProtocolFactory::LookupKnownIdentity(
        ASFW::DeviceProfiles::Audio::kPreSonusVendorId, ASFW::DeviceProfiles::Audio::kStudioLive1602ModelId);
    ASSERT_TRUE(studioLive.has_value());
    EXPECT_EQ(studioLive->integrationMode, DeviceIntegrationMode::kHardcodedNub);
    EXPECT_STREQ(studioLive->vendorName, ASFW::DeviceProfiles::Audio::kPreSonusVendorName);
    EXPECT_STREQ(studioLive->modelName, ASFW::DeviceProfiles::Audio::kStudioLive1602ModelName);
}

// Issue #115: the 24.4.2 had this recognition and a DiceProfileRegistry entry but no
// DeviceProtocolFactory::Create clause, so it published a nub, allocated its isoch
// geometry, and then failed every StartIO with kIOReturnNotReady. These assertions
// cover the identity half only — nothing here executes Create's dispatch, which is
// why the gap reached hardware. Proving the clause exists needs the dispatch split
// out as a pure function (backlogged).
TEST(DeviceProtocolFactoryTests, RecognizesPreSonusStudioLive2442DiceProfile) {
    const auto studioLive = DeviceProtocolFactory::LookupKnownIdentity(
        ASFW::DeviceProfiles::Audio::kPreSonusVendorId, ASFW::DeviceProfiles::Audio::kStudioLive2442ModelId);
    ASSERT_TRUE(studioLive.has_value());
    EXPECT_EQ(studioLive->integrationMode, DeviceIntegrationMode::kHardcodedNub);
    EXPECT_STREQ(studioLive->vendorName, ASFW::DeviceProfiles::Audio::kPreSonusVendorName);
    EXPECT_STREQ(studioLive->modelName, ASFW::DeviceProfiles::Audio::kStudioLive2442ModelName);
    // The two StudioLives share the factory clause, so they must stay distinct ids.
    EXPECT_NE(ASFW::DeviceProfiles::Audio::kStudioLive2442ModelId,
              ASFW::DeviceProfiles::Audio::kStudioLive1602ModelId);
}

TEST(DeviceProtocolFactoryTests, RecognizesMackieOnyxIOxfordAsAvcDriven) {
    const auto onyxI = DeviceProtocolFactory::LookupKnownIdentity(
        ASFW::DeviceProfiles::Audio::kMackieVendorId, ASFW::DeviceProfiles::Audio::kOnyxIOxfwModelId);
    ASSERT_TRUE(onyxI.has_value());
    EXPECT_EQ(onyxI->integrationMode, DeviceIntegrationMode::kAVCDriven);
    EXPECT_STREQ(onyxI->vendorName, ASFW::DeviceProfiles::Audio::kMackieVendorName);
    EXPECT_STREQ(onyxI->modelName, ASFW::DeviceProfiles::Audio::kOnyxIOxfwModelName);
    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  ASFW::DeviceProfiles::Audio::kMackieVendorId,
                  ASFW::DeviceProfiles::Audio::kOnyxIOxfwModelId),
              DeviceIntegrationMode::kAVCDriven);
}

} // namespace
