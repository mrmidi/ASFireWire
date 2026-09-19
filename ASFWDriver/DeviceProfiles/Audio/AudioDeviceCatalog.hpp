// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../Common/DeviceIdentityMatch.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ASFW::DeviceProfiles::Audio {

enum class DeviceDefinitionId : uint32_t {
    Unknown = 0,
    GenericAvc,
    FocusriteSPro14,
    FocusriteSPro24,
    FocusriteSPro24Dsp,
    FocusriteSPro40,
    FocusriteLiquidS56,
    FocusriteSPro26,
    FocusriteSPro40Tcd3070,
    WeissAdc2,
    WeissVesta,
    WeissDac2,
    WeissAfi1,
    WeissInt202,
    WeissDac202,
    WeissMaya,
    WeissInt203,
    WeissMan301,
    ApogeeDuet,
    TerraTecPhase88,
    AlesisMultiMix,
    AlesisIo,
    MidasVeniceF32,
    PreSonusStudioLive1602,
    PreSonusStudioLive1642,
    PreSonusStudioLive2442,
    PreSonusStudioLive3242,
    MAudioFireWire1814Bootloader,
    MAudioFireWire1814,
    MAudioProjectMix,
    // MOTU publishes root model_id 0; the model lives in Unit_Sw_Version, so
    // these are the one family matched from the unit directory alone.
    Motu828mk2,
    Motu896hd,
    MotuTraveler,
    MotuUltralite,
    Motu8pre,
    // Mackie/LOUD shipped the Onyx-i line in three production runs (Oxford,
    // DICE, Echo Fireworks) under one OUI. See AudioDeviceIds.hpp for the
    // per-id provenance.
    MackieOnyxIOxfw,
    MackieOnyx1640iOxfw,
    MackieOnyx1640iDice,
    MackieOnyxBlackbird,
    MackieOnyx400F,
    MackieOnyx1200F,
};

enum class AudioFamilyProviderId : uint8_t {
    None = 0,
    GenericAvc,
    BeBoB,
    DICE,
    OXFW,
    /// Echo Fireworks (EFC control on top of AV/C + CMP streaming).
    Fireworks,
    /// MOTU's vendor register protocol. No AV/C at all.
    MotuRegister,

    /// Alias for the last real member; see the note on ProfileBuilderId.
    kLastValid = MotuRegister,
};

enum class ProbePolicyId : uint8_t {
    None = 0,
    NoAutomaticTraffic,
    GenericAvc,
    BeBoBPlug0,
    DiceTcat,
    OxfwAvc,
    /// Firmware that hangs on AV/C it does not implement. Nothing is sent
    /// automatically; the few commands the device is known to tolerate are
    /// **allowed by an explicit table** and every other frame is refused at
    /// FCPTransport::SubmitCommand — an allowlist, not a blocklist. The table
    /// and its per-row evidence live in Protocols/AVC/AVCCommandFilter.hpp;
    /// the hazard itself is AVC_DEVICE_HAZARDS.md H1.
    BeBoBFilteredCommandSet,

    /// Echo Fireworks: HWINFO over EFC before the first stream.
    FireworksEfc,

    /// MOTU: clock/format registers, read directly. No FCP is ever sent.
    MotuRegister,

    /// Alias for the last real member; see the note on ProfileBuilderId.
    kLastValid = MotuRegister,
};

/// Device preparation that must run before an identity can become anything
/// else. Only the BeBoB bootloader persona carries one; see
/// documentation/MAUDIO_BOOTLOADER_CUE_DESIGN.md.
enum class BootloaderCuePolicy : uint8_t {
    None = 0,
    /// Write the frozen BeBoB "start application firmware" cue. This is the
    /// only bootloader command the driver can express.
    BeBoBStartFirmware,
};

