// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FamilyProtocolConstruction.cpp - Family-keyed device protocol construction

#include "FamilyProtocolConstruction.hpp"
#include "GenericAvcProtocol.hpp"

#include "DICE/Avid/AvidMboxProRouting.hpp"
#include "DICE/Focusrite/SPro24DspProtocol.hpp"
#include "DICE/TCAT/DICETcatProtocol.hpp"
#include "Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "Oxford/Mackie/MackieOnyxProtocol.hpp"
#include "Fireworks/FireworksProtocol.hpp"
#include "BeBoB/Phase88Protocol.hpp"
#include "BeBoB/GenericBeBoBProtocol.hpp"
#include "BeBoB/MAudioSpecialProtocol.hpp"
#include "MOTU/MotuV2Protocol.hpp"
#include "RME/FirefaceDeviceProtocol.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"

namespace ASFW::Audio {

static_assert(
    static_cast<uint8_t>(DeviceProfiles::Audio::AudioFamilyProviderId::kLastValid) ==
    static_cast<uint8_t>(DeviceProfiles::Audio::AudioFamilyProviderId::RmeRegister),
    "AudioFamilyProviderId member added without updating family protocol construction");

static_assert(
    static_cast<uint8_t>(DeviceProfiles::Audio::ProtocolImplementationId::kLastValid) ==
    static_cast<uint8_t>(DeviceProfiles::Audio::ProtocolImplementationId::GenericAvc),
    "ProtocolImplementationId member added without updating family protocol construction");

std::unique_ptr<IDeviceProtocol> CreateFamilyDeviceProtocol(
    const DeviceProfiles::Audio::StaticAudioEndpointPlan& plan,
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    Discovery::DeviceRegistry& routeRegistry,
    const Discovery::DeviceRouteToken& route,
    IRM::IRMClient* irmClient,
    CMP::CMPClient* cmpClient,
    Scheduling::ITimerScheduler* timerScheduler,
    DICE::DiceNotificationRouter* diceNotifications,
    bool isS800
) {
    if (!route) {
        return nullptr;
    }
    const uint16_t nodeId = route.nodeId;

    if (!DeviceProfiles::Audio::AllowsAudioRuntime(plan.support) ||
        plan.protocolImplementation ==
            DeviceProfiles::Audio::ProtocolImplementationId::None) {
        return nullptr;
    }

    using DeviceProfiles::Audio::AudioFamilyProviderId;
    using DeviceProfiles::Audio::ProtocolImplementationId;

    // Family guard: exhaustive switch over all 7 families with no default: arm.
    // Adding an AudioFamilyProviderId without updating this switch must not compile.
    switch (plan.family) {
        case AudioFamilyProviderId::DICE:
        case AudioFamilyProviderId::OXFW:
        case AudioFamilyProviderId::Fireworks:
        case AudioFamilyProviderId::BeBoB:
        case AudioFamilyProviderId::MotuRegister:
        case AudioFamilyProviderId::RmeRegister:
        case AudioFamilyProviderId::GenericAvc:
            break;

        case AudioFamilyProviderId::None:
            return nullptr;
    }

    // The catalog independently selects the concrete protocol. ProfileBuilderId
    // remains an endpoint/profile choice and is not a factory dispatch key.
    switch (plan.protocolImplementation) {
        // --- DICE Family ---
        case ProtocolImplementationId::DiceSPro24Dsp:
            ASFW_LOG(DICE,
                     "Creating SPro24DspProtocol node=0x%04x unitOffset=%u",
                     nodeId, plan.unit.unitDirectoryOffset);
            return std::make_unique<DICE::Focusrite::SPro24DspProtocol>(
                busOps, busInfo, routeRegistry, route, irmClient,
                DICE::DriverKitWaitClock::Shared(), diceNotifications);

        // The plain TCAT devices differ in their profile, not their protocol:
        // geometry comes from the device's own registers either way.
        case ProtocolImplementationId::DiceTcat:
            // The Avid Mbox Pro is a generic TCAT device in every respect but
            // one: it powers up with no usable router program and passes
            // nothing to its analog stage until a host writes one, so it
            // carries a startup program the other TCAT devices do not need.
            if (plan.profileBuilder ==
                DeviceProfiles::Audio::ProfileBuilderId::AvidMboxPro) {
                ASFW_LOG(DICE,
                         "Creating Avid Mbox Pro DICETcatProtocol node=0x%04x unitOffset=%u "
                         "with %u startup router entries",
                         nodeId, plan.unit.unitDirectoryOffset,
                         DICE::Avid::MboxProRouting::kRouterEntryCount);
                return std::make_unique<DICE::TCAT::DICETcatProtocol>(
                    busOps, busInfo, routeRegistry, route, irmClient,
                    DICE::DriverKitWaitClock::Shared(), diceNotifications,
                    DICE::TCAT::DICETcatRuntimePolicy{
                        .startupRouterEntries = DICE::Avid::MboxProRouting::kRouterEntries,
                        .startupRouterEntryCount = DICE::Avid::MboxProRouting::kRouterEntryCount,
                        .startupMixerCells =
                            DICE::Avid::MboxProRouting::kStartupMixerCoefficients,
                        .startupMixerCellCount =
                            DICE::Avid::MboxProRouting::kStartupMixerCoefficientCount,
                    });
            }
            ASFW_LOG(DICE,
                     "Creating generic DICETcatProtocol node=0x%04x unitOffset=%u",
                     nodeId, plan.unit.unitDirectoryOffset);
            return std::make_unique<DICE::TCAT::DICETcatProtocol>(
                busOps, busInfo, routeRegistry, route, irmClient,
                DICE::DriverKitWaitClock::Shared(), diceNotifications);

        // Weiss is the one DICE device with a non-default runtime policy: it is
        // a one-way interface, so CoreAudio must not be shown the device->host
        // side and source lock is not a precondition for enabling a stream.
        case ProtocolImplementationId::DiceWeissInt:
            ASFW_LOG(DICE,
                     "Creating Weiss DICETcatProtocol node=0x%04x unitOffset=%u; DICE "
                     "remains duplex while CoreAudio hides device->host channels",
                     nodeId, plan.unit.unitDirectoryOffset);
            return std::make_unique<DICE::TCAT::DICETcatProtocol>(
                busOps, busInfo, routeRegistry, route, irmClient,
                DICE::DriverKitWaitClock::Shared(), diceNotifications,
                DICE::TCAT::DICETcatRuntimePolicy{
                    .exposeDeviceToHostToCoreAudio = false,
                    .requireSourceLockBeforeStreamEnable = false,
                    .requireSourceLockAtConfirm = false,
                });

        // --- OXFW Family ---
        case ProtocolImplementationId::ApogeeDuet:
            ASFW_LOG(Audio, "Creating ApogeeDuetProtocol node=0x%04x", nodeId);
            // Factory path intentionally does not bind FCP transport yet.
            // AVCDiscovery wires transport for live command execution.
            return std::make_unique<Oxford::Apogee::ApogeeDuetProtocol>(
                busOps, busInfo, route, &routeRegistry, nullptr, irmClient, cmpClient,
                100U, timerScheduler);

        // Mackie Onyx-i, Oxford run (shared id 0x081216; geometry verified on a
        // real 820i). Plain AV/C + CMP duplex on the shared base -- no vendor codec.
        case ProtocolImplementationId::MackieOnyx:
            ASFW_LOG(Audio, "Creating MackieOnyxProtocol node=0x%04x", nodeId);
            return std::make_unique<Oxford::Mackie::MackieOnyxProtocol>(
                busOps, busInfo, route, irmClient, cmpClient, timerScheduler);

        // --- Fireworks Family ---
        // Mackie Onyx 400F, Echo Fireworks run: EFC-controlled clock on top of
        // the shared AV/C+CMP duplex base. Static 10x10 geometry is verified
        // against HWINFO before the first stream (Linux snd-fireworks is the
        // reference).
        case ProtocolImplementationId::FireworksOnyx400F:
            ASFW_LOG(Audio, "Creating FireworksProtocol for Onyx 400F node=0x%04x", nodeId);
            return std::make_unique<Fireworks::FireworksProtocol>(
                busOps, busInfo, route, irmClient, cmpClient, timerScheduler,
                Fireworks::kOnyx400FGeometry);

        // --- BeBoB Family ---
        case ProtocolImplementationId::BeBoBPhase88:
            ASFW_LOG(Audio, "Creating Phase88Protocol BeBoB/CMP backend node=0x%04x", nodeId);
            return std::make_unique<BeBoB::Phase88Protocol>(
                busOps, busInfo, route, irmClient, cmpClient, timerScheduler);

        // Conservative defaults -- plug-0, CMP, no mixer programming. No catalog
        // row selects this today (the one BeBoB device on this branch, the
        // PHASE 88, has its own builder), so it is reachable only when a future
        // row names it.
        case ProtocolImplementationId::BeBoBGeneric:
            ASFW_LOG(Audio, "Creating GenericBeBoBProtocol node=0x%04x", nodeId);
            return std::make_unique<BeBoB::GenericBeBoBProtocol>(
                busOps, busInfo, route, irmClient, cmpClient, timerScheduler,
                BeBoB::DeviceModel{});

        case ProtocolImplementationId::BeBoBMAudioSpecial:
            if (plan.profileBuilder != DeviceProfiles::Audio::ProfileBuilderId::MAudioFireWire1814 &&
                plan.profileBuilder != DeviceProfiles::Audio::ProfileBuilderId::MAudioProjectMix) {
                return nullptr;
            }
            ASFW_LOG(Audio, "Creating M-Audio special BeBoB protocol node=0x%04x", nodeId);
            return std::make_unique<BeBoB::MAudioSpecialProtocol>(
                busOps, busInfo, route, irmClient, cmpClient, timerScheduler,
                plan.profileBuilder == DeviceProfiles::Audio::ProfileBuilderId::MAudioFireWire1814);

        // --- MotuRegister Family ---
        // MOTU publishes model_id 0; the model is the unit's Unit_Sw_Version,
        // which the protocol needs in order to pick its chunk layout.
        // The protocol keeps the IRM client for its own use; the audio session
        // reserves the iso channels with the same client before programming
        // the device.
        case ProtocolImplementationId::MotuV2:
            ASFW_LOG(Audio,
                     "Creating MotuV2Protocol version=0x%06x node=0x%04x",
                     plan.unitVersion, nodeId);
            return std::make_unique<Motu::MotuV2Protocol>(
                busOps, busInfo, routeRegistry, route, plan.unitVersion, irmClient);

        case ProtocolImplementationId::RmeFireface: {
            const auto definition = plan.candidates.empty()
                ? DeviceProfiles::Audio::DeviceDefinitionId::Unknown
                : plan.candidates.front();
            const auto model = definition == DeviceProfiles::Audio::DeviceDefinitionId::RmeFireface800
                ? RME::FirefaceModel::kFF800 : RME::FirefaceModel::kFF400;
            return std::make_unique<RME::FirefaceDeviceProtocol>(
                busOps, busInfo, routeRegistry, route, model, isS800);
        }

        case ProtocolImplementationId::GenericAvc:
            ASFW_LOG(Audio, "[AvcRuntime] generic CMP backend node=0x%04x", nodeId);
            return std::make_unique<GenericAvcProtocol>(
                busOps, busInfo, route, irmClient, cmpClient, timerScheduler);

        case ProtocolImplementationId::None:
            return nullptr;
    }

    return nullptr;
}

} // namespace ASFW::Audio
