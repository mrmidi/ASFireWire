// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "ResolvedAudioEndpointProfile.hpp"
#include "../../Discovery/DeviceRouteToken.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../Protocols/AudioTypes.hpp"

#include <DriverKit/IOReturn.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <variant>
#include <vector>

namespace ASFW::Audio {
class IDuplexDeviceControl;
class IDeviceProtocol;
}

namespace ASFW::Audio::Devices {

struct GenericAvcProbeFacts final {
    AudioStreamRuntimeCaps streams{};
    std::vector<uint32_t> supportedRates;
    bool hasAudioSubunit{false};
    bool hasMusicSubunit{false};
};

struct BeBoBProbeFacts final {
    AudioStreamRuntimeCaps streams{};
    std::vector<uint32_t> supportedRates;
    uint32_t operationPolicyId{0};
};

struct DiceProbeFacts final {
    AudioStreamRuntimeCaps streams{};
    std::vector<uint32_t> supportedRates;
    uint32_t clockCapabilities{0};
    std::vector<std::string> inputLabels;
    std::vector<std::string> outputLabels;
};

struct OxfwProbeFacts final {
    AudioStreamRuntimeCaps streams{};
    std::vector<uint32_t> supportedRates;
    uint32_t operationPolicyId{0};
};

// Host-only fixture fact. It is impossible to register this provider in a
// production ProviderRegistry.
struct FakeProbeFacts final {
    AudioStreamRuntimeCaps streams{};
    std::vector<uint32_t> supportedRates;
    uint32_t fixtureValue{0};
};

using FamilyProbeFacts = std::variant<std::monostate, GenericAvcProbeFacts,
                                      BeBoBProbeFacts, DiceProbeFacts,
                                      OxfwProbeFacts, FakeProbeFacts>;

enum class ProbeError : uint8_t {
    Cancelled,
    StaleRoute,
    Unsupported,
    Transport,
    InvalidEvidence,
};

class IDeviceRouteAccessor {
public:
    virtual ~IDeviceRouteAccessor() = default;
    [[nodiscard]] virtual std::optional<Discovery::DeviceRouteToken>
    CurrentRoute(Discovery::DeviceInstanceId instanceId) const noexcept = 0;
    [[nodiscard]] virtual bool IsCurrent(
        const Discovery::DeviceRouteToken& route) const noexcept = 0;
};

class IAudioDeviceFacet {
public:
    virtual ~IAudioDeviceFacet() = default;
    [[nodiscard]] virtual FacetDescriptor Descriptor() const noexcept = 0;
};

class IAudioDeviceAdapter {
public:
    using ProbeCompletion =
        std::function<void(std::expected<FamilyProbeFacts, ProbeError>)>;

    virtual ~IAudioDeviceAdapter() = default;

    // Exactly one completion for every accepted call, including cancellation.
    [[nodiscard]] virtual bool Probe(ProbeCompletion completion) noexcept = 0;
    virtual void CancelProbe() noexcept = 0;
    [[nodiscard]] virtual IOReturn Shutdown() noexcept = 0;
    [[nodiscard]] virtual IDuplexDeviceControl* DuplexControl() noexcept = 0;
    [[nodiscard]] virtual std::shared_ptr<IDeviceProtocol> ProtocolHold() noexcept = 0;
    [[nodiscard]] virtual std::vector<std::shared_ptr<IAudioDeviceFacet>> Facets() const = 0;
};

struct AudioFamilyProviderContext final {
    Discovery::UnitInstanceId unit{};
    DeviceProfiles::Audio::StaticAudioEndpointPlan staticPlan{};
    IDeviceRouteAccessor& routes;
};

class IAudioFamilyProvider {
public:
    virtual ~IAudioFamilyProvider() = default;
    [[nodiscard]] virtual DeviceProfiles::Audio::AudioFamilyProviderId Id() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<IAudioDeviceAdapter>
    CreateAdapter(const AudioFamilyProviderContext& context) noexcept = 0;
};

} // namespace ASFW::Audio::Devices
