// SPDX-License-Identifier: LGPL-3.0-or-later

#include <gtest/gtest.h>

#include "Protocols/Audio/DeviceProtocolFactory.hpp"

namespace {

using ASFW::Audio::DeviceIntegrationMode;
using ASFW::Audio::DeviceProtocolFactory;
using ASFW::Audio::FireWireProfileSupportStatus;
using ASFW::Audio::FireWireProtocolFamily;
using ASFW::Audio::IsKnownFireWireAudioDevice;
using ASFW::Audio::LookupBestProfile;
using ASFW::Audio::Normalize24;

constexpr uint64_t MakeFocusriteGuidWithModelField(uint32_t modelField) {
    return (static_cast<uint64_t>(DeviceProtocolFactory::kFocusriteVendorId) << 40U) |
           (static_cast<uint64_t>(modelField & 0x3FU) << 22U);
}

TEST(DeviceProtocolFactoryTests, SelectsIntegrationModeForKnownDevices) {
    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kFocusriteVendorId,
                  DeviceProtocolFactory::kSPro14ModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kFocusriteVendorId,
                  DeviceProtocolFactory::kSPro24ModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kFocusriteVendorId,
                  DeviceProtocolFactory::kSPro24DspModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kFocusriteVendorId,
                  DeviceProtocolFactory::kSPro40ModelId),
              DeviceIntegrationMode::kNone);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kFocusriteVendorId,
                  DeviceProtocolFactory::kLiquidS56ModelId),
              DeviceIntegrationMode::kNone);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kFocusriteVendorId,
                  DeviceProtocolFactory::kSPro26ModelId),
              DeviceIntegrationMode::kNone);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kFocusriteVendorId,
                  DeviceProtocolFactory::kSPro40Tcd3070ModelId),
              DeviceIntegrationMode::kNone);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kApogeeVendorId,
                  DeviceProtocolFactory::kApogeeDuetModelId),
              DeviceIntegrationMode::kAVCDriven);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kAlesisVendorId,
                  DeviceProtocolFactory::kAlesisMultiMixModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kMidasVendorId,
                  DeviceProtocolFactory::kMidasVeniceF32ModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kMidasVendorId,
                  0x000024),
              DeviceIntegrationMode::kNone);
}

TEST(DeviceProtocolFactoryTests, RejectsUnknownDevices) {
    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(0x00ABCDEF, 0x00001234),
              DeviceIntegrationMode::kNone);
    EXPECT_FALSE(DeviceProtocolFactory::IsKnownDevice(0x00ABCDEF, 0x00001234));
}

TEST(DeviceProtocolFactoryTests, NormalizesTwentyFourBitIds) {
    EXPECT_EQ(Normalize24(0x0010c73fU), 0x10c73fU);
    EXPECT_EQ(Normalize24(0xff10c73fU), 0x10c73fU);
}

TEST(DeviceProtocolFactoryTests, RecognizesKnownVendorModelPairs) {
    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kFocusriteVendorId,
        DeviceProtocolFactory::kSPro14ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kFocusriteVendorId,
        DeviceProtocolFactory::kSPro24ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kFocusriteVendorId,
        DeviceProtocolFactory::kSPro24DspModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kFocusriteVendorId,
        DeviceProtocolFactory::kSPro40ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kFocusriteVendorId,
        DeviceProtocolFactory::kLiquidS56ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kFocusriteVendorId,
        DeviceProtocolFactory::kSPro26ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kFocusriteVendorId,
        DeviceProtocolFactory::kSPro40Tcd3070ModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kApogeeVendorId,
        DeviceProtocolFactory::kApogeeDuetModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kAlesisVendorId,
        DeviceProtocolFactory::kAlesisMultiMixModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kMidasVendorId,
        DeviceProtocolFactory::kMidasVeniceF32ModelId));

    EXPECT_FALSE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kMidasVendorId,
        0x000024));
}

