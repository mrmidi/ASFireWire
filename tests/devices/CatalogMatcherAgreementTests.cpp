// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// CatalogMatcherAgreementTests.cpp
//
// Concrete regression test tables verifying discovery, catalog resolution,
// protocol choice, backend selection, and command filters across real-world
// devices against explicit, expected historical decisions (non-tautological).

#include "Audio/Protocols/DeviceProtocolChoice.hpp"
#include "Audio/Protocols/SelectProbeBootstrap.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace ASFW;
using namespace ASFW::DeviceProfiles::Audio;

constexpr uint64_t MakeFocusriteGuid(uint32_t modelField) {
    return (static_cast<uint64_t>(kFocusriteVendorId) << 40U) |
           (static_cast<uint64_t>(modelField & 0x3FU) << 22U);
}

struct DeviceTestCase {
    const char* description{nullptr};
    Discovery::DeviceIdentityEvidence evidence{};
    SupportDisposition expectedSupport{SupportDisposition::Supported};
    AudioFamilyProviderId expectedFamily{AudioFamilyProviderId::None};
    ProfileBuilderId expectedProfileBuilder{ProfileBuilderId::None};
    const char* expectedModelName{nullptr};
    std::optional<Audio::AudioBackendKind> expectedBackend{Audio::AudioBackendKind::Avc};
    Audio::ProbeBootstrap expectedBootstrap{Audio::ProbeBootstrap::Unsupported};
    Discovery::AvcCommandFilterId expectedFilter{Discovery::AvcCommandFilterId::Unrestricted};
    uint32_t expectedStartRatePinHz{0};
    ForcedStreamMode expectedForcedStreamMode{ForcedStreamMode::Unspecified};
    StreamStartShape expectedStartShape{StreamStartShape::Default};
    bool expectedCmpChoosesIsoChannel{false};
};

Discovery::DeviceIdentityEvidence MakeEvidence(
    uint32_t rootVendorId,
    uint32_t rootModelId,
    std::optional<uint64_t> guid = std::nullopt,
    std::optional<uint32_t> unitSpecId = std::nullopt,
    std::optional<uint32_t> unitVersion = std::nullopt) {
    Discovery::DeviceIdentityEvidence ev{};
    ev.rootVendorId = rootVendorId;
    ev.rootModelId = rootModelId;
    if (guid.has_value()) {
        ev.observedGuid = *guid;
    }
    Discovery::UnitIdentityEvidence unit{};
    unit.unitDirectoryOffset = 0x400;
    unit.specifierId = unitSpecId;
    unit.version = unitVersion;
    ev.units.push_back(unit);
    return ev;
}

