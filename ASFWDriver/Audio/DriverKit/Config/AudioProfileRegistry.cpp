// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioProfileRegistry.cpp
// Global profile registry dispatcher.

#include "AudioProfileRegistry.hpp"
#include "MOTU/MOTU828Mk3Profile.hpp"
#include "MOTU/MotuV2Profile.hpp"
#include "AVC/MackieOnyx400FProfile.hpp"
#include "AVC/GenericAvcProfile.hpp"
#include "AVC/MAudioSpecialProfile.hpp"
#include "RME/FirefaceProfile.hpp"

#include "DICE/DiceProfile.hpp"
#include "../../../Logging/Logging.hpp"

#include "../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Isoch::Audio {

namespace {

// The DICE profiles: one class, one spec per catalog builder, holding only
// what the device cannot report (DICE_TCAT_ARCHITECTURE.md §4.1). The catalog
// picks the builder; nothing here matches on identity (issue #115 was a
// profile matcher that disagreed with the protocol factory).
using DICE::DiceProfile;
using DICE::DiceRangeMember;

RME::Profiles::FirefaceProfile gRmeFireface400Profile{18U, "RME Fireface 400"};
RME::Profiles::FirefaceProfile gRmeFireface800Profile{28U, "RME Fireface 800"};

// One Venice identity covers the F16, F24 and F32; the model number is the
// capture width (documentation/fixtures/DICE/midasF24.txt).
constexpr DiceRangeMember kVeniceMembers[] = {
    {16, "Midas Venice F16"},
    {24, "Midas Venice F24"},
    {32, "Midas Venice F32"},
};

// Saffire Pro 14 / 24 / 24 DSP, calibrated on hardware (FW-182 decision D3,
// carried over from the midi branch's DICE profile builder):
// - latency 53 in / 52 out at 48 kHz, doubling per rate tier. A physical
//   loopback (tools/rtl/rtl_loopback -d "Saffire") measured RTL_ts invariant at
//   105.01 frames across buffer sizes 512/128/64; the vendor ladder (29/29) left
//   a +47-frame uncompensated residual, the calibrated pair +0.01;
// - capture safety 10 packets (80 frames at 48 kHz): one completion batch with
//   headroom, 1 ms less round trip than the vendor's 16. The timing resolver
//   floors it at one completion batch.
// - Experiment E1a declared output 56 from a measured RTL_ts of 109.03; the
//   controlled restart runs (TX_OWNERSHIP.md §1g) showed that was 105.03 plus
//   the [TxAlign] rounding (0-7 frames per start). T7 removed the rounding, so
//   output is back to the measured 52 (RTL_ts 105.03, midi 105.01).
// - What the 53/52 contain: the Pro 24 / Pro 24 DSP use the TI PCM3168A
//   (Focusrite support e-mail, quoted on Gearspace thread 469949, post
//   6195320; not checked on the board). Its datasheet (SBAS452A) group delay
//   is ADC 27/fS + DAC 28/fS = 55 frames at the 1x rates. The unreported AD/DA
//   part of the DAWbench LLP Database (January 2025) Pro 24 entry is 58-60
//   frames, which agrees. The rest is FireWire transport. Converter delay is
//   not removable latency. The Pro 40 uses the Cirrus CS4272 (same source).
// - Experiment E1b (same doc) tried capture safety 8 packets (64 frames) and
//   reverted: RTL dropped by the predicted 16 frames, but at a 16-frame
//   buffer the steady-state headroom sat at 16 with drops to 0, the input
//   starved after start-up, and the tone clicked. 10 stays.
// - E2 (same doc, §1h): playback safety 6 -> 3 packets (48 -> 24 frames).
//   RTL 337/273/241 at 64/32/16-frame buffers, residual +0.03, tone clean,
//   no late fills at 32/64; at 16 frames the fill lands on the guard with
//   no cushion (acceptable at that buffer size).
// The Pro 40 was not measured and keeps the vendor ladder.
DiceProfile gFocusriteProfile{{.name = "Focusrite Saffire (DICE)",
                               .captureSafetyPackets = 10,
                               .playbackSafetyPackets = 3,
                               .inputLatency1x = 53,
                               .outputLatency1x = 52}};
DiceProfile gFocusritePro40Profile{{.name = "Focusrite Saffire Pro 40"}};
DiceProfile gMidasVeniceProfile{{.name = "Midas Venice F (DICE)", .rangeMembers = kVeniceMembers}};
DiceProfile gAvidMboxProProfile{{.name = "Avid Mbox Pro (DICE)"}};
DiceProfile gPreSonusStudioLiveProfile{{.name = "PreSonus StudioLive 16.0.2 (DICE)"}};
DiceProfile gPreSonusStudioLive2442Profile{{.name = "PreSonus StudioLive 24.4.2 (DICE)"}};
DiceProfile gPreSonusFireStudioProjectProfile{{.name = "PreSonus FireStudio Project (DICE)"}};
// Alesis leaves the non-audio slots alone. Its stream counts come from the
// registers like every DICE device: Alesis's own kext streams every stream they
// report (AllocateStreams), where libffado forces one playback stream.
DiceProfile gAlesisMultiMixProfile{{.name = "Alesis MultiMix FireWire (DICE)",
                                    .initializeNonAudioSlots = false}};
// Weiss still sends AM824, unlike WeissFirewire.kext's raw path; it has never
// run on hardware, so the flip waits for evidence (§3.2).
DiceProfile gWeissIntProfile{{.name = "Weiss INT (DICE)",
                              .txEncoding = Encoding::AudioWireFormat::kAM824,
                              .preserveFdfInNoDataPackets = false}};
// GenericDice's profile, and the registry's last resort for a nub whose
// builder did not travel. DiceAudioBackend names the device from its identity,
// not from this profile.
DiceProfile gGenericDiceProfile{{.name = "Generic DICE",
                                 .txEncoding = Encoding::AudioWireFormat::kAM824,
                                 .preserveFdfInNoDataPackets = false}};

// Every AV/C unit published from generic discovery, BeBoB or Oxford: its
// geometry, rates and name travel on the nub, so one host-side profile serves
// all of them. Device-specific code is controls only, and lives in the
// protocol (the Duet's knob, params and meters).
AVC::Profiles::GenericAvcProfile gGenericAvcProfile{};
AVC::Profiles::MackieOnyx400FProfile gMackieOnyx400FProfile{};
AVC::Profiles::MAudioSpecialProfile gMAudio1814Profile{false};
AVC::Profiles::MAudioSpecialProfile gMAudioProjectMixProfile{true};
MOTU::Profiles::MotuV2Profile gMotuUltraliteProfile{
    DeviceProfiles::Audio::kMotuUltraliteSwVersion};
MOTU::Profiles::MotuV2Profile gMotu828mk2Profile{
    DeviceProfiles::Audio::kMotu828mk2SwVersion};
MOTU::Profiles::MOTU828Mk3Profile gMotu828mk3Profile{};

// Motu828mk3 was appended after WeissDac and kLastValid moved by hand. The
// range checks below are runtime ones: a bound left behind would let the device
// publish a nub and then fail Start() with a bare kIOReturnBadArgument.
static_assert(DeviceProfiles::Audio::ProfileBuilderId::kLastValid ==
                  DeviceProfiles::Audio::ProfileBuilderId::Motu828mk3,
              "ProfileBuilderId::kLastValid must name the last builder");

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
        case Builder::WeissDac:
            return &gWeissIntProfile;
        case Builder::GenericDice:
            return &gGenericDiceProfile;
        case Builder::AlesisMultiMix:
            return &gAlesisMultiMixProfile;
        case Builder::MidasVeniceF32:
            return &gMidasVeniceProfile;
        case Builder::AvidMboxPro:
            return &gAvidMboxProProfile;
        case Builder::PreSonusStudioLive1602:
            return &gPreSonusStudioLiveProfile;
        case Builder::PreSonusStudioLive2442:
            return &gPreSonusStudioLive2442Profile;
        case Builder::PreSonusFireStudioProject:
            return &gPreSonusFireStudioProjectProfile;

