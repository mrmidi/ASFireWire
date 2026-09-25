// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioProfileRegistry.cpp
// Global profile registry dispatcher.

#include "AudioProfileRegistry.hpp"
#include "MOTU/MotuV2Profile.hpp"
#include "AVC/ApogeeDuetProfile.hpp"
#include "AVC/BeBoBProfile.hpp"
#include "AVC/MackieOnyx820iProfile.hpp"
#include "AVC/MackieOnyx400FProfile.hpp"
#include "AVC/Phase88Profile.hpp"
#include "AVC/MAudioSpecialProfile.hpp"

#include "DICE/DiceProfile.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../../Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"

#include "../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Isoch::Audio {

std::unordered_map<uint64_t, std::unique_ptr<IAudioDeviceProfile>>& AudioProfileRegistry::DynamicProfiles() {
    static std::unordered_map<uint64_t, std::unique_ptr<IAudioDeviceProfile>> profiles;
    return profiles;
}

namespace {

// The DICE profiles: one class, one spec per catalog builder, holding only
// what the device cannot report (DICE_TCAT_ARCHITECTURE.md §4.1). The catalog
// picks the builder; nothing here matches on identity (issue #115 was a
// profile matcher that disagreed with the protocol factory).
using DICE::DiceProfile;
using DICE::DiceRangeMember;

// One Venice identity covers the F16, F24 and F32; the model number is the
// capture width (documentation/fixtures/DICE/midasF24.txt).
constexpr DiceRangeMember kVeniceMembers[] = {
    {16, "Midas Venice F16"},
    {24, "Midas Venice F24"},
    {32, "Midas Venice F32"},
};

DiceProfile gFocusriteProfile{{.name = "Focusrite Saffire (DICE)"}};
DiceProfile gFocusritePro40Profile{{.name = "Focusrite Saffire Pro 40"}};
DiceProfile gMidasVeniceProfile{{.name = "Midas Venice F (DICE)", .rangeMembers = kVeniceMembers}};
DiceProfile gPreSonusStudioLiveProfile{{.name = "PreSonus StudioLive 16.0.2 (DICE)"}};
DiceProfile gPreSonusStudioLive2442Profile{{.name = "PreSonus StudioLive 24.4.2 (DICE)"}};
// Alesis leaves the non-audio slots alone, and has one playback stream whatever
// its register says (libffado dice_avdevice.cpp:1686-1700).
DiceProfile gAlesisMultiMixProfile{{.name = "Alesis MultiMix FireWire (DICE)",
                                    .initializeNonAudioSlots = false,
                                    .assertedPlaybackStreams = 1}};
// Weiss still sends AM824, unlike WeissFirewire.kext's raw path; it has never
// run on hardware, so the flip waits for evidence (§3.2).
DiceProfile gWeissIntProfile{{.name = "Weiss INT (DICE)",
                              .txEncoding = Encoding::AudioWireFormat::kAM824,
                              .preserveFdfInNoDataPackets = false}};
// The registry's last resort, for a nub whose builder did not travel.
DiceProfile gGenericDiceProfile{{.name = "Generic DICE",
                                 .txEncoding = Encoding::AudioWireFormat::kAM824,
                                 .preserveFdfInNoDataPackets = false}};

AVC::Profiles::ApogeeDuetProfile gApogeeDuetProfile{};
AVC::Profiles::Phase88Profile gPhase88Profile{};
AVC::Profiles::MackieOnyx820iProfile gMackieOnyx820iProfile{};
AVC::Profiles::MackieOnyx400FProfile gMackieOnyx400FProfile{};
AVC::Profiles::MAudioSpecialProfile gMAudio1814Profile{false};
AVC::Profiles::MAudioSpecialProfile gMAudioProjectMixProfile{true};
MOTU::Profiles::MotuV2Profile gMotuUltraliteProfile{
    DeviceProfiles::Audio::kMotuUltraliteSwVersion};
MOTU::Profiles::MotuV2Profile gMotu828mk2Profile{
    DeviceProfiles::Audio::kMotu828mk2SwVersion};

/// The DICE half, kept separate so DICE callers get the DICE profile without a
/// downcast. Returns nullptr for every non-DICE builder.
[[nodiscard]] const DICE::DiceProfile* DiceProfileForBuilder(
    DeviceProfiles::Audio::ProfileBuilderId builder) noexcept {
    using Builder = DeviceProfiles::Audio::ProfileBuilderId;
    switch (builder) {
        case Builder::FocusriteSPro40:
            return &gFocusritePro40Profile;
        case Builder::FocusriteSPro14:
        case Builder::FocusriteSPro24:
        case Builder::FocusriteSPro24Dsp:
            return &gFocusriteProfile;
        case Builder::WeissInt202:
        case Builder::WeissInt203:
            return &gWeissIntProfile;
        case Builder::AlesisMultiMix:
            return &gAlesisMultiMixProfile;
        case Builder::MidasVeniceF32:
            return &gMidasVeniceProfile;
        case Builder::PreSonusStudioLive1602:
            return &gPreSonusStudioLiveProfile;
        case Builder::PreSonusStudioLive2442:
            return &gPreSonusStudioLive2442Profile;

        // Not DICE, or DICE with no profile object on this branch.
        case Builder::FocusriteLiquidS56:
        case Builder::ApogeeDuet:
        case Builder::TerraTecPhase88:
        case Builder::MackieOnyxIOxfw:
        case Builder::MackieOnyx400F:
        case Builder::Motu828mk2:
        case Builder::MotuUltralite:
        case Builder::GenericAvc:
        case Builder::GenericBeBoB:
        case Builder::MAudioFireWire1814:
        case Builder::MAudioProjectMix:
        case Builder::None:
            break;
    }
    return nullptr;
}

/// One row per builder the catalog can resolve. No `default:` in either half --
/// a new builder that nobody taught these switches about must not compile,
/// because the alternative is a device that publishes a nub and then gets the
/// generic DICE geometry, which rejects every packet it receives.
[[nodiscard]] const IAudioDeviceProfile* ProfileForBuilder(
    DeviceProfiles::Audio::ProfileBuilderId builder) noexcept {
    using Builder = DeviceProfiles::Audio::ProfileBuilderId;
    if (const auto* dice = DiceProfileForBuilder(builder)) {
        return dice;
    }
    switch (builder) {
        case Builder::ApogeeDuet:
            return &gApogeeDuetProfile;
        case Builder::TerraTecPhase88:
            return &gPhase88Profile;
        case Builder::MAudioFireWire1814:
            return &gMAudio1814Profile;
        case Builder::MAudioProjectMix:
            return &gMAudioProjectMixProfile;
        // Asymmetric 8-in/2-out, captured from a real 820i. Falling through to
        // the generic DICE profile would hand it a symmetric 2x2/DBS-2 geometry
        // that the RX path rejects on every 8-channel packet.
        case Builder::MackieOnyxIOxfw:
            return &gMackieOnyx820iProfile;
        // Static 10x10. FireworksProtocol logs the device's HWINFO counts and
        // refuses to stream if they disagree, so a wrong guess here is loud.
        case Builder::MackieOnyx400F:
            return &gMackieOnyx400FProfile;

        // Protocol v2 covers five models whose fixed chunk counts are NOT
        // interchangeable (Linux motu-protocol-v2.c:274-320); the 8pre is even
        // asymmetric, 10/6 against 14/14. Only the two with a verified layout
        // resolve here, and the catalog names no builder for the other three.
        case Builder::Motu828mk2:
            return &gMotu828mk2Profile;
        case Builder::MotuUltralite:
            return &gMotuUltraliteProfile;

        // Handled by DiceProfileForBuilder above, or carrying no profile object
        // on this branch.
        case Builder::FocusriteSPro14:
        case Builder::FocusriteSPro24:
        case Builder::FocusriteSPro24Dsp:
        case Builder::FocusriteSPro40:
        case Builder::FocusriteLiquidS56:
        case Builder::WeissInt202:
        case Builder::WeissInt203:
        case Builder::AlesisMultiMix:
        case Builder::MidasVeniceF32:
        case Builder::PreSonusStudioLive1602:
        case Builder::PreSonusStudioLive2442:
        case Builder::GenericAvc:
        case Builder::GenericBeBoB:
        case Builder::None:
            break;
    }
    return nullptr;
}

} // namespace

