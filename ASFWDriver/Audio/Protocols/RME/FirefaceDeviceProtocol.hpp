// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "FirefaceFamilyDriver.hpp"
#include "../IDeviceProtocol.hpp"

namespace ASFW::Audio::RME {

/// Owns register IO before the family adapter that borrows it. Initialization
/// only publishes fixed geometry; firmware/status reads are deferred to the
/// 48 kHz session Configure gate.
class FirefaceDeviceProtocol final : public IDeviceProtocol {
public:
    FirefaceDeviceProtocol(Protocols::Ports::FireWireBusOps& busOps,
                           Protocols::Ports::FireWireBusInfo& busInfo,
                           Discovery::DeviceRegistry& registry,
                           const Discovery::DeviceRouteToken& route,
                           FirefaceModel model, bool s800) noexcept
        : registry_(registry), model_(model), io_(busOps, busInfo, registry, route), family_(io_, model, s800) {}

    IOReturn Initialize() override { return family_.LoadGeometry(); }
    IOReturn Shutdown() override { return kIOReturnSuccess; }
    const char* GetName() const override {
        return model_ == FirefaceModel::kFF800 ? "RME Fireface 800" : "RME Fireface 400";
    }
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& out) const override {
        const auto caps = family_.RuntimeCaps();
        if (!caps) return false;
        out = *caps;
        return true;
    }
    void EnsureRuntimeStreamGeometry(VoidCallback callback) override {
        callback(family_.LoadGeometry());
    }
    FamilyDriver* AsFamilyDriver() noexcept override { return &family_; }
    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              std::shared_ptr<ASFW::AVC::IAvcUnit>) override {
        io_.UpdateRoute(route);
        if (const auto record = registry_.SnapshotByGuid(route.guid)) {
            family_.SetLinkSpeed(record->link.isochToNode == FW::FwSpeed::S800);
        }
    }
    void UpdateLinkSpeed(bool s800) noexcept { family_.SetLinkSpeed(s800); }

private:
    Discovery::DeviceRegistry& registry_;
    FirefaceModel model_;
    Protocols::Ports::ProtocolRegisterIO io_;
    FirefaceFamilyDriver family_;
};

} // namespace ASFW::Audio::RME