const std::vector<DeviceTestCase>& GetHistoricalRegressionTable() {
    static const std::vector<DeviceTestCase> kTable = {
        // 1. Focusrite Saffire Pro 24 DSP
        {
            .description = "Focusrite Saffire Pro 24 DSP (DICE, supported)",
            .evidence = MakeEvidence(kFocusriteVendorId, kSPro24DspModelId, std::nullopt,
                                     std::nullopt, 0x000001),
            .expectedSupport = SupportDisposition::Supported,
            .expectedFamily = AudioFamilyProviderId::DICE,
            .expectedProfileBuilder = ProfileBuilderId::FocusriteSPro24Dsp,
            .expectedModelName = kSPro24DspModelName,
            .expectedBackend = Audio::AudioBackendKind::Dice,
            .expectedBootstrap = Audio::ProbeBootstrap::DiceProtocol,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
            .expectedForcedStreamMode = ForcedStreamMode::Blocking,
        },
        // 2. Focusrite Saffire Pro 40
        {
            .description = "Focusrite Saffire Pro 40 (DICE, supported)",
            .evidence = MakeEvidence(kFocusriteVendorId, kSPro40ModelId, std::nullopt,
                                     std::nullopt, 0x000001),
            .expectedSupport = SupportDisposition::Supported,
            .expectedFamily = AudioFamilyProviderId::DICE,
            .expectedProfileBuilder = ProfileBuilderId::FocusriteSPro40,
            .expectedModelName = kSPro40ModelName,
            .expectedBackend = Audio::AudioBackendKind::Dice,
            .expectedBootstrap = Audio::ProbeBootstrap::DiceProtocol,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
            .expectedForcedStreamMode = ForcedStreamMode::Blocking,
        },
        // 3. Focusrite Saffire Pro 40 (TCD3070 GUID quirk)
        {
            .description = "Focusrite Saffire Pro 40 TCD3070 (GUID quirk, recognized unsupported)",
            .evidence = MakeEvidence(kFocusriteVendorId, 0,
                                     MakeFocusriteGuid(kFocusriteGuidModelSPro40Tcd3070),
                                     std::nullopt, 0x000001),
            .expectedSupport = SupportDisposition::RecognizedUnsupported,
            .expectedFamily = AudioFamilyProviderId::DICE,
            .expectedProfileBuilder = ProfileBuilderId::None,
            .expectedModelName = kSPro40Tcd3070ModelName,
            .expectedBackend = std::nullopt, // unsupported DICE devices do not route to any backend
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
            .expectedForcedStreamMode = ForcedStreamMode::Blocking,
        },
        // 4. Apogee Duet
        {
            .description = "Apogee Duet (OXFW, supported)",
            .evidence = MakeEvidence(kApogeeVendorId, kApogeeDuetModelId, std::nullopt,
                                     0x00A02D, 0x010001),
            .expectedSupport = SupportDisposition::Supported,
            .expectedFamily = AudioFamilyProviderId::OXFW,
            .expectedProfileBuilder = ProfileBuilderId::ApogeeDuet,
            .expectedModelName = kApogeeDuetModelName,
            .expectedBackend = Audio::AudioBackendKind::Avc,
            .expectedBootstrap = Audio::ProbeBootstrap::AvcInitializeThenPlug0,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
            .expectedStartRatePinHz = 48000U,
            .expectedForcedStreamMode = ForcedStreamMode::Blocking,
            .expectedStartShape = StreamStartShape::ApogeeInterleaved,
            .expectedCmpChoosesIsoChannel = true,
        },
        // 5. Mackie Onyx-i (Oxford run)
        {
            .description = "Mackie Onyx-i Oxford (OXFW, supported)",
            .evidence = MakeEvidence(kMackieVendorId, kOnyxIOxfwModelId, std::nullopt,
                                     0x00A02D, 0x010001),
            .expectedSupport = SupportDisposition::Supported,
            .expectedFamily = AudioFamilyProviderId::OXFW,
            .expectedProfileBuilder = ProfileBuilderId::MackieOnyxIOxfw,
            .expectedModelName = kOnyxIOxfwModelName,
            .expectedBackend = Audio::AudioBackendKind::Avc,
            .expectedBootstrap = Audio::ProbeBootstrap::AvcInitializeThenPlug0,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
            .expectedStartRatePinHz = 44100U,
            .expectedForcedStreamMode = ForcedStreamMode::Blocking,
            .expectedStartShape = StreamStartShape::CmpReceiveThenTransmit,
            .expectedCmpChoosesIsoChannel = true,
        },
        // 6. Mackie Onyx 400F (Echo Fireworks)
        {
            .description = "Mackie Onyx 400F (Fireworks, supported)",
            .evidence = MakeEvidence(kMackieVendorId, kOnyx400FModelId, std::nullopt,
                                     0x00A02D, 0x010000),
            .expectedSupport = SupportDisposition::Supported,
            .expectedFamily = AudioFamilyProviderId::Fireworks,
            .expectedProfileBuilder = ProfileBuilderId::MackieOnyx400F,
            .expectedModelName = kOnyx400FModelName,
            .expectedBackend = Audio::AudioBackendKind::Avc,
            .expectedBootstrap = Audio::ProbeBootstrap::FireworksEfc,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
            .expectedStartRatePinHz = 44100U,
            .expectedForcedStreamMode = ForcedStreamMode::Blocking,
            .expectedStartShape = StreamStartShape::CmpReceiveThenTransmit,
            .expectedCmpChoosesIsoChannel = true,
        },
        // 7. PreSonus StudioLive 16.0.2
        {
            .description = "PreSonus StudioLive 16.0.2 (DICE, supported)",
            .evidence = MakeEvidence(kPreSonusVendorId, kStudioLive1602ModelId, std::nullopt,
                                     std::nullopt, 0x000001),
            .expectedSupport = SupportDisposition::Supported,
            .expectedFamily = AudioFamilyProviderId::DICE,
            .expectedProfileBuilder = ProfileBuilderId::PreSonusStudioLive1602,
            .expectedModelName = kStudioLive1602ModelName,
            .expectedBackend = Audio::AudioBackendKind::Dice,
            .expectedBootstrap = Audio::ProbeBootstrap::DiceProtocol,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
            .expectedForcedStreamMode = ForcedStreamMode::Blocking,
        },
        // 8. PreSonus StudioLive 24.4.2
        {
            .description = "PreSonus StudioLive 24.4.2 (DICE, supported)",
            .evidence = MakeEvidence(kPreSonusVendorId, kStudioLive2442ModelId, std::nullopt,
                                     std::nullopt, 0x000001),
            .expectedSupport = SupportDisposition::Supported,
            .expectedFamily = AudioFamilyProviderId::DICE,
            .expectedProfileBuilder = ProfileBuilderId::PreSonusStudioLive2442,
            .expectedModelName = kStudioLive2442ModelName,
            .expectedBackend = Audio::AudioBackendKind::Dice,
            .expectedBootstrap = Audio::ProbeBootstrap::DiceProtocol,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
            .expectedForcedStreamMode = ForcedStreamMode::Blocking,
        },
        // 9. MOTU 828mk2
        {
            .description = "MOTU 828mk2 (MotuRegister, supported)",
            .evidence = MakeEvidence(kMotuVendorId, 0, std::nullopt,
                                     kMotuVendorId, kMotu828mk2SwVersion),
            .expectedSupport = SupportDisposition::Supported,
            .expectedFamily = AudioFamilyProviderId::MotuRegister,
            .expectedProfileBuilder = ProfileBuilderId::Motu828mk2,
            .expectedModelName = kMotu828mk2ModelName,
            .expectedBackend = Audio::AudioBackendKind::MotuRegister,
            .expectedBootstrap = Audio::ProbeBootstrap::MotuRegister,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
        },
        // 10. MOTU UltraLite
        {
            .description = "MOTU UltraLite (MotuRegister, supported)",
            .evidence = MakeEvidence(kMotuVendorId, 0, std::nullopt,
                                     kMotuVendorId, kMotuUltraliteSwVersion),
            .expectedSupport = SupportDisposition::Supported,
            .expectedFamily = AudioFamilyProviderId::MotuRegister,
            .expectedProfileBuilder = ProfileBuilderId::MotuUltralite,
            .expectedModelName = kMotuUltraliteModelName,
            .expectedBackend = Audio::AudioBackendKind::MotuRegister,
            .expectedBootstrap = Audio::ProbeBootstrap::MotuRegister,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
        },
        // 11. M-Audio FireWire 1814
        {
            .description = "M-Audio FireWire 1814 (BeBoB, filtered command set)",
            .evidence = MakeEvidence(kMAudioVendorId, kMAudioFireWire1814ModelId, std::nullopt,
                                     0x00A02D, std::nullopt),
            .expectedSupport = SupportDisposition::RecognizedUnsupported,
            .expectedFamily = AudioFamilyProviderId::BeBoB,
            .expectedProfileBuilder = ProfileBuilderId::None,
            .expectedModelName = kMAudioFireWire1814ModelName,
            .expectedBackend = std::nullopt,
            .expectedBootstrap = Audio::ProbeBootstrap::BeBoBUnprobed,
            .expectedFilter = Discovery::AvcCommandFilterId::MAudioSpecialBeBoB,
        },
        // 12. M-Audio FireWire 1814 Bootloader
        {
            .description = "M-Audio FireWire 1814 Bootloader (persona)",
            .evidence = MakeEvidence(kMAudioVendorId, kMAudioFireWire1814BootloaderModelId,
                                     std::nullopt, std::nullopt, std::nullopt),
            .expectedSupport = SupportDisposition::RecognizedUnsupported,
            .expectedFamily = AudioFamilyProviderId::None,
            .expectedProfileBuilder = ProfileBuilderId::None,
            .expectedModelName = kMAudioFireWire1814BootloaderModelName,
            .expectedBackend = std::nullopt,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
        },
        // 13. Alesis MultiMix
        {
            .description = "Alesis MultiMix (DICE, supported)",
            .evidence = MakeEvidence(kAlesisVendorId, kAlesisMultiMixModelId, std::nullopt,
                                     std::nullopt, 0x000001),
            .expectedSupport = SupportDisposition::Supported,
            .expectedFamily = AudioFamilyProviderId::DICE,
            .expectedProfileBuilder = ProfileBuilderId::AlesisMultiMix,
            .expectedModelName = kAlesisMultiMixModelName,
            .expectedBackend = Audio::AudioBackendKind::Dice,
            .expectedBootstrap = Audio::ProbeBootstrap::DiceProtocol,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
            .expectedForcedStreamMode = ForcedStreamMode::Blocking,
        },
        // 14. Weiss INT202
        {
            .description = "Weiss INT202 (DICE, supported)",
            .evidence = MakeEvidence(kWeissVendorId, kWeissInt202ModelId, std::nullopt,
                                     std::nullopt, 0x000001),
            .expectedSupport = SupportDisposition::Supported,
            .expectedFamily = AudioFamilyProviderId::DICE,
            .expectedProfileBuilder = ProfileBuilderId::WeissInt202,
            .expectedModelName = kWeissInt202ModelName,
            .expectedBackend = Audio::AudioBackendKind::Dice,
            .expectedBootstrap = Audio::ProbeBootstrap::DiceProtocol,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
            .expectedForcedStreamMode = ForcedStreamMode::Blocking,
            .expectedStartShape = StreamStartShape::TransmitFirst,
        },
        // 15. Generic 1394TA AV/C soundcard
        {
            .description = "Generic 1394TA AV/C audio unit (general fallback)",
            .evidence = MakeEvidence(0x001234, 0x005678, std::nullopt,
                                     0x00A02D, 0x010001),
            .expectedSupport = SupportDisposition::GenericFallback,
            .expectedFamily = AudioFamilyProviderId::GenericAvc,
            .expectedProfileBuilder = ProfileBuilderId::GenericAvc,
            .expectedModelName = "Generic AV/C Audio",
            .expectedBackend = Audio::AudioBackendKind::Avc,
            .expectedBootstrap = Audio::ProbeBootstrap::AvcInitializeThenPlug0,
            .expectedFilter = Discovery::AvcCommandFilterId::Unrestricted,
        },
    };
    return kTable;
}

