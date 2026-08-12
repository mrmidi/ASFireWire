// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Host-only example of adding a device that uses an existing audio family.
// Production catalogs and provider registration cannot see these declarations.

#pragma once

#if defined(ASFW_HOST_TEST)

#include "../../Audio/Devices/AudioFamilyProvider.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"

#include <array>
#include <mutex>

namespace ASFW::Testing::AudioTemplate {

inline constexpr uint32_t kVendorId = 0x00F1A0;
inline constexpr uint32_t kModelId = 0x000123;
inline constexpr uint32_t kUnsafeModelId = 0x0001FE;
inline constexpr uint32_t kAvcSpecifierId = 0x00A02D;
inline constexpr uint32_t kEquivalenceClassId = 0xF1A0;
inline constexpr auto kDefinitionId =
    static_cast<DeviceProfiles::Audio::DeviceDefinitionId>(0x00F1A001);
inline constexpr auto kEquivalentDefinitionAId =
    static_cast<DeviceProfiles::Audio::DeviceDefinitionId>(0x00F1A002);
inline constexpr auto kEquivalentDefinitionBId =
    static_cast<DeviceProfiles::Audio::DeviceDefinitionId>(0x00F1A003);

inline constexpr DeviceProfiles::Audio::AudioDeviceDefinition kDefinition{
    .id = kDefinitionId,
    .variantId = 1,
    .clauses = {DeviceProfiles::IdentityMatchClause{
        .rootVendorId = DeviceProfiles::MaskedValue32{kVendorId},
        .rootModelId = DeviceProfiles::MaskedValue32{kModelId},
        .unitSpecifierId = DeviceProfiles::MaskedValue32{kAvcSpecifierId},
    }},
    .clauseCount = 1,
    .family = DeviceProfiles::Audio::AudioFamilyProviderId::GenericAvc,
    .probePolicy = DeviceProfiles::Audio::ProbePolicyId::GenericAvc,
    .profileBuilder = DeviceProfiles::Audio::ProfileBuilderId::GenericAvc,
    .support = DeviceProfiles::Audio::SupportDisposition::Supported,
    .guidReliability = DeviceProfiles::Audio::GuidReliability::ReliableWhenUnique,
    .vendorName = "ASFW Fixture Vendor",
    .modelName = "Existing-Family Fixture",
};

// Safety rules are evaluated before definitions and generic AV/C fallback.
// This fixture demonstrates how a protocol-violating identity can remain
// visible for Config-ROM diagnostics while producing no family transaction.
inline constexpr DeviceProfiles::Audio::AudioSafetyRule kSafetyRule{
    .clauses = {DeviceProfiles::IdentityMatchClause{
        .rootVendorId = DeviceProfiles::MaskedValue32{kVendorId},
        .rootModelId = DeviceProfiles::MaskedValue32{kUnsafeModelId},
    }},
    .clauseCount = 1,
    .reason = ASFW::Discovery::QuarantineReason::HazardousNoProbe,
    .name = "Unsafe existing-family fixture",
};
inline constexpr std::array kSafetyRules{kSafetyRule};

inline constexpr DeviceProfiles::IdentityMatchClause kEquivalentIdentityClause{
    .rootVendorId = DeviceProfiles::MaskedValue32{kVendorId},
    .unitSpecifierId = DeviceProfiles::MaskedValue32{kAvcSpecifierId},
};

// Optional equivalence candidates share one safe probe and one validated common
// builder. Typed stream facts select an exact variant when possible.
inline constexpr DeviceProfiles::Audio::AudioDeviceDefinition kEquivalentDefinitionA{
    .id = kEquivalentDefinitionAId,
    .variantId = 2,
    .equivalenceClassId = kEquivalenceClassId,
    .clauses = {kEquivalentIdentityClause},
    .clauseCount = 1,
    .family = DeviceProfiles::Audio::AudioFamilyProviderId::GenericAvc,
    .probePolicy = DeviceProfiles::Audio::ProbePolicyId::GenericAvc,
    .profileBuilder = DeviceProfiles::Audio::ProfileBuilderId::GenericAvc,
    .commonEquivalenceProfileBuilder =
        DeviceProfiles::Audio::ProfileBuilderId::GenericAvc,
    .support = DeviceProfiles::Audio::SupportDisposition::Supported,
    .probeConstraint = {.hostInputPcmChannels = 2},
    .vendorName = "ASFW Fixture Vendor",
    .modelName = "Existing-Family Fixture 2-in",
};

inline constexpr DeviceProfiles::Audio::AudioDeviceDefinition kEquivalentDefinitionB{
    .id = kEquivalentDefinitionBId,
    .variantId = 4,
    .equivalenceClassId = kEquivalenceClassId,
    .clauses = {kEquivalentIdentityClause},
    .clauseCount = 1,
    .family = DeviceProfiles::Audio::AudioFamilyProviderId::GenericAvc,
    .probePolicy = DeviceProfiles::Audio::ProbePolicyId::GenericAvc,
    .profileBuilder = DeviceProfiles::Audio::ProfileBuilderId::GenericAvc,
    .commonEquivalenceProfileBuilder =
        DeviceProfiles::Audio::ProfileBuilderId::GenericAvc,
    .support = DeviceProfiles::Audio::SupportDisposition::Supported,
    .probeConstraint = {.hostInputPcmChannels = 4},
    .vendorName = "ASFW Fixture Vendor",
    .modelName = "Existing-Family Fixture 4-in",
};
inline constexpr std::array kEquivalentDefinitions{
    kEquivalentDefinitionA,
    kEquivalentDefinitionB,
};

class FixtureFacet final : public ASFW::Audio::Devices::IAudioDeviceFacet {
public:
    [[nodiscard]] ASFW::Audio::Devices::FacetDescriptor Descriptor() const noexcept override {
        return {ASFW::Audio::Devices::FacetKind::VendorControl, 0x46585452};
    }
};

struct ProbeHarness final {
    using Completion = ASFW::Audio::Devices::IAudioDeviceAdapter::ProbeCompletion;

