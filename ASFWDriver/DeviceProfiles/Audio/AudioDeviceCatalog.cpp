// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioDeviceCatalog.hpp"

#include "AudioDeviceIds.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string_view>

namespace ASFW::DeviceProfiles::Audio {

namespace {

constexpr IdentityMatchClause Root(uint32_t vendor, uint32_t model) {
    return IdentityMatchClause{.rootVendorId = MaskedValue32{vendor},
                               .rootModelId = MaskedValue32{model}};
}

constexpr IdentityMatchClause GuidEncoded(uint32_t vendor, uint32_t model) {
    constexpr uint64_t kOuiMask = 0xFFFF'FF00'0000'0000ULL;
    constexpr uint64_t kModelMask = 0x0000'0000'0FC0'0000ULL;
    return IdentityMatchClause{
        .observedGuid = MaskedValue64{
            (static_cast<uint64_t>(vendor) << 40U) |
                (static_cast<uint64_t>(model & 0x3FU) << 22U),
            kOuiMask | kModelMask},
    };
}

constexpr AudioDeviceDefinition Definition(
    DeviceDefinitionId id, uint32_t vendor, uint32_t model,
    AudioFamilyProviderId family, ProbePolicyId probe, ProfileBuilderId builder,
    SupportDisposition support, const char* vendorName, const char* modelName,
    std::optional<uint32_t> guidModel = std::nullopt,
    BootloaderCuePolicy bootloaderCue = BootloaderCuePolicy::None,
    DeviceStreamTraits streamTraits = {}) {
    auto rootClause = Root(vendor, model);
    auto guidClause = IdentityMatchClause{};

    // Cross-validated with the Linux family match tables:
    //   firewire/dice/dice.c:248-262 and its catch-all at :457-460
    //   firewire/bebob/bebob.c:350-366
    //   firewire/oxfw/oxfw.c:323-337
    //   firewire/fireworks/fireworks.c (VENDOR_LOUD, version 0x010000)
    // Constraining the selected unit prevents a non-audio sibling unit on the
    // same physical node from inheriting a root-level audio definition.
    //
    // DICE is constrained on the interface version ALONE, deliberately. Linux's
    // DICE_DEV_ENTRY_TYPICAL does also pin specifier_id == vendor, but Linux
    // only uses that macro for the devices it has verified; Weiss, the PreSonus
    // FireStudio, the SSL Duende and the TCD3070 Saffire are all written without
    // it, and the table ends with a catch-all matching VERSION alone
    // (dice.c:457-460) precisely because the specifier is not dependable across
    // the family. Pinning it here would silently stop matching any such device
    // -- the Midas Venice is a live candidate, since its vendor kext keys on
    // Unit_Spec_ID 0x10C63F while the root vendor is 0x10C73F. The root
    // vendor+model clause is what identifies the device; this only picks the
    // unit.
    const auto constrainSelectedUnit = [family](IdentityMatchClause& clause) constexpr {
        switch (family) {
            case AudioFamilyProviderId::DICE:
                clause.unitVersion = MaskedValue32{0x000001};
                break;
            case AudioFamilyProviderId::BeBoB:
                clause.unitSpecifierId = MaskedValue32{0x00A02D};
                break;
            case AudioFamilyProviderId::Fireworks:
                clause.unitSpecifierId = MaskedValue32{0x00A02D};
                clause.unitVersion = MaskedValue32{0x010000};
                break;
            case AudioFamilyProviderId::OXFW:
            case AudioFamilyProviderId::GenericAvc:
                clause.unitSpecifierId = MaskedValue32{0x00A02D};
                clause.unitVersion = MaskedValue32{0x010001};
                break;
            case AudioFamilyProviderId::MotuRegister:
                // Matched by MotuDefinition() below, which states the unit
                // constraint itself because it is the whole identity.
                break;
            case AudioFamilyProviderId::None:
                break;
        }
    };
    constrainSelectedUnit(rootClause);

    AudioDeviceDefinition result{
        .id = id,
        .variantId = static_cast<uint32_t>(id),
        .clauses = {rootClause, IdentityMatchClause{}},
        .clauseCount = 1,
        .family = family,
        .probePolicy = probe,
        .profileBuilder = builder,
        .support = support,
        .guidReliability = GuidReliability::ReliableWhenUnique,
        .streamTraits = streamTraits,
        .bootloaderCue = bootloaderCue,
        .vendorName = vendorName,
        .modelName = modelName,
    };
    if (guidModel.has_value()) {
        guidClause = GuidEncoded(vendor, *guidModel);
        constrainSelectedUnit(guidClause);
        result.clauses[1] = guidClause;
        result.clauseCount = 2;
    }
    return result;
}

// MOTU is the one family (vendor, model) cannot discriminate: the root
// directory publishes model_id 0 for every unit, and the model lives in
// Unit_Sw_Version. Matched from the unit directory, as Linux does
// (firewire/motu/motu.c:151-181, MATCH_VENDOR_ID | SPECIFIER_ID | VERSION).
//
// This is the case that motivated DeviceIdentityEvidence: a flattened query
// cannot tell a published model_id of 0 from a model_id that was never
// published, so the old matcher had to reach past it into the unit directory
// by hand. Here it is just another clause.
constexpr AudioDeviceDefinition MotuDefinition(DeviceDefinitionId id,
                                               uint32_t swVersion,
                                               ProfileBuilderId builder,
                                               SupportDisposition support,
                                               const char* modelName) {
    return AudioDeviceDefinition{
        .id = id,
        .variantId = static_cast<uint32_t>(id),
        .clauses = {IdentityMatchClause{
                        .rootVendorId = MaskedValue32{kMotuVendorId},
                        .rootModelId = MaskedValue32{0},
                        .unitSpecifierId = MaskedValue32{kMotuVendorId},
                        .unitVersion = MaskedValue32{swVersion},
                    },
                    IdentityMatchClause{}},
        .clauseCount = 1,
        .family = AudioFamilyProviderId::MotuRegister,
        .probePolicy = support == SupportDisposition::Supported
                           ? ProbePolicyId::MotuRegister
                           : ProbePolicyId::None,
        .profileBuilder = builder,
        .support = support,
        .guidReliability = GuidReliability::ReliableWhenUnique,
        .vendorName = kMotuVendorName,
        .modelName = modelName,
    };
}

// Trait shapes shared by more than one device, named once so the rows read as
// rules rather than as repeated literals.

// Every DICE part. Linux's snd-dice is unconditionally CIP_BLOCKING
// (dice-stream.c:508): 8 samples per packet plus NO-DATA packets.
constexpr DeviceStreamTraits kDiceTraits{
    .forcedStreamMode = ForcedStreamMode::Blocking,
};

// The CMP families. Linux initialises BeBoB's AMDTP with CIP_BLOCKING
// (bebob_stream.c:400-465), forces blocking transmission for every Loud OXFW
// unit (oxfw.c:189-196, SND_OXFW_QUIRK_BLOCKING_TRANSMISSION), and Fireworks
// rides the same choreography.
constexpr DeviceStreamTraits kCmpBlockingTraits{
    .forcedStreamMode = ForcedStreamMode::Blocking,
    .startShape = StreamStartShape::CmpReceiveThenTransmit,
    .cmpChoosesIsoChannel = true,
};

// As above, plus: the capture-side CIP dbs field is untrusted, so the
// configured slot count is the authority for the stride.
// The Mackie/Loud models this driver does not stream. Blocking still applies:
// snd-oxfw forces it for every Loud OXFW unit and snd-dice is unconditionally
// blocking, so it is right for both production runs. No start shape, because
// nothing starts them.
constexpr DeviceStreamTraits kMackieBlockingTraits{
    .forcedStreamMode = ForcedStreamMode::Blocking,
};

constexpr DeviceStreamTraits kCmpBlockingUntrustedStrideTraits{
    .forcedStreamMode = ForcedStreamMode::Blocking,
    .startShape = StreamStartShape::CmpReceiveThenTransmit,
    .cmpChoosesIsoChannel = true,
    .captureTrustConfiguredStride = true,
};

constexpr std::array kDefinitions{
    Definition(DeviceDefinitionId::FocusriteSPro14, kFocusriteVendorId, kSPro14ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::FocusriteSPro14, SupportDisposition::Supported,
               kFocusriteVendorName, kSPro14ModelName, kSPro14ModelId,
               BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::FocusriteSPro24, kFocusriteVendorId, kSPro24ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::FocusriteSPro24, SupportDisposition::Supported,
               kFocusriteVendorName, kSPro24ModelName, kSPro24ModelId,
               BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::FocusriteSPro24Dsp, kFocusriteVendorId,
               kSPro24DspModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::DiceTcat, ProfileBuilderId::FocusriteSPro24Dsp,
               SupportDisposition::Supported, kFocusriteVendorName,
               kSPro24DspModelName, kSPro24DspModelId, BootloaderCuePolicy::None,
               // The DSP model is the one device whose wire format depends on its
               // runtime configuration rather than its identity: 8 PCM in 9 slots
               // is raw 24-in-32, anything else is AM824.
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking,
                                  .rawPcm24In32WhenEightInNineSlots = true}),
    Definition(DeviceDefinitionId::FocusriteSPro40, kFocusriteVendorId, kSPro40ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::FocusriteSPro40, SupportDisposition::Supported,
               kFocusriteVendorName, kSPro40ModelName, kSPro40ModelId,
               BootloaderCuePolicy::None, kDiceTraits),
    // `midi` marks the Liquid Saffire 56 Supported because it carries a
    // FocusriteLiquidS56 builder. This branch does not: the LS56 bring-up work
    // is uncommitted, so a Supported row here would resolve to a builder that
    // does not exist -- the #115 failure exactly. Flipping the two fields is
    // the whole change once the builder lands.
    Definition(DeviceDefinitionId::FocusriteLiquidS56, kFocusriteVendorId,
               kLiquidS56ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kFocusriteVendorName,
               kLiquidS56ModelName, kLiquidS56ModelId, BootloaderCuePolicy::None,
               kDiceTraits),
    Definition(DeviceDefinitionId::FocusriteSPro26, kFocusriteVendorId, kSPro26ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kFocusriteVendorName,
               kSPro26ModelName, kSPro26ModelId, BootloaderCuePolicy::None,
               kDiceTraits),
    Definition(DeviceDefinitionId::FocusriteSPro40Tcd3070, kFocusriteVendorId,
               kSPro40Tcd3070ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kFocusriteVendorName,
               kSPro40Tcd3070ModelName, kFocusriteGuidModelSPro40Tcd3070,
               BootloaderCuePolicy::None, kDiceTraits),

