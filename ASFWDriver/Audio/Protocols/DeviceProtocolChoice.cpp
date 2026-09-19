// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "DeviceProtocolChoice.hpp"

namespace ASFW::Audio {

std::optional<DeviceProtocolChoice>
ChooseDeviceProtocol(const Discovery::DeviceRecord& record) noexcept {
    using DeviceProfiles::Audio::AudioDeviceCatalog;
    using DeviceProfiles::Audio::DeviceDefinitionId;
    using DeviceProfiles::Audio::ProfileBuilderId;

    // A device has units, plural. Walk them in ROM order and take the first
    // that resolves to something we stream; a non-audio sibling unit (SBP-2,
    // for instance) simply does not resolve, and a recognised-but-unplayable
    // one names no builder.
    for (const auto& unit : record.identity.units) {
        const auto plan = AudioDeviceCatalog::Resolve(record, unit);
        if (!plan.has_value() || plan->profileBuilder == ProfileBuilderId::None) {
            continue;
        }
        return DeviceProtocolChoice{
            .definition = plan->candidates.empty() ? DeviceDefinitionId::Unknown
                                                   : plan->candidates.front(),
            .builder = plan->profileBuilder,
            .unitVersion = unit.version.value_or(0U),
            .unitDirectoryOffset = unit.unitDirectoryOffset,
        };
    }
    return std::nullopt;
}

AudioBackendKind ChooseAudioBackend(const Discovery::DeviceRecord& record) noexcept {
    using DeviceProfiles::Audio::AudioDeviceCatalog;
    using DeviceProfiles::Audio::AudioFamilyProviderId;
    using DeviceProfiles::Audio::SupportDisposition;

    for (const auto& unit : record.identity.units) {
        const auto plan = AudioDeviceCatalog::Resolve(record, unit);
        if (!plan.has_value() || plan->support != SupportDisposition::Supported) {
            continue;
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
            case AudioFamilyProviderId::None:
                break;
        }
    }
    return AudioBackendKind::Avc;
}

} // namespace ASFW::Audio
