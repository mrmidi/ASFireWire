// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Pins the metadata behavior of the DeviceProfiles audio layer: identity enrichment,
// Focusrite GUID inference (including the Pro 40 / TCD3070 quirk), and the integration
// mode / family for every recognized device. Ported from DeviceProtocolFactoryTests so
// the family-based profile layer is the source of truth.

#include <gtest/gtest.h>

#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "DeviceProfiles/Audio/AudioProfileRegistry.hpp"

namespace {

namespace ids = ASFW::DeviceProfiles::Audio;
using ASFW::DeviceProfiles::DeviceProfileQuery;
using ASFW::DeviceProfiles::Audio::AudioIntegrationMode;
using ASFW::DeviceProfiles::Audio::AudioProfileRegistry;
using ASFW::DeviceProfiles::Audio::AudioProtocolFamily;

constexpr uint64_t MakeFocusriteGuidWithModelField(uint32_t modelField) {
    return (static_cast<uint64_t>(ids::kFocusriteVendorId) << 40U) |
           (static_cast<uint64_t>(modelField & 0x3FU) << 22U);
}

DeviceProfileQuery ByVendorModel(uint32_t vendorId, uint32_t modelId) {
    return DeviceProfileQuery{.vendorId = vendorId, .modelId = modelId};
}

AudioIntegrationMode ModeFor(uint32_t vendorId, uint32_t modelId) {
    const auto profile =
        AudioProfileRegistry::LookupBestAudioProfile(ByVendorModel(vendorId, modelId));
    return profile.has_value() ? profile->mode : AudioIntegrationMode::kNone;
}

TEST(AudioProfileRegistryTests, SelectsIntegrationModeForKnownDevices) {
    EXPECT_EQ(ModeFor(ids::kFocusriteVendorId, ids::kSPro14ModelId),
              AudioIntegrationMode::kHardcodedNub);
    EXPECT_EQ(ModeFor(ids::kFocusriteVendorId, ids::kSPro24ModelId),
              AudioIntegrationMode::kHardcodedNub);
    EXPECT_EQ(ModeFor(ids::kFocusriteVendorId, ids::kSPro24DspModelId),
              AudioIntegrationMode::kHardcodedNub);
    EXPECT_EQ(ModeFor(ids::kFocusriteVendorId, ids::kSPro40ModelId), AudioIntegrationMode::kNone);
    EXPECT_EQ(ModeFor(ids::kFocusriteVendorId, ids::kLiquidS56ModelId), AudioIntegrationMode::kNone);
    EXPECT_EQ(ModeFor(ids::kFocusriteVendorId, ids::kSPro26ModelId), AudioIntegrationMode::kNone);
    EXPECT_EQ(ModeFor(ids::kFocusriteVendorId, ids::kSPro40Tcd3070ModelId),
              AudioIntegrationMode::kNone);
    EXPECT_EQ(ModeFor(ids::kApogeeVendorId, ids::kApogeeDuetModelId),
              AudioIntegrationMode::kAVCDriven);
    EXPECT_EQ(ModeFor(ids::kAlesisVendorId, ids::kAlesisMultiMixModelId),
              AudioIntegrationMode::kHardcodedNub);
    EXPECT_EQ(ModeFor(ids::kMidasVendorId, ids::kMidasVeniceModelId),
              AudioIntegrationMode::kHardcodedNub);
    EXPECT_EQ(ModeFor(ids::kPreSonusVendorId, ids::kStudioLive1602ModelId),
              AudioIntegrationMode::kHardcodedNub);
}

TEST(AudioProfileRegistryTests, RejectsUnknownDevices) {
    EXPECT_FALSE(
        AudioProfileRegistry::LookupIdentity(ByVendorModel(0x00ABCDEF, 0x00001234)).has_value());
    EXPECT_FALSE(AudioProfileRegistry::LookupBestAudioProfile(ByVendorModel(0x00ABCDEF, 0x00001234))
                     .has_value());
    EXPECT_EQ(ModeFor(0x00ABCDEF, 0x00001234), AudioIntegrationMode::kNone);
}

TEST(AudioProfileRegistryTests, RecognizesKnownVendorModelPairs) {
    EXPECT_TRUE(AudioProfileRegistry::LookupIdentity(
                    ByVendorModel(ids::kFocusriteVendorId, ids::kSPro14ModelId))
                    .has_value());
    EXPECT_TRUE(AudioProfileRegistry::LookupIdentity(
                    ByVendorModel(ids::kFocusriteVendorId, ids::kSPro24ModelId))
                    .has_value());
    EXPECT_TRUE(AudioProfileRegistry::LookupIdentity(
                    ByVendorModel(ids::kFocusriteVendorId, ids::kSPro24DspModelId))
                    .has_value());
    EXPECT_TRUE(AudioProfileRegistry::LookupIdentity(
                    ByVendorModel(ids::kFocusriteVendorId, ids::kSPro40ModelId))
                    .has_value());
    EXPECT_TRUE(AudioProfileRegistry::LookupIdentity(
                    ByVendorModel(ids::kFocusriteVendorId, ids::kLiquidS56ModelId))
                    .has_value());
    EXPECT_TRUE(AudioProfileRegistry::LookupIdentity(
                    ByVendorModel(ids::kFocusriteVendorId, ids::kSPro26ModelId))
                    .has_value());
    EXPECT_TRUE(AudioProfileRegistry::LookupIdentity(
                    ByVendorModel(ids::kFocusriteVendorId, ids::kSPro40Tcd3070ModelId))
                    .has_value());
    EXPECT_TRUE(AudioProfileRegistry::LookupIdentity(
                    ByVendorModel(ids::kApogeeVendorId, ids::kApogeeDuetModelId))
                    .has_value());
    EXPECT_TRUE(AudioProfileRegistry::LookupIdentity(
                    ByVendorModel(ids::kAlesisVendorId, ids::kAlesisMultiMixModelId))
                    .has_value());
    EXPECT_TRUE(AudioProfileRegistry::LookupIdentity(
                    ByVendorModel(ids::kMidasVendorId, ids::kMidasVeniceModelId))
                    .has_value());
    EXPECT_TRUE(AudioProfileRegistry::LookupIdentity(
                    ByVendorModel(ids::kPreSonusVendorId, ids::kStudioLive1602ModelId))
                    .has_value());
}

TEST(AudioProfileRegistryTests, InfersFocusriteIdentityFromGuid) {
    const uint64_t guid = MakeFocusriteGuidWithModelField(ids::kSPro24DspModelId);
    const auto identity = AudioProfileRegistry::LookupIdentity(DeviceProfileQuery{.guid = guid});
    ASSERT_TRUE(identity.has_value());
    EXPECT_EQ(identity->vendorId, ids::kFocusriteVendorId);
    EXPECT_EQ(identity->modelId, ids::kSPro24DspModelId);
    EXPECT_EQ(ModeFor(identity->vendorId, identity->modelId), AudioIntegrationMode::kHardcodedNub);
}

TEST(AudioProfileRegistryTests, MapsFocusritePro40Tcd3070GuidQuirk) {
    const uint64_t guid = MakeFocusriteGuidWithModelField(ids::kFocusriteGuidModelSPro40Tcd3070);
    const auto identity = AudioProfileRegistry::LookupIdentity(DeviceProfileQuery{.guid = guid});
    ASSERT_TRUE(identity.has_value());
    EXPECT_EQ(identity->vendorId, ids::kFocusriteVendorId);
    EXPECT_EQ(identity->modelId, ids::kSPro40Tcd3070ModelId);
    EXPECT_STREQ(identity->modelName, ids::kSPro40Tcd3070ModelName);
    EXPECT_EQ(ModeFor(identity->vendorId, identity->modelId), AudioIntegrationMode::kNone);
}

TEST(AudioProfileRegistryTests, KeepsDeferredMultistreamFocusriteModelsRecognizedButDisabled) {
    for (const uint32_t modelId :
         {ids::kSPro40ModelId, ids::kLiquidS56ModelId, ids::kSPro26ModelId}) {
        const auto identity =
            AudioProfileRegistry::LookupIdentity(ByVendorModel(ids::kFocusriteVendorId, modelId));
        ASSERT_TRUE(identity.has_value());
        EXPECT_EQ(ModeFor(ids::kFocusriteVendorId, modelId), AudioIntegrationMode::kNone);
    }
}

TEST(AudioProfileRegistryTests, RecognizesAlesisMultiMixDiceProfile) {
    const auto identity = AudioProfileRegistry::LookupIdentity(
        ByVendorModel(ids::kAlesisVendorId, ids::kAlesisMultiMixModelId));
    ASSERT_TRUE(identity.has_value());
    EXPECT_STREQ(identity->vendorName, ids::kAlesisVendorName);
    EXPECT_STREQ(identity->modelName, ids::kAlesisMultiMixModelName);

    const auto profile = AudioProfileRegistry::LookupBestAudioProfile(
        ByVendorModel(ids::kAlesisVendorId, ids::kAlesisMultiMixModelId));
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->mode, AudioIntegrationMode::kHardcodedNub);
    EXPECT_EQ(profile->family, AudioProtocolFamily::DICE);
}