    Definition(DeviceDefinitionId::WeissAdc2, kWeissVendorId, kWeissAdc2ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kWeissVendorName,
               kWeissAdc2ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::WeissVesta, kWeissVendorId, kWeissVestaModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kWeissVendorName,
               kWeissVestaModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::WeissDac2, kWeissVendorId, kWeissDac2ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kWeissVendorName,
               kWeissDac2ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::WeissAfi1, kWeissVendorId, kWeissAfi1ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kWeissVendorName,
               kWeissAfi1ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::WeissInt202, kWeissVendorId, kWeissInt202ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::WeissInt202, SupportDisposition::Supported,
               kWeissVendorName, kWeissInt202ModelName, std::nullopt, BootloaderCuePolicy::None,
               // Output-only in CoreAudio but duplex on the wire: host transmit
               // goes first after GLOBAL_ENABLE so its AM824 packets establish
               // the device receive-clock path. dice-weiss.c:10-35.
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking,
                                  .startShape = StreamStartShape::TransmitFirst}),
    Definition(DeviceDefinitionId::WeissDac202, kWeissVendorId, kWeissDac202ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kWeissVendorName,
               kWeissDac202ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::WeissMaya, kWeissVendorId, kWeissMayaModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kWeissVendorName,
               kWeissMayaModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::WeissInt203, kWeissVendorId, kWeissInt203ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::WeissInt203, SupportDisposition::Supported,
               kWeissVendorName, kWeissInt203ModelName, std::nullopt, BootloaderCuePolicy::None,
               // Output-only in CoreAudio but duplex on the wire: host transmit
               // goes first after GLOBAL_ENABLE so its AM824 packets establish
               // the device receive-clock path. dice-weiss.c:10-35.
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking,
                                  .startShape = StreamStartShape::TransmitFirst}),
    Definition(DeviceDefinitionId::WeissMan301, kWeissVendorId, kWeissMan301ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kWeissVendorName,
               kWeissMan301ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),

    Definition(DeviceDefinitionId::ApogeeDuet, kApogeeVendorId, kApogeeDuetModelId,
               AudioFamilyProviderId::OXFW, ProbePolicyId::OxfwAvc,
               ProfileBuilderId::ApogeeDuet, SupportDisposition::Supported,
               kApogeeVendorName, kApogeeDuetModelName, std::nullopt,
               BootloaderCuePolicy::None,
               // Discovery reports and supports non-blocking, and host playback
               // works that way, but the observed device output cadence is
               // blocking -- forcing it keeps host and device aligned.
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking,
                                  .startShape = StreamStartShape::ApogeeInterleaved,
                                  .cmpChoosesIsoChannel = true}),
    Definition(DeviceDefinitionId::TerraTecPhase88, kTerraTecVendorId,
               kPhase88RackFwModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBPlug0, ProfileBuilderId::TerraTecPhase88,
               SupportDisposition::Supported, kTerraTecVendorName,
               kPhase88RackFwModelName, std::nullopt, BootloaderCuePolicy::None,
               kCmpBlockingTraits),
    Definition(DeviceDefinitionId::AlesisMultiMix, kAlesisVendorId,
               kAlesisMultiMixModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::DiceTcat, ProfileBuilderId::AlesisMultiMix,
               SupportDisposition::Supported, kAlesisVendorName,
               kAlesisMultiMixModelName, std::nullopt, BootloaderCuePolicy::None,
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking,
                                  .clampCaptureStreamsToOne = true}),
    // Recognition only -- its geometry has never been captured, so it names no
    // builder and nothing streams it. The row exists to hold the capture-stream
    // clamp, which FFADO applies to this model as well as the MultiMix; losing
    // that when the predicate went would have been losing evidence, not
    // deleting dead code.
    Definition(DeviceDefinitionId::AlesisIo, kAlesisVendorId, kAlesisIoModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None,
               ProfileBuilderId::None, SupportDisposition::RecognizedUnsupported,
               kAlesisVendorName, kAlesisIoModelName, std::nullopt,
               BootloaderCuePolicy::None,
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking,
                                  .clampCaptureStreamsToOne = true}),
    Definition(DeviceDefinitionId::MidasVeniceF32, kMidasVendorId,
               kMidasVeniceModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::DiceTcat, ProfileBuilderId::MidasVeniceF32,
               SupportDisposition::Supported, kMidasVendorName,
               kMidasVeniceModelName, std::nullopt, BootloaderCuePolicy::None,
               kDiceTraits),
    Definition(DeviceDefinitionId::PreSonusStudioLive1602, kPreSonusVendorId,
               kStudioLive1602ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::DiceTcat, ProfileBuilderId::PreSonusStudioLive1602,
               SupportDisposition::Supported, kPreSonusVendorName,
               kStudioLive1602ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::PreSonusStudioLive1642, kPreSonusVendorId,
               kStudioLive1642ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kPreSonusVendorName,
               kStudioLive1642ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    // Anna's 24.4.2 (GUID 0x000A9204049204CB): profile landed in #122, the
    // DeviceProtocolFactory clause it was missing in #124. Its playback side is
    // asymmetric (16 + 10), which is what Stage 3 has to frame per stream.
    Definition(DeviceDefinitionId::PreSonusStudioLive2442, kPreSonusVendorId,
               kStudioLive2442ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::DiceTcat, ProfileBuilderId::PreSonusStudioLive2442,
               SupportDisposition::Supported, kPreSonusVendorName,
               kStudioLive2442ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::PreSonusStudioLive3242, kPreSonusVendorId,
               kStudioLive3242ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kPreSonusVendorName,
               kStudioLive3242ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    // ---- M-Audio "special firmware" ----
    // These three exist here for their probe policy, not for audio: this branch
    // has no MAudioSpecialProtocol, so none of them names a builder and none
    // will be streamed. What they carry is the bound on what may be *sent*.
    //
    // Without a definition, an 1814 is an ordinary AV/C unit, and AVCDiscovery
    // opens on it with generic UNIT_INFO / SUBUNIT_INFO -- two of the four
    // shapes AVC_DEVICE_HAZARDS.md H1 records as freeze-capable on this
    // firmware. ProbePolicyId::BeBoBFilteredCommandSet resolves through
    // CommandFilterFor() to AvcCommandFilterId::MAudioSpecialBeBoB, and
    // FCPTransport::SubmitCommand then refuses every frame the allowlist in
    // AVCCommandFilter.hpp does not name. So recognising these devices is what
    // makes them safe to have on the bus at all.
    //
    // The bootloader persona is not an audio endpoint and never becomes one.
    // Its BootloaderCuePolicy has no consumer on this branch; it is carried so
    // the identity is complete and so the merge with `midi` stays textual.
    Definition(DeviceDefinitionId::MAudioFireWire1814Bootloader, kMAudioVendorId,
               kMAudioFireWire1814BootloaderModelId, AudioFamilyProviderId::None,
               ProbePolicyId::NoAutomaticTraffic, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kMAudioVendorName,
               kMAudioFireWire1814BootloaderModelName, std::nullopt,
               BootloaderCuePolicy::BeBoBStartFirmware),
    Definition(DeviceDefinitionId::MAudioFireWire1814, kMAudioVendorId,
               kMAudioFireWire1814ModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBFilteredCommandSet, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kMAudioVendorName,
               kMAudioFireWire1814ModelName),
    Definition(DeviceDefinitionId::MAudioProjectMix, kMAudioVendorId,
               kMAudioProjectMixModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBFilteredCommandSet, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kMAudioVendorName,
               kMAudioProjectMixModelName),

    // ---- MOTU (vendor register protocol, matched from the unit directory) ----
    // Only the 828mkII is hardware-verified (Config ROM captured 2026-07-26).
    // The UltraLite shares its chunk layout exactly (motu-protocol-v2.c:274-282
    // vs :302-310) and differs only in a fetching-mode write the protocol
    // already handles, so it rides along. The other three resolve an identity
    // so they are named in diagnostics, and stop there until their chunk
    // layouts are confirmed against real hardware.
    MotuDefinition(DeviceDefinitionId::Motu828mk2, kMotu828mk2SwVersion,
                   ProfileBuilderId::Motu828mk2, SupportDisposition::Supported,
                   kMotu828mk2ModelName),
    MotuDefinition(DeviceDefinitionId::MotuUltralite, kMotuUltraliteSwVersion,
                   ProfileBuilderId::MotuUltralite, SupportDisposition::Supported,
                   kMotuUltraliteModelName),
    MotuDefinition(DeviceDefinitionId::Motu896hd, kMotu896hdSwVersion,
                   ProfileBuilderId::None,
                   SupportDisposition::RecognizedUnsupported, kMotu896hdModelName),
    MotuDefinition(DeviceDefinitionId::MotuTraveler, kMotuTravelerSwVersion,
                   ProfileBuilderId::None,
                   SupportDisposition::RecognizedUnsupported, kMotuTravelerModelName),
    MotuDefinition(DeviceDefinitionId::Motu8pre, kMotu8preSwVersion,
                   ProfileBuilderId::None,
                   SupportDisposition::RecognizedUnsupported, kMotu8preModelName),

    // ---- Mackie / LOUD Technologies ----
    // One OUI, three production runs. The Oxford run's shared id 0x081216 is
    // verified on a real 820i (2026-08-17 capture: unit specifier 0x00A02D
    // version 0x010001, ROM strings "Loud Technologies Inc." / "Onyx-i"); the
    // 400F is an Echo Fireworks part on version 0x010000, so the two cannot
    // collide on the unit constraint even though they share a vendor.
    //
    // kOnyx820iModelId is deliberately absent: it is still the
    // kMackieModelIdPendingCapture sentinel (0xffffffff), which is not a valid
    // 24-bit model id, and a clause built from it would be a row that can never
    // match. It gets a definition when a DICE-run unit is captured.
    Definition(DeviceDefinitionId::MackieOnyxIOxfw, kMackieVendorId,
               kOnyxIOxfwModelId, AudioFamilyProviderId::OXFW,
               ProbePolicyId::OxfwAvc, ProfileBuilderId::MackieOnyxIOxfw,
               SupportDisposition::Supported, kMackieVendorName,
               kOnyxIOxfwModelName, std::nullopt, BootloaderCuePolicy::None,
               kCmpBlockingUntrustedStrideTraits),
    Definition(DeviceDefinitionId::MackieOnyx400F, kMackieVendorId,
               kOnyx400FModelId, AudioFamilyProviderId::Fireworks,
               ProbePolicyId::FireworksEfc, ProfileBuilderId::MackieOnyx400F,
               SupportDisposition::Supported, kMackieVendorName,
               kOnyx400FModelName, std::nullopt, BootloaderCuePolicy::None,
               kCmpBlockingUntrustedStrideTraits),
    Definition(DeviceDefinitionId::MackieOnyx1640iOxfw, kMackieVendorId,
               kOnyx1640iOxfwModelId, AudioFamilyProviderId::OXFW,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kMackieVendorName,
               kOnyx1640iModelName, std::nullopt, BootloaderCuePolicy::None,
               kMackieBlockingTraits),
    Definition(DeviceDefinitionId::MackieOnyx1640iDice, kMackieVendorId,
               kOnyx1640iDiceModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kMackieVendorName,
               kOnyx1640iModelName, std::nullopt, BootloaderCuePolicy::None,
               kMackieBlockingTraits),
    Definition(DeviceDefinitionId::MackieOnyxBlackbird, kMackieVendorId,
               kOnyxBlackbirdModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kMackieVendorName,
               kOnyxBlackbirdModelName, std::nullopt, BootloaderCuePolicy::None,
               kMackieBlockingTraits),
    Definition(DeviceDefinitionId::MackieOnyx1200F, kMackieVendorId,
               kOnyx1200FModelId, AudioFamilyProviderId::Fireworks,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kMackieVendorName,
               kOnyx1200FModelName, std::nullopt, BootloaderCuePolicy::None,
               kMackieBlockingTraits),
};

// No safety rule is currently defined.
//
// The M-Audio special-firmware personas (0x00010071, 0x00010091) used to live
// here. A safety rule is a device-level kill switch: it quarantines the record,
// which stops the audio session, the family adapter, *and* FCP transport
// construction alike. That was too blunt in both directions — it refused the one
// command those devices tolerate (AVC_DEVICE_HAZARDS.md H1) while doing nothing
// to distinguish the commands that actually freeze them. They now carry
// ProbePolicyId::BeBoBFilteredCommandSet, which bounds them per-frame at the
// point frames are sent.
//
// Keep the mechanism. It remains the right expression for an identity that must
// never be touched at all, as opposed to one that must be touched carefully.
constexpr std::array<AudioSafetyRule, 0> kSafetyRules{};

[[nodiscard]] const Discovery::UnitIdentityEvidence*
FindUnit(const Discovery::DeviceRecord& device, uint32_t directoryOffset) noexcept {
    const auto it = std::ranges::find_if(
        device.identity.units,
        [directoryOffset](const Discovery::UnitIdentityEvidence& unit) {
            return unit.unitDirectoryOffset == directoryOffset;
        });
    return it != device.identity.units.end() ? &*it : nullptr;
}

[[nodiscard]] constexpr bool Valid(const MaskedValue32& value) noexcept {
    return value.mask != 0 && (value.value & ~value.mask) == 0;
}

[[nodiscard]] constexpr bool Valid(const MaskedValue64& value) noexcept {
    return value.mask != 0 && (value.value & ~value.mask) == 0;
}

[[nodiscard]] bool ClauseHasConstraint(const IdentityMatchClause& clause) noexcept {
    return clause.observedGuid || clause.busInfoWord || clause.nodeVendorOui ||
           clause.rootVendorId || clause.rootModelId || clause.unitVendorId ||
           clause.unitModelId || clause.unitSpecifierId || clause.unitVersion ||
           clause.rootVendorName || clause.rootModelName || clause.unitVendorName ||
           clause.unitModelName;
}

[[nodiscard]] bool ClauseIsValid(const IdentityMatchClause& clause) noexcept {
    if (!ClauseHasConstraint(clause)) return false;
    if (clause.observedGuid && !Valid(*clause.observedGuid)) return false;
    if (clause.busInfoWord &&
        (clause.busInfoWord->index > 63 || !Valid(clause.busInfoWord->value))) return false;
    for (const auto* value : {&clause.nodeVendorOui, &clause.rootVendorId,
                              &clause.rootModelId, &clause.unitVendorId,
                              &clause.unitModelId, &clause.unitSpecifierId,
                              &clause.unitVersion}) {
        if (*value && !Valid(**value)) return false;
    }
    return true;
}

template <typename T>
[[nodiscard]] constexpr bool MaskedConstraintsOverlap(
    const std::optional<T>& first,
    const std::optional<T>& second) noexcept {
    if (!first || !second) return true;
    return ((first->value ^ second->value) & (first->mask & second->mask)) == 0;
}

template <typename T>
[[nodiscard]] constexpr bool ExactConstraintsOverlap(
    const std::optional<T>& first,
    const std::optional<T>& second) noexcept {
    return !first || !second || *first == *second;
}

enum ClauseConstraintBit : uint16_t {
    kObservedGuid = 1U << 0U,
    kBusInfoWord = 1U << 1U,
    kNodeVendorOui = 1U << 2U,
    kRootVendorId = 1U << 3U,
    kRootModelId = 1U << 4U,
    kUnitVendorId = 1U << 5U,
    kUnitModelId = 1U << 6U,
    kUnitSpecifierId = 1U << 7U,
    kUnitVersion = 1U << 8U,
    kRootVendorName = 1U << 9U,
    kRootModelName = 1U << 10U,
    kUnitVendorName = 1U << 11U,
    kUnitModelName = 1U << 12U,
};

[[nodiscard]] constexpr uint16_t ConstraintBits(
    const IdentityMatchClause& clause) noexcept {
    return (clause.observedGuid ? kObservedGuid : 0U) |
           (clause.busInfoWord ? kBusInfoWord : 0U) |
           (clause.nodeVendorOui ? kNodeVendorOui : 0U) |
           (clause.rootVendorId ? kRootVendorId : 0U) |
           (clause.rootModelId ? kRootModelId : 0U) |
           (clause.unitVendorId ? kUnitVendorId : 0U) |
           (clause.unitModelId ? kUnitModelId : 0U) |
           (clause.unitSpecifierId ? kUnitSpecifierId : 0U) |
           (clause.unitVersion ? kUnitVersion : 0U) |
           (clause.rootVendorName ? kRootVendorName : 0U) |
           (clause.rootModelName ? kRootModelName : 0U) |
           (clause.unitVendorName ? kUnitVendorName : 0U) |
           (clause.unitModelName ? kUnitModelName : 0U);
}

[[nodiscard]] bool ClausesProvablyOverlap(const IdentityMatchClause& first,
                                          const IdentityMatchClause& second) noexcept {
    const uint16_t firstBits = ConstraintBits(first);
    const uint16_t secondBits = ConstraintBits(second);
    // If neither clause is a refinement of the other, independent Config-ROM
    // identity surfaces could still conflict at runtime, but the catalog cannot
    // prove that intersection statically. Runtime resolution remains fail-closed.
    if ((firstBits & secondBits) != firstBits &&
        (firstBits & secondBits) != secondBits) {
        return false;
    }

    const bool busOverlap = !first.busInfoWord || !second.busInfoWord ||
        first.busInfoWord->index != second.busInfoWord->index ||
        MaskedConstraintsOverlap(
            std::optional{first.busInfoWord->value},
            std::optional{second.busInfoWord->value});
    return MaskedConstraintsOverlap(first.observedGuid, second.observedGuid) &&
           busOverlap &&
           MaskedConstraintsOverlap(first.nodeVendorOui, second.nodeVendorOui) &&
           MaskedConstraintsOverlap(first.rootVendorId, second.rootVendorId) &&
           MaskedConstraintsOverlap(first.rootModelId, second.rootModelId) &&
           MaskedConstraintsOverlap(first.unitVendorId, second.unitVendorId) &&
           MaskedConstraintsOverlap(first.unitModelId, second.unitModelId) &&
           MaskedConstraintsOverlap(first.unitSpecifierId, second.unitSpecifierId) &&
           MaskedConstraintsOverlap(first.unitVersion, second.unitVersion) &&
           ExactConstraintsOverlap(first.rootVendorName, second.rootVendorName) &&
           ExactConstraintsOverlap(first.rootModelName, second.rootModelName) &&
           ExactConstraintsOverlap(first.unitVendorName, second.unitVendorName) &&
           ExactConstraintsOverlap(first.unitModelName, second.unitModelName);
}

[[nodiscard]] constexpr bool KnownFamily(AudioFamilyProviderId id) noexcept {
    return id >= AudioFamilyProviderId::GenericAvc &&
           id <= AudioFamilyProviderId::kLastValid;
}

[[nodiscard]] constexpr bool KnownProbe(ProbePolicyId id) noexcept {
    return id >= ProbePolicyId::GenericAvc && id <= ProbePolicyId::kLastValid;
}

[[nodiscard]] constexpr bool KnownBuilder(ProfileBuilderId id) noexcept {
    return id >= ProfileBuilderId::GenericAvc && id <= ProfileBuilderId::kLastValid;
}

[[nodiscard]] constexpr bool CompatibleOverlap(
    const AudioDeviceDefinition& first,
    const AudioDeviceDefinition& second) noexcept {
    return first.equivalenceClassId != 0 &&
           first.equivalenceClassId == second.equivalenceClassId &&
           first.family == second.family &&
           first.probePolicy == second.probePolicy &&
           first.commonEquivalenceProfileBuilder != ProfileBuilderId::None &&
           first.commonEquivalenceProfileBuilder == second.commonEquivalenceProfileBuilder;
}

} // namespace