TEST(CatalogMatcherAgreement, HistoricalDecisionsRegressionTable) {
    for (const auto& testCase : GetHistoricalRegressionTable()) {
        SCOPED_TRACE(testCase.description);

        // 1. Catalog Resolution
        const auto plan = AudioDeviceCatalog::Resolve(testCase.evidence);
        ASSERT_TRUE(plan.has_value()) << "Failed to resolve: " << testCase.description;
        EXPECT_EQ(plan->support, testCase.expectedSupport);
        EXPECT_EQ(plan->family, testCase.expectedFamily);
        EXPECT_EQ(plan->profileBuilder, testCase.expectedProfileBuilder);
        if (testCase.expectedSupport == SupportDisposition::Supported) {
            EXPECT_NE(plan->protocolImplementation, ProtocolImplementationId::None);
        } else {
            EXPECT_EQ(plan->protocolImplementation, ProtocolImplementationId::None);
        }
        if (testCase.expectedModelName != nullptr) {
            EXPECT_EQ(plan->modelName, testCase.expectedModelName);
        }
        EXPECT_EQ(plan->streamTraits.startRatePinHz, testCase.expectedStartRatePinHz);
        EXPECT_EQ(plan->streamTraits.forcedStreamMode, testCase.expectedForcedStreamMode);
        EXPECT_EQ(plan->streamTraits.startShape, testCase.expectedStartShape);
        EXPECT_EQ(plan->streamTraits.cmpChoosesIsoChannel, testCase.expectedCmpChoosesIsoChannel);

        // Build DeviceRecord to check protocol/backend choice
        Discovery::DeviceRecord record{};
        record.instanceId = Discovery::DeviceInstanceId{1};
        record.guid = testCase.evidence.observedGuid != 0
                          ? testCase.evidence.observedGuid
                          : 0x0011223344556677ULL;
        record.identity = testCase.evidence;

        // 2. Protocol Choice
        const auto protocolChoice = Audio::ChooseDeviceProtocol(record);
        const auto protocolFromPlan = Audio::ChooseDeviceProtocol(*plan);
        EXPECT_EQ(protocolChoice.has_value(), protocolFromPlan.has_value());
        if (testCase.expectedProfileBuilder != ProfileBuilderId::None) {
            ASSERT_TRUE(protocolChoice.has_value());
            ASSERT_TRUE(protocolFromPlan.has_value());
            EXPECT_EQ(protocolChoice->builder, testCase.expectedProfileBuilder);
            EXPECT_EQ(protocolChoice->implementation, plan->protocolImplementation);
            EXPECT_EQ(protocolFromPlan->builder, protocolChoice->builder);
            EXPECT_EQ(protocolFromPlan->implementation, protocolChoice->implementation);
            EXPECT_EQ(protocolChoice->unitDirectoryOffset, 0x400U);
        } else {
            EXPECT_FALSE(protocolChoice.has_value());
        }

        // 3. Audio Backend Choice
        const auto backend = Audio::ChooseAudioBackend(record);
        EXPECT_EQ(backend, testCase.expectedBackend);
        EXPECT_EQ(Audio::ChooseAudioBackend(*plan), backend);
        EXPECT_EQ(Audio::SelectProbeBootstrap(*plan), testCase.expectedBootstrap);

        // 4. Command Filter Choice
        const auto filter = AudioDeviceCatalog::CommandFilterFor(testCase.evidence);
        EXPECT_EQ(filter, testCase.expectedFilter);
        EXPECT_EQ(AudioDeviceCatalog::CommandFilterFor(*plan), filter);
    }
}

} // namespace
