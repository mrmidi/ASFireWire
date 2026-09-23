// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "DeviceProtocolChoice.hpp"

namespace ASFW::Audio {

std::optional<DeviceProtocolChoice>
ChooseDeviceProtocol(const Discovery::DeviceRecord& record) noexcept {
    using DeviceProfiles::Audio::AudioDeviceCatalog;
    using DeviceProfiles::Audio::DeviceDefinitionId;
    using DeviceProfiles::Audio::ProfileBuilderId;

    const auto plan = AudioDeviceCatalog::Resolve(record);
    if (!plan.has_value() || plan->profileBuilder == ProfileBuilderId::None) {
        return std::nullopt;
    }
    return DeviceProtocolChoice{
        .definition = plan->candidates.empty() ? DeviceDefinitionId::Unknown
                                               : plan->candidates.front(),
        .builder = plan->profileBuilder,
        .implementation = plan->protocolImplementation,
        .unitVersion = plan->unitVersion,
        .unitDirectoryOffset = plan->unit.unitDirectoryOffset,
    };
}

std::optional<AudioBackendKind>
ChooseAudioBackend(const Discovery::DeviceRecord& record) noexcept {
    using DeviceProfiles::Audio::AudioDeviceCatalog;
    using DeviceProfiles::Audio::AudioFamilyProviderId;
    using DeviceProfiles::Audio::SupportDisposition;

    const auto plan = AudioDeviceCatalog::Resolve(record);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (plan->support != SupportDisposition::Supported &&
        plan->support != SupportDisposition::GenericFallback) {
        return std::nullopt;
    }
    switch (plan->family) {
        case AudioFamilyProviderId::DICE:
            return AudioBackendKind::Dice;
        case AudioFamilyProviderId::MotuRegister:
            return AudioBackendKind::MotuRegister;
        case AudioFamilyProviderId::GenericAvc:
        case AudioFamilyProviderId::BeBoB:
        case AudioFamilyProviderId::OXFW:
        case AudioFamilyProviderId::Fireworks:
            return AudioBackendKind::Avc;
        case AudioFamilyProviderId::None:
            return std::nullopt;
    }
    return std::nullopt;
}

} // namespace ASFW::Audio