std::expected<StaticAudioEndpointPlan, CatalogResolutionError>
AudioDeviceCatalog::Resolve(const Discovery::DeviceRecord& device,
                            const Discovery::UnitIdentityEvidence& unit) noexcept {
    return ResolveWithDefinitions(device, unit, kDefinitions, kSafetyRules, true);
}

std::expected<StaticAudioEndpointPlan, CatalogResolutionError>
AudioDeviceCatalog::ResolveWithDefinitions(
    const Discovery::DeviceRecord& device,
    const Discovery::UnitIdentityEvidence& unit,
    std::span<const AudioDeviceDefinition> definitions,
    std::span<const AudioSafetyRule> safetyRules,
    bool allowGenericAvcFallback) noexcept {
    if (!device.instanceId || FindUnit(device, unit.unitDirectoryOffset) == nullptr) {
        return std::unexpected(CatalogResolutionError::InvalidUnit);
    }

    for (const auto& rule : safetyRules) {
        for (uint8_t i = 0; i < rule.clauseCount; ++i) {
            if (rule.clauses[i].Matches(device.identity, unit)) {
                return std::unexpected(CatalogResolutionError::HazardousIdentity);
            }
        }
    }

    struct Match {
        const AudioDeviceDefinition* definition{nullptr};
        uint8_t clauseIndex{0};
    };
    std::vector<Match> matches;
    for (const auto& definition : definitions) {
        for (uint8_t i = 0; i < definition.clauseCount; ++i) {
            if (definition.clauses[i].Matches(device.identity, unit)) {
                matches.push_back(Match{&definition, i});
                break;
            }
        }
    }

    if (matches.empty()) {
        // 1394 TA general AV/C units advertise this exact specifier/version
        // pair. DICE uses a vendor specifier with interface version 0x000001,
        // so an unknown DICE unit cannot fall through into an FCP probe.
        // Cross-validated with protocols/ta1394/general/README.md:78 and Linux
        // firewire/dice/dice.c:248-262.
        if (!allowGenericAvcFallback ||
            unit.specifierId.value_or(0) != 0x00A02D ||
            unit.version.value_or(0) != 0x010001) {
            return std::unexpected(CatalogResolutionError::NoMatch);
        }
        return StaticAudioEndpointPlan{
            .unit = Discovery::UnitInstanceId{device.instanceId,
                                               unit.unitDirectoryOffset},
            .exactVariantId = std::nullopt,
            .family = AudioFamilyProviderId::GenericAvc,
            .probePolicy = ProbePolicyId::GenericAvc,
            .support = SupportDisposition::GenericFallback,
            .guidReliability = GuidReliability::ReliableWhenUnique,
            .persistentKeyRecipe = PersistentKeyRecipeId::ReliableObservedEui64,
            .candidates = {DeviceDefinitionId::GenericAvc},
            .candidatePlans = {{DeviceDefinitionId::GenericAvc,
                                0,
                                ProfileBuilderId::GenericAvc,
                                {},
                                device.identity.rootVendorName,
                                device.identity.rootModelName.empty()
                                    ? "Generic AV/C Audio"
                                    : device.identity.rootModelName}},
            .provenance = {{DeviceDefinitionId::GenericAvc, 0}},
            .profileBuilder = ProfileBuilderId::GenericAvc,
            .vendorName = device.identity.rootVendorName,
            .modelName = device.identity.rootModelName.empty()
                             ? "Generic AV/C Audio"
                             : device.identity.rootModelName,
        };
    }

    const auto& first = *matches.front().definition;
    if (matches.size() > 1U) {
        if (first.equivalenceClassId == 0) {
            return std::unexpected(CatalogResolutionError::AmbiguousIdentity);
        }
        for (const auto& match : matches) {
            if (match.definition->equivalenceClassId != first.equivalenceClassId ||
                match.definition->family != first.family ||
                match.definition->probePolicy != first.probePolicy ||
                match.definition->support != first.support ||
                first.commonEquivalenceProfileBuilder == ProfileBuilderId::None ||
                match.definition->commonEquivalenceProfileBuilder !=
                    first.commonEquivalenceProfileBuilder) {
                return std::unexpected(CatalogResolutionError::AmbiguousIdentity);
            }
        }
    }

    StaticAudioEndpointPlan plan{
        .unit = Discovery::UnitInstanceId{device.instanceId, unit.unitDirectoryOffset},
        .exactVariantId = matches.size() == 1U && first.variantId != 0
                              ? std::optional<uint32_t>{first.variantId}
                              : std::nullopt,
        .family = first.family,
        .probePolicy = first.probePolicy,
        .support = first.support,
        .guidReliability = first.guidReliability,
        .persistentKeyRecipe = first.persistentKeyRecipe,
        .equivalenceClassId = first.equivalenceClassId,
        .profileBuilder = matches.size() > 1U
                              ? first.commonEquivalenceProfileBuilder
                              : first.profileBuilder,
        .commonEquivalenceProfileBuilder = first.commonEquivalenceProfileBuilder,
        .streamTraits = first.streamTraits,
        .bootloaderCue = first.bootloaderCue,
        .vendorName = first.vendorName != nullptr ? first.vendorName : "",
        .modelName = first.modelName != nullptr ? first.modelName : "",
    };
    for (const auto& match : matches) {
        plan.candidates.push_back(match.definition->id);
        plan.candidatePlans.push_back(CandidateEndpointPlan{
            .definitionId = match.definition->id,
            .variantId = match.definition->variantId,
            .profileBuilder = match.definition->profileBuilder,
            .probeConstraint = match.definition->probeConstraint,
            .vendorName = match.definition->vendorName != nullptr
                              ? match.definition->vendorName
                              : "",
            .modelName = match.definition->modelName != nullptr
                             ? match.definition->modelName
                             : "",
        });
        plan.provenance.push_back(MatchProvenance{match.definition->id,
                                                  match.clauseIndex});
    }
    return plan;
}