enum class ProfileBuilderId : uint16_t {
    None = 0,
    GenericAvc,
    FocusriteSPro14,
    FocusriteSPro24,
    FocusriteSPro24Dsp,
    WeissInt202,
    WeissInt203,
    ApogeeDuet,
    TerraTecPhase88,
    AlesisMultiMix,
    MidasVeniceF32,
    PreSonusStudioLive1602,
    // Two ids for one protocol class: the class is shared, and the id is how
    // the family provider tells the two personas apart when picking a rate list.
    MAudioFireWire1814,
    MAudioProjectMix,
    FocusriteLiquidS56,
    // Builders that exist on this branch but not on `midi`; appended so the
    // enum values `midi` already assigned do not shift under the merge.
    FocusriteSPro40,
    PreSonusStudioLive2442,
    Motu828mk2,
    MotuUltralite,
    MackieOnyxIOxfw,
    MackieOnyx400F,
    GenericBeBoB,

    // Alias for the last real member. Range checks over this enum live in two
    // places — the catalog validator and the endpoint-profile wire validator —
    // and a bound left pointing at an older member does not fail loudly: the
    // device installs, publishes a nub, and then Start() rejects the profile
    // with a bare kIOReturnBadArgument. Extend the enum above this line and the
    // bounds follow.
    kLastValid = GenericBeBoB,
};

/// AMDTP cadence a device must be driven at regardless of what it reports.
/// Unspecified means "believe the probe", which is what an unlisted device gets.
enum class ForcedStreamMode : uint8_t {
    Unspecified = 0,
    Blocking,
    NonBlocking,
};

/// The shape of a device's stream start/stop choreography. These are wire-
/// visible orderings cross-validated against the reference stacks, not
/// preferences -- getting one wrong is a stream that never establishes.
enum class StreamStartShape : uint8_t {
    /// DICE and anything else with no stated opinion: the coordinator's
    /// default, including its pre-stream clock-lock gate.
    Default = 0,
    /// Apogee Duet: host IR -> CMP oPCR -> host IT -> CMP iPCR, and the
    /// mirrored teardown. Preserves the ordering AVCAudioBackend had.
    ApogeeInterleaved,
    /// Linux's CMP choreography: reserve both resources, establish remote iPCR
    /// then oPCR, then start the domain receive-before-transmit. Shared by
    /// BeBoB, the Oxford-run Onyx-i and the Fireworks-run Onyx 400F, all of
    /// which are SYT-unaware and so must not run the pre-stream lock gate.
    /// bebob_stream.c:525-590,593-674; fireworks_stream.c.
    CmpReceiveThenTransmit,
    /// Weiss INT202/203: host transmit first after GLOBAL_ENABLE so its AM824
    /// packets can establish the device receive-clock path. Source lock is
    /// inspected post-start, not used as an admission gate.
    TransmitFirst,
};

/// Wire-level facts about a device that no probe reports, so the driver has to
/// be told them. Every field defaults to "nothing special", which is exactly
/// how an unlisted device behaves -- a definition that states none of these is
/// indistinguishable from having no definition at all, for these purposes.
struct DeviceStreamTraits final {
    ForcedStreamMode forcedStreamMode{ForcedStreamMode::Unspecified};
    StreamStartShape startShape{StreamStartShape::Default};

    /// The device advertises more capture streams than it has. FFADO clamps
    /// the affected Alesis models for the same reason:
    /// libffado-2.5.0/src/dice/dice_avdevice.cpp:1682-1695.
    bool clampCaptureStreamsToOne{false};

    /// CMP owns the isochronous channel: the device has no fixed one, IRM
    /// picks it and the PCR commits it back. True for every CMP-driven family
    /// (BeBoB, Oxford, Fireworks); false for DICE, which programs a channel
    /// into its own registers, and for MOTU.
    bool cmpChoosesIsoChannel{false};

    /// The capture-side CIP dbs field is untrusted and the configured slot
    /// count is the authority. Loud/Mackie (snd-oxfw oxfw.c:189-196,
    /// amdtp-stream.c:766-769) and Fireworks, which gives dbc its own meaning
    /// and whose firmware 4.6.0 stamps a wrong dbs above 88.2 kHz.
    bool captureTrustConfiguredStride{false};

