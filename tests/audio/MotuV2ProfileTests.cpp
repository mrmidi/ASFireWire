// SPDX-License-Identifier: Apache-2.0
//
// MOTU device-profile and catalog-resolution tests.
//
// MOTU is the one audio family that (vendor_id, model_id) cannot discriminate: the root
// directory publishes model_id 0 and the model lives in the unit directory's
// Unit_Sw_Version (Linux sound/firewire/motu/motu.c:151-181).
//
// Identity values below are from a real 828mkII config ROM captured 2026-07-26:
// vendor 0x0001F2, Unit_Spec_Id 0x0001F2, Unit_Sw_Version 0x000003.

#include <gtest/gtest.h>

#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace {

using ASFW::DeviceProfiles::Audio::AudioDeviceCatalog;
using ASFW::DeviceProfiles::Audio::AudioFamilyProviderId;
using ASFW::DeviceProfiles::Audio::ProfileBuilderId;
using ASFW::DeviceProfiles::Audio::SupportDisposition;
using ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion;
using ASFW::DeviceProfiles::Audio::kMotu896hdSwVersion;
using ASFW::DeviceProfiles::Audio::kMotuTravelerSwVersion;
using ASFW::DeviceProfiles::Audio::kMotuUltraliteSwVersion;
using ASFW::DeviceProfiles::Audio::kMotu8preSwVersion;
using ASFW::DeviceProfiles::Audio::kMotuVendorId;

ASFW::Discovery::DeviceIdentityEvidence MakeMotuEvidence(uint32_t swVersion) {
    ASFW::Discovery::DeviceIdentityEvidence evidence{};
    evidence.rootVendorId = kMotuVendorId;
    evidence.rootModelId = 0U;
    evidence.rootVendorName = "MOTU";
    evidence.units.push_back(ASFW::Discovery::UnitIdentityEvidence{
        .unitDirectoryOffset = 0x400,
        .specifierId = kMotuVendorId,
        .version = swVersion,
    });
    return evidence;
}

//==============================================================================
// Profile and Catalog identity surface
//==============================================================================

TEST(MotuProfileTests, Identifies828mk2FromUnitDirectory) {
    const auto plan = AudioDeviceCatalog::Resolve(MakeMotuEvidence(kMotu828mk2SwVersion));
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->family, AudioFamilyProviderId::MotuRegister);
    EXPECT_EQ(plan->support, SupportDisposition::Supported);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::Motu828mk2);
    EXPECT_EQ(plan->vendorName, "MOTU");
    EXPECT_EQ(plan->modelName, "828mkII");
}

TEST(MotuProfileTests, Enables828mk2AudioIntegration) {
    const auto plan = AudioDeviceCatalog::Resolve(MakeMotuEvidence(kMotu828mk2SwVersion));
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->support, SupportDisposition::Supported);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::Motu828mk2);
}

TEST(MotuProfileTests, NamesUnverifiedSiblingsWithoutEnablingThem) {
    const auto plan = AudioDeviceCatalog::Resolve(MakeMotuEvidence(kMotu896hdSwVersion));
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->modelName, "896HD");
    EXPECT_EQ(plan->support, SupportDisposition::RecognizedUnsupported);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::None);
}

TEST(MotuProfileTests, RejectsMotuWithoutMatchingUnit) {
    ASFW::Discovery::DeviceIdentityEvidence evidence{};
    evidence.rootVendorId = kMotuVendorId;
    evidence.rootModelId = 0U;
    // No units matching MOTU software versions
    const auto plan = AudioDeviceCatalog::Resolve(evidence);
    EXPECT_FALSE(plan.has_value());
}

//==============================================================================
// UltraLite enablement (unit version 0x0d)
//==============================================================================

TEST(MotuProfileTests, UltraLiteIsAudioEnabledAndNamed) {
    const auto plan = AudioDeviceCatalog::Resolve(MakeMotuEvidence(0x00000du));
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->modelName, "UltraLite");
    EXPECT_EQ(plan->support, SupportDisposition::Supported);
    EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::MotuUltralite);
}

TEST(MotuProfileTests, SiblingsWithUnconfirmedLayoutsStayAudioDisabled) {
    for (const uint32_t version : {0x000005u, 0x000009u, 0x00000fu}) {
        const auto plan = AudioDeviceCatalog::Resolve(MakeMotuEvidence(version));
        ASSERT_TRUE(plan.has_value()) << "version " << version;
        EXPECT_EQ(plan->support, SupportDisposition::RecognizedUnsupported)
            << "version " << version;
        EXPECT_EQ(plan->profileBuilder, ProfileBuilderId::None)
            << "version " << version;
    }
}

TEST(MotuProfileTests, ResolvesKnownMotuSwVersionsToNames) {
    EXPECT_STREQ(AudioDeviceCatalog::MotuModelNameForSwVersion(kMotu828mk2SwVersion), "828mkII");
    EXPECT_STREQ(AudioDeviceCatalog::MotuModelNameForSwVersion(kMotu896hdSwVersion), "896HD");
    EXPECT_STREQ(AudioDeviceCatalog::MotuModelNameForSwVersion(kMotuTravelerSwVersion), "Traveler");
    EXPECT_STREQ(AudioDeviceCatalog::MotuModelNameForSwVersion(kMotuUltraliteSwVersion), "UltraLite");
    EXPECT_STREQ(AudioDeviceCatalog::MotuModelNameForSwVersion(kMotu8preSwVersion), "8pre");
    EXPECT_EQ(AudioDeviceCatalog::MotuModelNameForSwVersion(0xFFFFFF), nullptr);
}

} // namespace