const IAudioDeviceProfile* AudioProfileRegistry::ProfileForBuilderId(
    uint32_t profileBuilderId) noexcept {
    using Builder = DeviceProfiles::Audio::ProfileBuilderId;
    if (profileBuilderId == 0 ||
        profileBuilderId > static_cast<uint32_t>(Builder::kLastValid)) {
        return nullptr;
    }
    return ProfileForBuilder(static_cast<Builder>(profileBuilderId));
}

const DICE::DiceProfile* AudioProfileRegistry::DiceProfileForBuilderId(
    uint32_t profileBuilderId) noexcept {
    using Builder = DeviceProfiles::Audio::ProfileBuilderId;
    if (profileBuilderId == 0 ||
        profileBuilderId > static_cast<uint32_t>(Builder::kLastValid)) {
        return nullptr;
    }
    return DiceProfileForBuilder(static_cast<Builder>(profileBuilderId));
}

const IAudioDeviceProfile* AudioProfileRegistry::FindProfile(uint32_t vendorId,
                                                             uint32_t modelId,
                                                             uint64_t guid,
                                                             uint32_t profileBuilderId) noexcept {
    using Builder = DeviceProfiles::Audio::ProfileBuilderId;
    if (profileBuilderId != 0 &&
        profileBuilderId <= static_cast<uint32_t>(Builder::kLastValid)) {
        if (const auto* profile = ProfileForBuilder(static_cast<Builder>(profileBuilderId))) {
            return profile;
        }
    } else if (vendorId != 0) {
        // A device was identified well enough to publish a nub, but its
        // builder did not travel with it. Say so: the fallback below cannot
        // identify every family, and the failure is silent otherwise.
        ASFW_LOG_WARNING(Audio,
                         "AudioProfileRegistry: no profile builder for vendor=0x%06x "
                         "model=0x%06x guid=0x%016llx; falling back to identity matching",
                         vendorId, modelId, guid);
    }

    // Per-GUID BeBoB profiles, registered during discovery, give a BeBoB
    // device without a curated profile its discovery-derived geometry. They are
    // keyed by GUID and not by identity, so no matching happens here either.
    if (guid != 0) {
        auto& dynamic = DynamicProfiles();
        if (auto it = dynamic.find(guid); it != dynamic.end()) {
            return it->second.get();
        }
    }

    // The generic DICE fallback. Reaching it means either an unrecognised
    // device -- which is correct -- or a builder that did not travel, which the
    // warning above has already reported.
    return &gGenericDiceProfile;
}

const IAudioDeviceProfile* AudioProfileRegistry::RegisterBeBoBProfile(
    uint64_t guid, const void* discoveryModel) noexcept {
    if (guid == 0 || discoveryModel == nullptr) return nullptr;
    auto& dynamic = DynamicProfiles();
    if (dynamic.find(guid) != dynamic.end()) return dynamic[guid].get();
    const auto* model = static_cast<const ::ASFW::Audio::BeBoB::DeviceModel*>(discoveryModel);
    dynamic[guid] = std::make_unique<AVC::Profiles::BeBoBProfile>(*model);
    return dynamic[guid].get();
}

void AudioProfileRegistry::UnregisterProfile(uint64_t guid) noexcept {
    DynamicProfiles().erase(guid);
}

} // namespace ASFW::Isoch::Audio