    /// Host<->device PCM is raw 24-in-32 rather than AM824 *when the runtime
    /// geometry matches* (8 PCM channels in 9 slots). Conditional on purpose:
    /// the Saffire Pro 24 DSP switches wire format with its configuration, so
    /// this cannot be a static property of the identity.
    bool rawPcm24In32WhenEightInNineSlots{false};
};

enum class SupportDisposition : uint8_t {
    Supported = 0,
    GenericFallback,
    RecognizedUnsupported,
    Quarantined,
};

enum class GuidReliability : uint8_t {
    Unspecified = 0,
    ReliableWhenUnique,
    KnownNonUnique,
};

enum class PersistentKeyRecipeId : uint8_t {
    None = 0,
    ReliableObservedEui64,
};

// Safe, read-only family probe facts may disambiguate marketing variants that
// Config ROM cannot distinguish. Every populated field is an AND constraint.
// This deliberately describes stream evidence without depending on Audio
// runtime types, keeping the catalog declarative.
struct SafeProbeConstraint final {
    std::optional<uint32_t> hostInputPcmChannels;
    std::optional<uint32_t> hostOutputPcmChannels;
    std::optional<uint32_t> deviceToHostSlots;
    std::optional<uint32_t> hostToDeviceSlots;
    std::optional<uint32_t> deviceToHostStreamCount;
    std::optional<uint32_t> hostToDeviceStreamCount;
};

enum class CatalogResolutionError : uint8_t {
    NoMatch = 0,
    HazardousIdentity,
    AmbiguousIdentity,
    InvalidUnit,
};

struct AudioDeviceDefinition final {
    DeviceDefinitionId id{DeviceDefinitionId::Unknown};
    uint32_t variantId{0};
    uint32_t equivalenceClassId{0};
    std::array<IdentityMatchClause, 2> clauses{};
    uint8_t clauseCount{0};
    AudioFamilyProviderId family{AudioFamilyProviderId::None};
    ProbePolicyId probePolicy{ProbePolicyId::None};
    ProfileBuilderId profileBuilder{ProfileBuilderId::None};
    ProfileBuilderId commonEquivalenceProfileBuilder{ProfileBuilderId::None};
    SupportDisposition support{SupportDisposition::RecognizedUnsupported};
    GuidReliability guidReliability{GuidReliability::ReliableWhenUnique};
    PersistentKeyRecipeId persistentKeyRecipe{
        PersistentKeyRecipeId::ReliableObservedEui64};
    SafeProbeConstraint probeConstraint{};
    DeviceStreamTraits streamTraits{};
    BootloaderCuePolicy bootloaderCue{BootloaderCuePolicy::None};
    const char* vendorName{nullptr};
    const char* modelName{nullptr};
};

struct AudioSafetyRule final {
    std::array<IdentityMatchClause, 6> clauses{};
    uint8_t clauseCount{0};
    Discovery::QuarantineReason reason{Discovery::QuarantineReason::HazardousNoProbe};
    const char* name{nullptr};
};

struct MatchProvenance final {
    DeviceDefinitionId definitionId{DeviceDefinitionId::Unknown};
    uint8_t clauseIndex{0};
};

struct CandidateEndpointPlan final {
    DeviceDefinitionId definitionId{DeviceDefinitionId::Unknown};
    uint32_t variantId{0};
    ProfileBuilderId profileBuilder{ProfileBuilderId::None};
    SafeProbeConstraint probeConstraint{};
    std::string vendorName;
    std::string modelName;
};