TEST(AudioProfileRegistryTests, RecognizesPreSonusStudioLive1602DiceProfile) {
    const auto identity = AudioProfileRegistry::LookupIdentity(
        ByVendorModel(ids::kPreSonusVendorId, ids::kStudioLive1602ModelId));
    ASSERT_TRUE(identity.has_value());
    EXPECT_STREQ(identity->vendorName, ids::kPreSonusVendorName);
    EXPECT_STREQ(identity->modelName, ids::kStudioLive1602ModelName);

    const auto profile = AudioProfileRegistry::LookupBestAudioProfile(
        ByVendorModel(ids::kPreSonusVendorId, ids::kStudioLive1602ModelId));
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->mode, AudioIntegrationMode::kHardcodedNub);
    EXPECT_EQ(profile->family, AudioProtocolFamily::DICE);
}

TEST(AudioProfileRegistryTests, RejectsOtherPreSonusModels) {
    // PreSonus BeBoB devices (FireBox/FP10/Inspire) and the DICE FireStudio share
    // the OUI but must not resolve to the StudioLive profile.
    EXPECT_FALSE(AudioProfileRegistry::LookupIdentity(
                     ByVendorModel(ids::kPreSonusVendorId, 0x000008))
                     .has_value());
    EXPECT_FALSE(AudioProfileRegistry::LookupBestAudioProfile(
                     ByVendorModel(ids::kPreSonusVendorId, 0x000008))
                     .has_value());
}

TEST(AudioProfileRegistryTests, RecognizesMidasVeniceDiceProfile) {
    const auto identity = AudioProfileRegistry::LookupIdentity(
        ByVendorModel(ids::kMidasVendorId, ids::kMidasVeniceModelId));
    ASSERT_TRUE(identity.has_value());
    EXPECT_STREQ(identity->vendorName, ids::kMidasVendorName);
    EXPECT_STREQ(identity->modelName, ids::kMidasVeniceModelName);

    const auto profile = AudioProfileRegistry::LookupBestAudioProfile(
        ByVendorModel(ids::kMidasVendorId, ids::kMidasVeniceModelId));
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->mode, AudioIntegrationMode::kHardcodedNub);
    EXPECT_EQ(profile->family, AudioProtocolFamily::DICE);
}

} // namespace