Discovery::AvcCommandFilterId AudioDeviceCatalog::CommandFilterFor(
    const Discovery::DeviceIdentityEvidence& device) noexcept {
    const auto filterForPolicy = [](ProbePolicyId policy) {
        switch (policy) {
            case ProbePolicyId::BeBoBFilteredCommandSet:
                return Discovery::AvcCommandFilterId::MAudioSpecialBeBoB;
            case ProbePolicyId::None:
            case ProbePolicyId::NoAutomaticTraffic:
            case ProbePolicyId::GenericAvc:
            case ProbePolicyId::BeBoBPlug0:
            case ProbePolicyId::DiceTcat:
            case ProbePolicyId::OxfwAvc:
            case ProbePolicyId::FireworksEfc:
            // MOTU speaks no AV/C at all, so there is nothing to filter.
            case ProbePolicyId::MotuRegister:
                break;
        }
        return Discovery::AvcCommandFilterId::Unrestricted;
    };

    const auto matchAgainst = [&](const Discovery::UnitIdentityEvidence& unit) {
        for (const auto& definition : kDefinitions) {
            for (uint8_t i = 0; i < definition.clauseCount; ++i) {
                if (definition.clauses[i].Matches(device, unit)) {
                    return filterForPolicy(definition.probePolicy);
                }
            }
        }
        return Discovery::AvcCommandFilterId::Unrestricted;
    };

    if (device.units.empty()) {
        const Discovery::UnitIdentityEvidence emptyUnit{};
        return matchAgainst(emptyUnit);
    }
    for (const auto& unit : device.units) {
        if (const auto filter = matchAgainst(unit);
            filter != Discovery::AvcCommandFilterId::Unrestricted) {
            return filter;
        }
    }
    return Discovery::AvcCommandFilterId::Unrestricted;
}

