// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xDeviceProtocol.hpp - IDeviceProtocol for the Digidesign Digi 002 /
// 003 family (issue #249). Owns the register IO before the family driver that
// borrows it. Initialization only publishes fixed geometry; every register
// access happens in the session stages.

#pragma once

#include "Digi00xFamilyDriver.hpp"
#include "../IDeviceProtocol.hpp"

#include <string>
#include <utility>

namespace ASFW::Audio::Digi00x {

class Digi00xDeviceProtocol final : public IDeviceProtocol {
public:
    Digi00xDeviceProtocol(Protocols::Ports::FireWireBusOps& busOps,
                          Protocols::Ports::FireWireBusInfo& busInfo,
                          Discovery::DeviceRegistry& registry,
                          const Discovery::DeviceRouteToken& route,
                          std::string name) noexcept
        : name_(std::move(name)), io_(busOps, busInfo, registry, route), family_(io_) {}

    IOReturn Initialize() override { return family_.LoadGeometry(); }
    IOReturn Shutdown() override { return kIOReturnSuccess; }
    const char* GetName() const override { return name_.c_str(); }
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
    }

private:
    std::string name_;
    Protocols::Ports::ProtocolRegisterIO io_;
    Digi00xFamilyDriver family_;
};

} // namespace ASFW::Audio::Digi00x