    void Complete(std::expected<ASFW::Audio::Devices::FamilyProbeFacts,
                                ASFW::Audio::Devices::ProbeError> result) {
        Completion callback;
        {
            std::lock_guard guard(lock);
            callback = std::move(pending);
        }
        if (callback) callback(std::move(result));
    }

    void CompleteSuccess(uint32_t sampleRateHz = 48000) {
        ASFW::Audio::AudioStreamRuntimeCaps caps{};
        caps.sampleRateHz = sampleRateHz;
        caps.hostInputPcmChannels = 2;
        caps.hostOutputPcmChannels = 2;
        caps.deviceToHostAm824Slots = 2;
        caps.hostToDeviceAm824Slots = 2;
        caps.deviceToHostStreamCount = 1;
        caps.hostToDeviceStreamCount = 1;
        caps.deviceToHostStreams[0] = {1, 2, 2, 0};
        caps.hostToDeviceStreams[0] = {0, 2, 2, 0};
        Complete(ASFW::Audio::Devices::GenericAvcProbeFacts{
            .streams = caps,
            .supportedRates = {sampleRateHz},
            .hasAudioSubunit = true,
            .hasMusicSubunit = true,
        });
    }

    std::mutex lock;
    Completion pending;
    uint32_t acceptedProbeCount{0};
    uint32_t cancellationCount{0};
    uint32_t shutdownCount{0};
};

class Adapter final : public ASFW::Audio::Devices::IAudioDeviceAdapter {
public:
    explicit Adapter(std::shared_ptr<ProbeHarness> harness)
        : harness_(std::move(harness)) {}

    [[nodiscard]] bool Probe(ProbeCompletion completion) noexcept override {
        if (!completion) return false;
        std::lock_guard guard(harness_->lock);
        if (harness_->pending) return false;
        ++harness_->acceptedProbeCount;
        harness_->pending = std::move(completion);
        return true;
    }

    void CancelProbe() noexcept override {
        ProbeCompletion callback;
        {
            std::lock_guard guard(harness_->lock);
            if (!harness_->pending) return;
            ++harness_->cancellationCount;
            callback = std::move(harness_->pending);
        }
        callback(std::unexpected(ASFW::Audio::Devices::ProbeError::Cancelled));
    }

    [[nodiscard]] IOReturn Shutdown() noexcept override {
        CancelProbe();
        std::lock_guard guard(harness_->lock);
        ++harness_->shutdownCount;
        return kIOReturnSuccess;
    }

    [[nodiscard]] ASFW::Audio::IDuplexDeviceControl* DuplexControl() noexcept override {
        return nullptr;
    }

    [[nodiscard]] std::shared_ptr<ASFW::Audio::IDeviceProtocol>
    ProtocolHold() noexcept override {
        return nullptr;
    }

    [[nodiscard]] std::vector<std::shared_ptr<ASFW::Audio::Devices::IAudioDeviceFacet>>
    Facets() const override {
        return {std::make_shared<FixtureFacet>()};
    }

private:
    std::shared_ptr<ProbeHarness> harness_;
};

class Provider final : public ASFW::Audio::Devices::IAudioFamilyProvider {
public:
    explicit Provider(std::shared_ptr<ProbeHarness> harness)
        : harness_(std::move(harness)) {}

    [[nodiscard]] DeviceProfiles::Audio::AudioFamilyProviderId Id() const noexcept override {
        return DeviceProfiles::Audio::AudioFamilyProviderId::GenericAvc;
    }

    [[nodiscard]] std::unique_ptr<ASFW::Audio::Devices::IAudioDeviceAdapter>
    CreateAdapter(const ASFW::Audio::Devices::AudioFamilyProviderContext& context) noexcept override {
        if (context.staticPlan.family != Id()) return nullptr;
        return std::make_unique<Adapter>(harness_);
    }

private:
    std::shared_ptr<ProbeHarness> harness_;
};

} // namespace ASFW::Testing::AudioTemplate

#endif // ASFW_HOST_TEST
