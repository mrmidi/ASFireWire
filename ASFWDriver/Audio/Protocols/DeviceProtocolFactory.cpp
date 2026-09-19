// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// DeviceProtocolFactory.cpp - Factory for creating device-specific protocol handlers

#include "DeviceProtocolFactory.hpp"
#include "DICE/Focusrite/SPro24DspProtocol.hpp"
#include "DICE/TCAT/DICETcatProtocol.hpp"
#include "Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "Oxford/Mackie/MackieOnyxProtocol.hpp"
#include "Fireworks/FireworksProtocol.hpp"
#include "BeBoB/Phase88Protocol.hpp"
#include "BeBoB/GenericBeBoBProtocol.hpp"
#include "MOTU/MotuV2Protocol.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"

namespace ASFW::Audio {

namespace {

using DeviceProfiles::Audio::ProfileBuilderId;

} // namespace

std::unique_ptr<IDeviceProtocol> DeviceProtocolFactory::Create(
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

    const auto choice = ChooseDeviceProtocol(record);
    if (!choice.has_value()) {
        // Not a device this driver streams. Silent by design: an unknown device
        // on the bus is not a fault. A device the catalog calls Supported can no
        // longer land here -- a Supported row must name a builder (asserted in
        // AudioDeviceCatalogTests) and every builder is handled below, with no
        // default arm, so omitting one is a compile error rather than issue
        // #115's silent nub-without-a-protocol.
        return nullptr;
    }

    // No `default:`. Adding a ProfileBuilderId without teaching this switch what
    // to construct must not compile.
    switch (choice->builder) {
        case ProfileBuilderId::FocusriteSPro24Dsp:
            ASFW_LOG(DICE,
                     "Creating SPro24DspProtocol node=0x%04x unitOffset=%u",
                     nodeId, choice->unitDirectoryOffset);
            return std::make_unique<DICE::Focusrite::SPro24DspProtocol>(
                busOps, busInfo, routeRegistry, route, irmClient);

        // The plain TCAT devices differ in their profile, not their protocol:
        // geometry comes from the device's own registers either way.
        case ProfileBuilderId::FocusriteSPro14:
        case ProfileBuilderId::FocusriteSPro24:
        case ProfileBuilderId::FocusriteSPro40:
        case ProfileBuilderId::FocusriteLiquidS56:
        case ProfileBuilderId::AlesisMultiMix:
        case ProfileBuilderId::MidasVeniceF32:
        case ProfileBuilderId::PreSonusStudioLive1602:
        case ProfileBuilderId::PreSonusStudioLive2442:
            ASFW_LOG(DICE,
                     "Creating generic DICETcatProtocol node=0x%04x unitOffset=%u",
                     nodeId, choice->unitDirectoryOffset);
            return std::make_unique<DICE::TCAT::DICETcatProtocol>(
                busOps, busInfo, routeRegistry, route, irmClient, timerScheduler);

        // Weiss is the one DICE device with a non-default runtime policy: it is
        // a one-way interface, so CoreAudio must not be shown the device->host
        // side and source lock is not a precondition for enabling a stream.
        case ProfileBuilderId::WeissInt202:
        case ProfileBuilderId::WeissInt203:
            ASFW_LOG(DICE,
                     "Creating Weiss DICETcatProtocol node=0x%04x unitOffset=%u; DICE "
                     "remains duplex while CoreAudio hides device->host channels",
                     nodeId, choice->unitDirectoryOffset);
            return std::make_unique<DICE::TCAT::DICETcatProtocol>(
                busOps, busInfo, routeRegistry, route, irmClient, timerScheduler,
                DICE::TCAT::DICETcatRuntimePolicy{
                    .exposeDeviceToHostToCoreAudio = false,
                    .requireSourceLockBeforeStreamEnable = false,
                    .requireSourceLockAtConfirm = false,
                });

        case ProfileBuilderId::ApogeeDuet:
            ASFW_LOG(Audio, "Creating ApogeeDuetProtocol node=0x%04x", nodeId);
            // Factory path intentionally does not bind FCP transport yet.
            // AVCDiscovery wires transport for live command execution.
            return std::make_unique<Oxford::Apogee::ApogeeDuetProtocol>(
                busOps, busInfo, route, &routeRegistry, nullptr, irmClient, cmpClient,
                100U, timerScheduler);

        // Mackie Onyx-i, Oxford run (shared id 0x081216; geometry verified on a
        // real 820i). Plain AV/C + CMP duplex on the shared base -- no vendor codec.
        case ProfileBuilderId::MackieOnyxIOxfw:
            ASFW_LOG(Audio, "Creating MackieOnyxProtocol node=0x%04x", nodeId);
            return std::make_unique<Oxford::Mackie::MackieOnyxProtocol>(
                busOps, busInfo, route, irmClient, cmpClient, timerScheduler);

        // Mackie Onyx 400F, Echo Fireworks run: EFC-controlled clock on top of
        // the shared AV/C+CMP duplex base. Static 10x10 geometry is verified
        // against HWINFO before the first stream (Linux snd-fireworks is the
        // reference).
        case ProfileBuilderId::MackieOnyx400F:
            ASFW_LOG(Audio, "Creating FireworksProtocol for Onyx 400F node=0x%04x", nodeId);
            return std::make_unique<Fireworks::FireworksProtocol>(
                busOps, busInfo, route, irmClient, cmpClient, timerScheduler,
                Fireworks::kOnyx400FGeometry);

        case ProfileBuilderId::TerraTecPhase88:
            ASFW_LOG(Audio, "Creating Phase88Protocol BeBoB/CMP backend node=0x%04x", nodeId);
            return std::make_unique<BeBoB::Phase88Protocol>(
                busOps, busInfo, route, irmClient, cmpClient, timerScheduler);

        // Conservative defaults -- plug-0, CMP, no mixer programming. No catalog
        // row selects this today (the one BeBoB device on this branch, the
        // PHASE 88, has its own builder), so it is reachable only when a future
        // row names it.
        case ProfileBuilderId::GenericBeBoB:
            ASFW_LOG(Audio, "Creating GenericBeBoBProtocol node=0x%04x", nodeId);
            return std::make_unique<BeBoB::GenericBeBoBProtocol>(
                busOps, busInfo, route, irmClient, cmpClient, timerScheduler,
                BeBoB::DeviceModel{});

        // MOTU publishes model_id 0; the model is the unit's Unit_Sw_Version,
        // which the protocol needs in order to pick its chunk layout.
        // The IRM client must reach the protocol: the coordinator allocates iso
        // channels through IDuplexDeviceControl::GetIRMClient() before
        // programming the device.
        case ProfileBuilderId::Motu828mk2:
        case ProfileBuilderId::MotuUltralite:
            ASFW_LOG(Audio,
                     "Creating MotuV2Protocol version=0x%06x node=0x%04x",
                     choice->unitVersion, nodeId);
            return std::make_unique<Motu::MotuV2Protocol>(
                busOps, busInfo, routeRegistry, route, choice->unitVersion, irmClient);

        // An unknown AV/C unit resolves to the generic fallback in the catalog,
        // which is a *classification*, not a decision to stream it. This branch
        // has no generic AV/C backend, and inventing one here would start
        // talking to every AV/C device on the bus.
        case ProfileBuilderId::GenericAvc:
        // M-Audio's special firmware is recognised for its command bound only;
        // this branch has no MAudioSpecialProtocol. See AVCCommandFilter.hpp.
        case ProfileBuilderId::MAudioFireWire1814:
        case ProfileBuilderId::MAudioProjectMix:
        case ProfileBuilderId::None:
            return nullptr;
    }
    return nullptr;
}

} // namespace ASFW::Audio
