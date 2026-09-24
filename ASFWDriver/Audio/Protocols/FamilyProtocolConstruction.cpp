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
#include "BeBoB/MAudioSpecialProtocol.hpp"
#include "MOTU/MotuV2Protocol.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"

namespace ASFW::Audio {

static_assert(
    static_cast<uint8_t>(DeviceProfiles::Audio::AudioFamilyProviderId::kLastValid) ==
    static_cast<uint8_t>(DeviceProfiles::Audio::AudioFamilyProviderId::MotuRegister),
    "AudioFamilyProviderId member added without updating family protocol construction");

static_assert(
    static_cast<uint8_t>(DeviceProfiles::Audio::ProtocolImplementationId::kLastValid) ==
    static_cast<uint8_t>(DeviceProfiles::Audio::ProtocolImplementationId::MotuV2),
    "ProtocolImplementationId member added without updating family protocol construction");

std::unique_ptr<IDeviceProtocol> CreateFamilyDeviceProtocol(
    const DeviceProfiles::Audio::StaticAudioEndpointPlan& plan,
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

    if (plan.support != DeviceProfiles::Audio::SupportDisposition::Supported ||
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
            break;

        case AudioFamilyProviderId::GenericAvc:
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
                DICE::DriverKitWaitClock::Shared());

        // The plain TCAT devices differ in their profile, not their protocol:
        // geometry comes from the device's own registers either way.
        case ProtocolImplementationId::DiceTcat:
            ASFW_LOG(DICE,
                     "Creating generic DICETcatProtocol node=0x%04x unitOffset=%u",
                     nodeId, plan.unit.unitDirectoryOffset);
            return std::make_unique<DICE::TCAT::DICETcatProtocol>(
                busOps, busInfo, routeRegistry, route, irmClient,
                DICE::DriverKitWaitClock::Shared());

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
                DICE::DriverKitWaitClock::Shared(),
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
        // The IRM client must reach the protocol: the coordinator allocates iso
        // channels through IDuplexDeviceControl::GetIRMClient() before
        // programming the device.
        case ProtocolImplementationId::MotuV2:
            ASFW_LOG(Audio,
                     "Creating MotuV2Protocol version=0x%06x node=0x%04x",
                     plan.unitVersion, nodeId);
            return std::make_unique<Motu::MotuV2Protocol>(
                busOps, busInfo, routeRegistry, route, plan.unitVersion, irmClient);

        // --- Generic AV/C & None ---
        // An unknown AV/C unit resolves to the generic fallback in the catalog,
        // which is a *classification*, not a decision to stream it. This branch
        // has no generic AV/C backend, and inventing one here would start
        // talking to every AV/C device on the bus.
        case ProtocolImplementationId::None:
            return nullptr;
    }

    return nullptr;
}

} // namespace ASFW::Audio