struct StaticAudioEndpointPlan final {
    Discovery::UnitInstanceId unit{};
    std::optional<uint32_t> exactVariantId;
    AudioFamilyProviderId family{AudioFamilyProviderId::None};
    ProbePolicyId probePolicy{ProbePolicyId::None};
    SupportDisposition support{SupportDisposition::RecognizedUnsupported};
    GuidReliability guidReliability{GuidReliability::Unspecified};
    PersistentKeyRecipeId persistentKeyRecipe{PersistentKeyRecipeId::None};
    uint32_t equivalenceClassId{0};
    std::vector<DeviceDefinitionId> candidates;
    std::vector<CandidateEndpointPlan> candidatePlans;
    std::vector<MatchProvenance> provenance;
    ProfileBuilderId profileBuilder{ProfileBuilderId::None};
    ProfileBuilderId commonEquivalenceProfileBuilder{ProfileBuilderId::None};
    DeviceStreamTraits streamTraits{};
    BootloaderCuePolicy bootloaderCue{BootloaderCuePolicy::None};
    std::string vendorName;
    std::string modelName;
};

struct CatalogValidationIssue final {
    DeviceDefinitionId first{DeviceDefinitionId::Unknown};
    DeviceDefinitionId second{DeviceDefinitionId::Unknown};
    const char* reason{nullptr};
};

class AudioDeviceCatalog final {
public:
    [[nodiscard]] static std::expected<StaticAudioEndpointPlan, CatalogResolutionError>
    Resolve(const Discovery::DeviceRecord& device,
            const Discovery::UnitIdentityEvidence& unit) noexcept;

    // Pure injection point used by host fixtures and future family-local
    // catalogs. Production callers use Resolve().
    [[nodiscard]] static std::expected<StaticAudioEndpointPlan, CatalogResolutionError>
    ResolveWithDefinitions(const Discovery::DeviceRecord& device,
                           const Discovery::UnitIdentityEvidence& unit,
                           std::span<const AudioDeviceDefinition> definitions,
                           std::span<const AudioSafetyRule> safetyRules,
                           bool allowGenericAvcFallback) noexcept;

    [[nodiscard]] static std::optional<const AudioSafetyRule*>
    MatchSafetyRule(const Discovery::DeviceIdentityEvidence& device,
                    const Discovery::UnitIdentityEvidence& unit) noexcept;

    [[nodiscard]] static std::optional<const AudioSafetyRule*>
    MatchAnySafetyRule(const Discovery::DeviceIdentityEvidence& device) noexcept;

    /// Which AV/C command shapes this identity may be sent, decided from Config
    /// ROM alone and before any transaction. Device-level like
    /// MatchAnySafetyRule: it tries every unit and returns the first definition
    /// that constrains the device. Unmatched identities are Unrestricted, which
    /// preserves today's behaviour for every device without a definition.
    [[nodiscard]] static Discovery::AvcCommandFilterId
    CommandFilterFor(const Discovery::DeviceIdentityEvidence& device) noexcept;

    /// The stream traits this identity carries, decided from Config ROM alone.
    /// Device-level like CommandFilterFor: it tries every unit and returns the
    /// first definition that matches. An unmatched identity gets the defaults,
    /// which is today's behaviour for every device without a definition.
    [[nodiscard]] static DeviceStreamTraits
    StreamTraitsFor(const Discovery::DeviceIdentityEvidence& device) noexcept;

    /// The profile builder this identity resolves to, or None. Device-level
    /// like the two above, for callers that hold Config-ROM evidence but not a
    /// registry record -- the AV/C discovery path, which must tell the nub what
    /// the device is at publication time.
    [[nodiscard]] static ProfileBuilderId
    ProfileBuilderFor(const Discovery::DeviceIdentityEvidence& device) noexcept;

    [[nodiscard]] static std::span<const AudioDeviceDefinition> Definitions() noexcept;
    [[nodiscard]] static std::span<const AudioSafetyRule> SafetyRules() noexcept;
    [[nodiscard]] static std::vector<CatalogValidationIssue> Validate() noexcept;
    [[nodiscard]] static std::vector<CatalogValidationIssue>
    ValidateDefinitions(std::span<const AudioDeviceDefinition> definitions) noexcept;
};

} // namespace ASFW::DeviceProfiles::Audio
