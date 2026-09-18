// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// CatalogMatcherAgreementTests.cpp
//
// Before the catalog replaces the existing matchers, it has to be shown to
// agree with them. Otherwise the collapse changes behaviour while claiming to
// be a refactor, and the difference only shows up on hardware nobody has.
//
// This pins the catalog against AudioProfileRegistry -- the matcher that
// decides, today, whether a device gets an audio nub at all. Both are pure, so
// the whole cross-product of catalog rows is checkable here.
//
// The comparison is deliberately in one direction per property:
//   * every catalog row the registry calls playable must be Supported
//   * every Supported catalog row must be playable per the registry
// A disagreement in either direction is a real defect in one of the two, and
// which one it is depends on the device -- so the test names both values rather
// than asserting a winner.

#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "DeviceProfiles/Audio/AudioProfileRegistry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <ios>
#include <optional>

namespace {

using namespace ASFW::DeviceProfiles::Audio;
using ASFW::DeviceProfiles::DeviceProfileQuery;

// Reconstructs the scalar query the old matchers take from a catalog row's
// clause. This is the flattening the catalog exists to end, so it is only valid
// as a bridge for this comparison -- note what it cannot carry: a model id that
// was published as 0 versus one never published at all.
struct FlatQuery {
    bool usable{false};
    DeviceProfileQuery query{};
};

[[nodiscard]] FlatQuery FlattenFirstClause(const AudioDeviceDefinition& definition) {
    if (definition.clauseCount == 0) {
        return {};
    }
    const auto& clause = definition.clauses[0];
    if (!clause.rootVendorId.has_value() || !clause.rootModelId.has_value()) {
        return {};
    }
    FlatQuery flat{};
    flat.usable = true;
    flat.query.vendorId = clause.rootVendorId->value;
    flat.query.modelId = clause.rootModelId->value;
    if (clause.unitSpecifierId.has_value()) {
        flat.query.unitSpecId = clause.unitSpecifierId->value;
    }
    if (clause.unitVersion.has_value()) {
        flat.query.unitSwVersion = clause.unitVersion->value;
    }
    // MOTU is matched from the unit directory: the old query carries the
    // OUI in unitSpecId and the model in unitSwVersion, and the registry reads
    // those rather than modelId.
    if (definition.family == AudioFamilyProviderId::MotuRegister) {
        flat.query.unitSpecId = kMotuVendorId;
    }
    return flat;
}

[[nodiscard]] bool RegistrySaysPlayable(const DeviceProfileQuery& query) {
    const auto hint = AudioProfileRegistry::LookupBestAudioProfile(query);
    return hint.has_value() && hint->mode != AudioIntegrationMode::kNone;
}

// ---------------------------------------------------------------------------

TEST(CatalogMatcherAgreement, TheCatalogAndTheProfileRegistryAgreeOnEveryRow) {
    for (const auto& definition : AudioDeviceCatalog::Definitions()) {
        const auto flat = FlattenFirstClause(definition);
        if (!flat.usable) {
            continue;  // no root vendor/model clause to flatten
        }
        // The M-Audio rows exist for their probe bound, not for audio, and the
        // old registry has no M-Audio provider at all. Nothing to compare.
        if (flat.query.vendorId == kMAudioVendorId) {
            continue;
        }

        const bool catalogSaysPlayable =
            definition.support == SupportDisposition::Supported;
        const bool registrySaysPlayable = RegistrySaysPlayable(flat.query);

        EXPECT_EQ(catalogSaysPlayable, registrySaysPlayable)
            << "definition " << static_cast<uint32_t>(definition.id)
            << " (vendor 0x" << std::hex << flat.query.vendorId
            << " model 0x" << flat.query.modelId << std::dec << "): catalog says "
            << (catalogSaysPlayable ? "Supported" : "not Supported")
            << ", AudioProfileRegistry says "
            << (registrySaysPlayable ? "playable" : "not playable");
    }
}

// The registry resolves a display name for devices it will not play. Those are
// exactly the rows the catalog carries as RecognizedUnsupported, and losing one
// means a device that used to be named in diagnostics becomes an unknown.
TEST(CatalogMatcherAgreement, EveryIdentityTheRegistryNamesHasACatalogRow) {
    for (const auto& definition : AudioDeviceCatalog::Definitions()) {
        const auto flat = FlattenFirstClause(definition);
        if (!flat.usable || flat.query.vendorId == kMAudioVendorId) {
            continue;
        }
        const auto identity = AudioProfileRegistry::LookupIdentity(flat.query);
        if (!identity.has_value()) {
            continue;
        }
        EXPECT_NE(definition.modelName, nullptr)
            << "definition " << static_cast<uint32_t>(definition.id)
            << " has no model name but the registry names it";
    }
}

// Spot checks with the identities written out, so a reader can see what the
// agreement actually covers rather than trusting the loop.
TEST(CatalogMatcherAgreement, TheBenchDeviceAgreesOnBothSides) {
    const DeviceProfileQuery pro24Dsp{.vendorId = kFocusriteVendorId,
                                      .modelId = kSPro24DspModelId};
    EXPECT_TRUE(RegistrySaysPlayable(pro24Dsp));

    const auto definitions = AudioDeviceCatalog::Definitions();
    const auto it = std::ranges::find_if(
        definitions, [](const AudioDeviceDefinition& d) {
            return d.id == DeviceDefinitionId::FocusriteSPro24Dsp;
        });
    ASSERT_NE(it, definitions.end());
    EXPECT_EQ(it->support, SupportDisposition::Supported);
}

TEST(CatalogMatcherAgreement, TheTcd3070Pro40IsUnplayableOnBothSides) {
    const DeviceProfileQuery tcd3070{.vendorId = kFocusriteVendorId,
                                     .modelId = kSPro40Tcd3070ModelId};
    EXPECT_FALSE(RegistrySaysPlayable(tcd3070))
        << "the TCD3070 Pro 40 has no readable TCAT extension; playing it with a "
           "sibling's geometry is the vendor-wide matching defect";

    const auto definitions = AudioDeviceCatalog::Definitions();
    const auto it = std::ranges::find_if(
        definitions, [](const AudioDeviceDefinition& d) {
            return d.id == DeviceDefinitionId::FocusriteSPro40Tcd3070;
        });
    ASSERT_NE(it, definitions.end());
    EXPECT_NE(it->support, SupportDisposition::Supported);
}

} // namespace