DeviceStreamTraits AudioDeviceCatalog::StreamTraitsFor(
    const Discovery::DeviceIdentityEvidence& device) noexcept {
    const auto matchAgainst =
        [&](const Discovery::UnitIdentityEvidence& unit) -> const AudioDeviceDefinition* {
        for (const auto& definition : kDefinitions) {
            for (uint8_t i = 0; i < definition.clauseCount; ++i) {
                if (definition.clauses[i].Matches(device, unit)) {
                    return &definition;
                }
            }
        }
        return nullptr;
    };

    if (device.units.empty()) {
        const Discovery::UnitIdentityEvidence emptyUnit{};
        if (const auto* definition = matchAgainst(emptyUnit)) {
            return definition->streamTraits;
        }
        return DeviceStreamTraits{};
    }
    for (const auto& unit : device.units) {
        if (const auto* definition = matchAgainst(unit)) {
            return definition->streamTraits;
        }
    }
    return DeviceStreamTraits{};
}

std::optional<const AudioSafetyRule*>
AudioDeviceCatalog::MatchSafetyRule(
    const Discovery::DeviceIdentityEvidence& device,
    const Discovery::UnitIdentityEvidence& unit) noexcept {
    for (const auto& rule : kSafetyRules) {
        for (uint8_t i = 0; i < rule.clauseCount; ++i) {
            if (rule.clauses[i].Matches(device, unit)) {
                return &rule;
            }
        }
    }
    return std::nullopt;
}

