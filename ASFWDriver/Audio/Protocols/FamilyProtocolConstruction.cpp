// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FamilyProtocolConstruction.cpp - Family-keyed device protocol construction

#include "FamilyProtocolConstruction.hpp"

#include "DICE/Focusrite/SPro24DspProtocol.hpp"
#include "DICE/TCAT/DICETcatProtocol.hpp"
#include "Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "Oxford/Mackie/MackieOnyxProtocol.hpp"
#include "Fireworks/FireworksProtocol.hpp"
#include "BeBoB/Phase88Protocol.hpp"
#include "BeBoB/GenericBeBoBProtocol.hpp"
#include "MOTU/MotuV2Protocol.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"

namespace ASFW::Audio {

static_assert(
    static_cast<uint8_t>(DeviceProfiles::Audio::AudioFamilyProviderId::kLastValid) ==
    static_cast<uint8_t>(DeviceProfiles::Audio::AudioFamilyProviderId::MotuRegister),
    "AudioFamilyProviderId member added without updating family protocol construction");

std::unique_ptr<IDeviceProtocol> CreateFamilyDeviceProtocol(
    const Discovery::DeviceRecord& record,
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    Discovery::DeviceRegistry& routeRegistry,
    const Discovery::DeviceRouteToken& route,
    IRM::IRMClient* irmClient,
    CMP::CMPClient* cmpClient,
    Scheduling::ITimerScheduler* timerScheduler
) {
    if (!route) {
        return nullptr;
    }
    const uint16_t nodeId = route.nodeId;

    const auto plan = DeviceProfiles::Audio::AudioDeviceCatalog::Resolve(record);
    if (!plan.has_value() ||
        plan->profileBuilder == DeviceProfiles::Audio::ProfileBuilderId::None) {
        return nullptr;
    }

    using DeviceProfiles::Audio::AudioFamilyProviderId;
    using DeviceProfiles::Audio::ProfileBuilderId;

    switch (plan->family) {
        case AudioFamilyProviderId::DICE: {
            if (plan->profileBuilder == ProfileBuilderId::FocusriteSPro24Dsp) {
                ASFW_LOG(DICE,
                         "Creating SPro24DspProtocol node=0x%04x unitOffset=%u",
                         nodeId, plan->unit.unitDirectoryOffset);
                return std::make_unique<DICE::Focusrite::SPro24DspProtocol>(
                    busOps, busInfo, routeRegistry, route, irmClient);
            }

            if (plan->profileBuilder == ProfileBuilderId::WeissInt202 ||
                plan->profileBuilder == ProfileBuilderId::WeissInt203) {
                ASFW_LOG(DICE,
                         "Creating Weiss DICETcatProtocol node=0x%04x unitOffset=%u; DICE "
                         "remains duplex while CoreAudio hides device->host channels",
                         nodeId, plan->unit.unitDirectoryOffset);
                return std::make_unique<DICE::TCAT::DICETcatProtocol>(
                    busOps, busInfo, routeRegistry, route, irmClient, timerScheduler,
                    DICE::TCAT::DICETcatRuntimePolicy{
                        .exposeDeviceToHostToCoreAudio = false,
                        .requireSourceLockBeforeStreamEnable = false,
                        .requireSourceLockAtConfirm = false,
                    });
            }

            ASFW_LOG(DICE,
                     "Creating generic DICETcatProtocol node=0x%04x unitOffset=%u",
                     nodeId, plan->unit.unitDirectoryOffset);
            return std::make_unique<DICE::TCAT::DICETcatProtocol>(
                busOps, busInfo, routeRegistry, route, irmClient, timerScheduler);
        }

        case AudioFamilyProviderId::OXFW: {
            if (plan->profileBuilder == ProfileBuilderId::ApogeeDuet) {
                ASFW_LOG(Audio, "Creating ApogeeDuetProtocol node=0x%04x", nodeId);
                return std::make_unique<Oxford::Apogee::ApogeeDuetProtocol>(
                    busOps, busInfo, route, &routeRegistry, nullptr, irmClient, cmpClient,
                    100U, timerScheduler);
            }

            if (plan->profileBuilder == ProfileBuilderId::MackieOnyxIOxfw) {
                ASFW_LOG(Audio, "Creating MackieOnyxProtocol node=0x%04x", nodeId);
                return std::make_unique<Oxford::Mackie::MackieOnyxProtocol>(
                    busOps, busInfo, route, irmClient, cmpClient, timerScheduler);
            }
            return nullptr;
        }

        case AudioFamilyProviderId::Fireworks: {
            if (plan->profileBuilder == ProfileBuilderId::MackieOnyx400F) {
                ASFW_LOG(Audio, "Creating FireworksProtocol for Onyx 400F node=0x%04x", nodeId);
                return std::make_unique<Fireworks::FireworksProtocol>(
                    busOps, busInfo, route, irmClient, cmpClient, timerScheduler,
                    Fireworks::kOnyx400FGeometry);
            }
            return nullptr;
        }

        case AudioFamilyProviderId::BeBoB: {
            if (plan->profileBuilder == ProfileBuilderId::TerraTecPhase88) {
                ASFW_LOG(Audio, "Creating Phase88Protocol BeBoB/CMP backend node=0x%04x", nodeId);
                return std::make_unique<BeBoB::Phase88Protocol>(
                    busOps, busInfo, route, irmClient, cmpClient, timerScheduler);
            }

            if (plan->profileBuilder == ProfileBuilderId::GenericBeBoB) {
                ASFW_LOG(Audio, "Creating GenericBeBoBProtocol node=0x%04x", nodeId);
                return std::make_unique<BeBoB::GenericBeBoBProtocol>(
                    busOps, busInfo, route, irmClient, cmpClient, timerScheduler,
                    BeBoB::DeviceModel{});
            }
            return nullptr;
        }

        case AudioFamilyProviderId::MotuRegister: {
            if (plan->profileBuilder == ProfileBuilderId::Motu828mk2 ||
                plan->profileBuilder == ProfileBuilderId::MotuUltralite) {
                ASFW_LOG(Audio,
                         "Creating MotuV2Protocol version=0x%06x node=0x%04x",
                         plan->unitVersion, nodeId);
                return std::make_unique<Motu::MotuV2Protocol>(
                    busOps, busInfo, routeRegistry, route, plan->unitVersion, irmClient);
            }
            return nullptr;
        }

        case AudioFamilyProviderId::GenericAvc:
        case AudioFamilyProviderId::None:
            return nullptr;
    }
}

} // namespace ASFW::Audio
