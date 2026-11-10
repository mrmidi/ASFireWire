// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xDeviceProtocol.hpp - IDeviceProtocol for the Digidesign Digi 002 /
// 003 family (issue #249). Owns the register IO before the family driver that
// borrows it. Initialization only publishes fixed geometry; register access
// happens in the session stages, and after a bus reset, when discovery asks
// for the message address again (RebindNotifications).

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
    // Linux re-sends the message address after every bus reset
    // (digi00x.c update -> snd_dg00x_transaction_reregister). The caller keeps
    // this protocol alive until the callback runs.
    void RebindNotifications(VoidCallback callback) override {
        const auto block = Messages::AddressBlock(io_.LocalNodeId());
        if (!block) {
            callback(kIOReturnNotReady);
            return;
        }
        const auto handle = io_.WriteBlock(
            Async::FWAddress{Async::FWAddress::AddressParts{
                .addressHi = static_cast<uint16_t>(Register::kMessageAddress >> 32U),
                .addressLo = static_cast<uint32_t>(Register::kMessageAddress)}},
            std::span<const uint8_t>(block->data(), block->size()),
            [callback = std::move(callback)](Async::AsyncStatus status) mutable {
                callback(Protocols::Ports::MapAsyncStatusToIOReturn(status));
            });
        (void)handle;
    }
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