std::optional<const AudioSafetyRule*>
AudioDeviceCatalog::MatchAnySafetyRule(
    const Discovery::DeviceIdentityEvidence& device) noexcept {
    if (device.units.empty()) {
        const Discovery::UnitIdentityEvidence emptyUnit{};
        return MatchSafetyRule(device, emptyUnit);
    }
    for (const auto& unit : device.units) {
        if (auto match = MatchSafetyRule(device, unit); match.has_value()) {
            return match;
        }
    }
    return std::nullopt;
}

std::span<const AudioDeviceDefinition> AudioDeviceCatalog::Definitions() noexcept {
    return kDefinitions;
}

std::span<const AudioSafetyRule> AudioDeviceCatalog::SafetyRules() noexcept {
    return kSafetyRules;
}

std::vector<CatalogValidationIssue> AudioDeviceCatalog::Validate() noexcept {
    return ValidateDefinitions(kDefinitions);
}

std::vector<CatalogValidationIssue> AudioDeviceCatalog::ValidateDefinitions(
    std::span<const AudioDeviceDefinition> definitions) noexcept {
    std::vector<CatalogValidationIssue> issues;
    std::set<DeviceDefinitionId> seenIds;
    for (size_t definitionIndex = 0; definitionIndex < definitions.size(); ++definitionIndex) {
        const auto& definition = definitions[definitionIndex];
        if (definition.id == DeviceDefinitionId::Unknown ||
            !seenIds.insert(definition.id).second) {
            issues.push_back({definition.id, definition.id, "duplicate definition id"});
        }
        if (definition.clauseCount == 0 ||
            definition.clauseCount > definition.clauses.size()) {
            issues.push_back({definition.id, definition.id, "invalid clause count"});
            continue;
        }
        for (uint8_t clause = 0; clause < definition.clauseCount; ++clause) {
            if (!ClauseIsValid(definition.clauses[clause])) {
                issues.push_back({definition.id, definition.id,
                                  "empty clause or invalid mask"});
            }
        }
        if (definition.support == SupportDisposition::Supported &&
            (!KnownFamily(definition.family) ||
             !KnownBuilder(definition.profileBuilder) ||
             !KnownProbe(definition.probePolicy))) {
            issues.push_back({definition.id, definition.id,
                              "supported definition lacks provider/probe/profile"});
        }
        if (definition.equivalenceClassId != 0 &&
            !KnownBuilder(definition.commonEquivalenceProfileBuilder)) {
            issues.push_back({definition.id, definition.id,
                              "equivalence definition lacks common profile"});
        }

        for (size_t priorIndex = 0; priorIndex < definitionIndex; ++priorIndex) {
            const auto& prior = definitions[priorIndex];
            bool overlappingClause = false;
            for (uint8_t clause = 0; clause < definition.clauseCount && !overlappingClause;
                 ++clause) {
                for (uint8_t priorClause = 0; priorClause < prior.clauseCount;
                     ++priorClause) {
                    if (ClausesProvablyOverlap(definition.clauses[clause],
                                               prior.clauses[priorClause])) {
                        overlappingClause = true;
                        break;
                    }
                }
            }
            if (overlappingClause && !CompatibleOverlap(definition, prior)) {
                issues.push_back({prior.id, definition.id,
                                  "provably overlapping incompatible match clauses"});
            }
        }
    }
    return issues;
}

} // namespace ASFW::DeviceProfiles::Audio