TEST(DeviceProtocolFactoryTests, InfersFocusriteIdentityFromGuid) {
    constexpr uint64_t guid =
        MakeFocusriteGuidWithModelField(DeviceProtocolFactory::kSPro24DspModelId);

    const auto known = DeviceProtocolFactory::LookupKnownIdentityByGuid(guid);
    ASSERT_TRUE(known.has_value());
    EXPECT_EQ(known->vendorId, DeviceProtocolFactory::kFocusriteVendorId);
    EXPECT_EQ(known->modelId, DeviceProtocolFactory::kSPro24DspModelId);
    EXPECT_EQ(known->integrationMode, DeviceIntegrationMode::kHardcodedNub);
}

TEST(DeviceProtocolFactoryTests, MapsFocusritePro40Tcd3070GuidQuirk) {
    constexpr uint64_t guid = MakeFocusriteGuidWithModelField(
        DeviceProtocolFactory::kFocusriteGuidModelSPro40Tcd3070);

    const auto known = DeviceProtocolFactory::LookupKnownIdentityByGuid(guid);
    ASSERT_TRUE(known.has_value());
    EXPECT_EQ(known->vendorId, DeviceProtocolFactory::kFocusriteVendorId);
    EXPECT_EQ(known->modelId, DeviceProtocolFactory::kSPro40Tcd3070ModelId);
    EXPECT_EQ(known->integrationMode, DeviceIntegrationMode::kNone);
    EXPECT_STREQ(known->modelName, DeviceProtocolFactory::kSPro40Tcd3070ModelName);
}

TEST(DeviceProtocolFactoryTests, KeepsDeferredMultistreamFocusriteModelsRecognizedButDisabled) {
    const auto spro40 = DeviceProtocolFactory::LookupKnownIdentity(
        DeviceProtocolFactory::kFocusriteVendorId, DeviceProtocolFactory::kSPro40ModelId);
    ASSERT_TRUE(spro40.has_value());
    EXPECT_EQ(spro40->integrationMode, DeviceIntegrationMode::kNone);
    EXPECT_STREQ(spro40->modelName, DeviceProtocolFactory::kSPro40ModelName);

    const auto liquid56 = DeviceProtocolFactory::LookupKnownIdentity(
        DeviceProtocolFactory::kFocusriteVendorId, DeviceProtocolFactory::kLiquidS56ModelId);
    ASSERT_TRUE(liquid56.has_value());
    EXPECT_EQ(liquid56->integrationMode, DeviceIntegrationMode::kNone);
    EXPECT_STREQ(liquid56->modelName, DeviceProtocolFactory::kLiquidS56ModelName);

    const auto spro26 = DeviceProtocolFactory::LookupKnownIdentity(
        DeviceProtocolFactory::kFocusriteVendorId, DeviceProtocolFactory::kSPro26ModelId);
    ASSERT_TRUE(spro26.has_value());
    EXPECT_EQ(spro26->integrationMode, DeviceIntegrationMode::kNone);
    EXPECT_STREQ(spro26->modelName, DeviceProtocolFactory::kSPro26ModelName);
}

TEST(DeviceProtocolFactoryTests, RecognizesAlesisMultiMixDiceProfile) {
    const auto multiMix = DeviceProtocolFactory::LookupKnownIdentity(
        DeviceProtocolFactory::kAlesisVendorId, DeviceProtocolFactory::kAlesisMultiMixModelId);
    ASSERT_TRUE(multiMix.has_value());
    EXPECT_EQ(multiMix->integrationMode, DeviceIntegrationMode::kHardcodedNub);
    EXPECT_STREQ(multiMix->vendorName, DeviceProtocolFactory::kAlesisVendorName);
    EXPECT_STREQ(multiMix->modelName, DeviceProtocolFactory::kAlesisMultiMixModelName);
}

