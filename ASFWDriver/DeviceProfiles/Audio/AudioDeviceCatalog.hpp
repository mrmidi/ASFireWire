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
    MidasVeniceF32,
    PreSonusStudioLive1602,
    PreSonusStudioLive1642,
    PreSonusStudioLive2442,
    PreSonusStudioLive3242,
    MAudioFireWire1814Bootloader,
};

enum class AudioFamilyProviderId : uint8_t {
    None = 0,
    GenericAvc,
    BeBoB,
    DICE,
    OXFW,
};

enum class ProbePolicyId : uint8_t {
    None = 0,
    NoAutomaticTraffic,
    GenericAvc,
    BeBoBPlug0,
    DiceTcat,
    OxfwAvc,
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

    [[nodiscard]] static std::span<const AudioDeviceDefinition> Definitions() noexcept;
    [[nodiscard]] static std::span<const AudioSafetyRule> SafetyRules() noexcept;
    [[nodiscard]] static std::vector<CatalogValidationIssue> Validate() noexcept;
    [[nodiscard]] static std::vector<CatalogValidationIssue>
    ValidateDefinitions(std::span<const AudioDeviceDefinition> definitions) noexcept;
};

} // namespace ASFW::DeviceProfiles::Audio
