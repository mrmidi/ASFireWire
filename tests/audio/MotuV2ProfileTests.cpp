// SPDX-License-Identifier: Apache-2.0
//
// MOTU device-profile and factory-identity matching tests.
//
// MOTU is the one audio family that (vendor_id, model_id) cannot discriminate: the root
// directory publishes model_id 0 and the model lives in the unit directory's
// Unit_Sw_Version (Linux sound/firewire/motu/motu.c:151-181).
//
// Identity values below are from a real 828mkII config ROM captured 2026-07-26:
// vendor 0x0001F2, Unit_Spec_Id 0x0001F2, Unit_Sw_Version 0x000003.

#include <gtest/gtest.h>

#include "Audio/Protocols/DeviceProtocolFactory.hpp"
#include "DeviceProfiles/Audio/AudioProfileRegistry.hpp"

namespace {

using ASFW::Audio::DeviceIntegrationMode;
using ASFW::Audio::DeviceProtocolFactory;
using ASFW::DeviceProfiles::MatchSource;
using ASFW::DeviceProfiles::DeviceProfileQuery;
using ASFW::DeviceProfiles::Audio::AudioIntegrationMode;
using ASFW::DeviceProfiles::Audio::AudioProfileRegistry;
using ASFW::DeviceProfiles::Audio::AudioProtocolFamily;
using ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion;
using ASFW::DeviceProfiles::Audio::kMotu896hdSwVersion;
using ASFW::DeviceProfiles::Audio::kMotuVendorId;

constexpr DeviceProfileQuery Make828mk2Query() {
    return DeviceProfileQuery{.vendorId = kMotuVendorId,
                              .modelId = 0x000000U, // MOTU publishes no root model_id
                              .unitSpecId = kMotuVendorId,
                              .unitSwVersion = kMotu828mk2SwVersion};
}

//==============================================================================
// Profile registry
//==============================================================================

TEST(MotuProfileTests, Identifies828mk2FromUnitDirectory) {
    const auto identity = AudioProfileRegistry::LookupIdentity(Make828mk2Query());
    ASSERT_TRUE(identity.has_value());
    EXPECT_EQ(identity->vendorId, kMotuVendorId);
    EXPECT_STREQ(identity->vendorName, "MOTU");
    EXPECT_STREQ(identity->modelName, "828mkII");
    // The software version is the only stable model discriminator MOTU publishes.
    EXPECT_EQ(identity->modelId, kMotu828mk2SwVersion);
    EXPECT_EQ(identity->source, MatchSource::ConfigROM);
}

TEST(MotuProfileTests, Enables828mk2AudioIntegration) {
    const auto profile = AudioProfileRegistry::LookupBestAudioProfile(Make828mk2Query());
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->family, AudioProtocolFamily::VendorSpecific);
    EXPECT_EQ(profile->mode, AudioIntegrationMode::kHardcodedNub);
}

TEST(MotuProfileTests, NamesUnverifiedSiblingsWithoutEnablingThem) {
    auto query = Make828mk2Query();
    query.unitSwVersion = kMotu896hdSwVersion;

    const auto identity = AudioProfileRegistry::LookupIdentity(query);
    ASSERT_TRUE(identity.has_value());
    EXPECT_STREQ(identity->modelName, "896HD");

    // Recognized by name, but no protocol is constructed until the chunk layout is
    // confirmed on real hardware.
    EXPECT_FALSE(AudioProfileRegistry::LookupBestAudioProfile(query).has_value());
}

TEST(MotuProfileTests, RejectsUnknownSwVersion) {
    auto query = Make828mk2Query();
    query.unitSwVersion = 0x0000ffU;
    EXPECT_FALSE(AudioProfileRegistry::LookupIdentity(query).has_value());
    EXPECT_FALSE(AudioProfileRegistry::LookupBestAudioProfile(query).has_value());
}

TEST(MotuProfileTests, RequiresMotuSpecifierNotJustVendor) {
    // Vendor OUI alone must not match: the specifier ID must also be the MOTU OUI,
    // mirroring IEEE1394_MATCH_SPECIFIER_ID in the Linux id table.
    auto query = Make828mk2Query();
    query.unitSpecId = 0x00a02dU; // generic 1394TA audio specifier
    EXPECT_FALSE(AudioProfileRegistry::LookupIdentity(query).has_value());
}

TEST(MotuProfileTests, IgnoresQueriesWithoutUnitIdentity) {
    // A bare (vendor, model) query — the shape every pre-MOTU call site uses — must not
    // accidentally resolve to a MOTU device.
    const DeviceProfileQuery query{.vendorId = kMotuVendorId, .modelId = 0x000003U};
    EXPECT_FALSE(AudioProfileRegistry::LookupIdentity(query).has_value());
}

//==============================================================================
// Factory identity surface
//==============================================================================

TEST(MotuFactoryTests, ResolvesIntegrationModeFromUnitIdentity) {
    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  kMotuVendorId,
                  0x000000U,
                  DeviceProtocolFactory::UnitIdentity{.specId = kMotuVendorId,
                                                      .swVersion = kMotu828mk2SwVersion}),
              DeviceIntegrationMode::kHardcodedNub);
}

TEST(MotuFactoryTests, UnitIdentityDefaultsLeaveExistingLookupsUnchanged) {
    // The defaulted UnitIdentity parameter must not change the answer for any
    // model_id-matched family.
    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kAlesisVendorId,
                  DeviceProtocolFactory::kAlesisMultiMixModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(kMotuVendorId, 0x000000U),
              DeviceIntegrationMode::kNone);
}

TEST(MotuFactoryTests, RecognizesKnownDeviceViaUnitIdentity) {
    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        kMotuVendorId,
        0x000000U,
        DeviceProtocolFactory::UnitIdentity{.specId = kMotuVendorId,
                                            .swVersion = kMotu828mk2SwVersion}));

    EXPECT_FALSE(DeviceProtocolFactory::IsKnownDevice(kMotuVendorId, 0x000000U));
}

} // namespace