TEST(DeviceProtocolFactoryTests, RecognizesSourcedMidasVeniceF32Only) {
    const auto f32 = DeviceProtocolFactory::LookupKnownIdentity(
        DeviceProtocolFactory::kMidasVendorId, DeviceProtocolFactory::kMidasVeniceF32ModelId);
    ASSERT_TRUE(f32.has_value());
    EXPECT_EQ(f32->integrationMode, DeviceIntegrationMode::kHardcodedNub);
    EXPECT_STREQ(f32->vendorName, DeviceProtocolFactory::kMidasVendorName);
    EXPECT_STREQ(f32->modelName, DeviceProtocolFactory::kMidasVeniceF32ModelName);

    const auto unknownVenice = DeviceProtocolFactory::LookupKnownIdentity(
        DeviceProtocolFactory::kMidasVendorId, 0x000024);
    EXPECT_FALSE(unknownVenice.has_value());
    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(DeviceProtocolFactory::kMidasVendorId, 0x000024),
              DeviceIntegrationMode::kNone);
}

TEST(DeviceProtocolFactoryTests, ImportsTargetFamiliesAsMetadata) {
    struct ExpectedMetadata {
        uint32_t vendor;
        uint32_t model;
        uint32_t unitSpec;
        uint32_t unitVersion;
        FireWireProtocolFamily family;
        const char* modelName;
    };

    constexpr ExpectedMetadata cases[] = {
        {0x00022eU, 0x000000U, 0x00022eU, 0x800000U, FireWireProtocolFamily::kUnknown, "FW-1884"},
        {0x0004c4U, 0x000000U, 0x0004c4U, 0x000001U, FireWireProtocolFamily::kDICE, "Zed R16"},
        {0x000ff2U, 0x010065U, 0x00a02dU, 0x010001U, FireWireProtocolFamily::kUnknown, "Mackie Onyx FireWire"},
        {0x000d6cU, 0x010081U, 0x00a02dU, 0x010001U, FireWireProtocolFamily::kBeBoB, "NRV10"},
        {0x000d6cU, 0x000010U, 0x000d6cU, 0x0100c1U, FireWireProtocolFamily::kDICE, "ProFire 2626"},
        {0x001564U, 0x00fc22U, 0x00a02dU, 0x010001U, FireWireProtocolFamily::kOxford, "FCA202"},
        {0x001486U, 0x000af2U, 0x00a02dU, 0x010000U, FireWireProtocolFamily::kFireWorks, "AudioFire2"},
        {0x0040abU, 0x010049U, 0x00a02dU, 0x010001U, FireWireProtocolFamily::kBeBoB, "FA-66"},
    };

    for (const auto& expected : cases) {
        const auto* profile = LookupBestProfile(expected.vendor,
                                                expected.model,
                                                expected.unitSpec,
                                                expected.unitVersion);
        ASSERT_NE(profile, nullptr) << expected.modelName;
        EXPECT_EQ(profile->supportStatus, FireWireProfileSupportStatus::kMetadataOnly) << expected.modelName;
        EXPECT_EQ(profile->integrationMode, DeviceIntegrationMode::kNone) << expected.modelName;
        EXPECT_EQ(profile->protocolFamily, expected.family) << expected.modelName;
        EXPECT_STREQ(profile->modelName, expected.modelName);
        EXPECT_TRUE(IsKnownFireWireAudioDevice(expected.vendor,
                                              expected.model,
                                              expected.unitSpec,
                                              expected.unitVersion));
    }
}

TEST(DeviceProtocolFactoryTests, MetadataOnlyProfilesDoNotBecomeKnownByWeakVendorModelWhenUnitIsRequired) {
    EXPECT_FALSE(DeviceProtocolFactory::IsKnownDevice(0x00022eU, 0x000000U));

    const auto tascam = DeviceProtocolFactory::LookupKnownIdentity(0x00022eU,
                                                                   0x000000U,
                                                                   0x00022eU,
                                                                   0x800000U);
    ASSERT_TRUE(tascam.has_value());
    EXPECT_EQ(tascam->supportStatus, FireWireProfileSupportStatus::kMetadataOnly);
    EXPECT_EQ(tascam->integrationMode, DeviceIntegrationMode::kNone);
    EXPECT_STREQ(tascam->modelName, "FW-1884");
}

} // namespace