        // Not DICE, or DICE with no profile object on this branch.
        case Builder::FocusriteLiquidS56:
        case Builder::ApogeeDuet:
        case Builder::TerraTecPhase88:
        case Builder::MackieOnyxIOxfw:
        case Builder::MackieOnyx400F:
        case Builder::Motu828mk2:
        case Builder::MotuUltralite:
        case Builder::Motu828mk3:
        case Builder::GenericAvc:
        case Builder::MAudioFireWire1814:
        case Builder::MAudioProjectMix:
        case Builder::RmeFireface400:
        case Builder::RmeFireface800:
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
        case Builder::MAudioFireWire1814:
            return &gMAudio1814Profile;
        case Builder::MAudioProjectMix:
            return &gMAudioProjectMixProfile;
        // Its controls are the Duet protocol's; its streams are discovered.
        case Builder::ApogeeDuet:
        case Builder::TerraTecPhase88:
        case Builder::MackieOnyxIOxfw:
        case Builder::GenericAvc:
            return &gGenericAvcProfile;
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
        // Protocol v3: its own wire format (kMotuV3Packed) and geometry.
        case Builder::Motu828mk3:
            return &gMotu828mk3Profile;
        case Builder::RmeFireface400:
            return &gRmeFireface400Profile;
        case Builder::RmeFireface800:
            return &gRmeFireface800Profile;

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
        case Builder::AvidMboxPro:
        case Builder::PreSonusStudioLive1602:
        case Builder::PreSonusStudioLive2442:
        case Builder::PreSonusFireStudioProject:
        case Builder::GenericDice:
        case Builder::WeissDac:
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

    // The generic DICE fallback. Reaching it means either an unrecognised
    // device -- which is correct -- or a builder that did not travel, which the
    // warning above has already reported.
    return &gGenericDiceProfile;
}

} // namespace ASFW::Isoch::Audio
