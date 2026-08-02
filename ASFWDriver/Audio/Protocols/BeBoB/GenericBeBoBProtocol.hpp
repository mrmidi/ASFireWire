// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// GenericBeBoBProtocol.hpp — Concrete BeBoB fallback for known-but-untested devices.
//
// Inherits the general BeBoB/CMP lifecycle from BeBoBProtocol. Derives stream
// geometry from discovery data (BeBoBPlug0StreamDiscovery::DeviceModel) rather
// than hardcoding Phase88's 10+1@48k. Provides conservative defaults: plug-0,
// CMP-based, no mixer programming, PCR-connectivity health.
//
// Use this for any recognized BeBoB device that lacks a HW-verified custom protocol.
// Verified devices (Phase88) get their own subclass with optimized behavior.

#pragma once

#include "BeBoBProtocol.hpp"
#include "BeBoBPlug0StreamDiscovery.hpp"

#include <vector>

namespace ASFW::Audio::BeBoB {

class GenericBeBoBProtocol final : public BeBoBProtocol {
public:
    GenericBeBoBProtocol(Protocols::Ports::FireWireBusOps& busOps,
                         Protocols::Ports::FireWireBusInfo& busInfo,
                         uint16_t nodeId,
                         IRM::IRMClient* irmClient,
                         CMP::CMPClient* cmpClient,
                         uint64_t deviceGuid,
                         Scheduling::ITimerScheduler* timerScheduler,
                         const DeviceModel& discoveryModel) noexcept;

    const char* GetName() const override { return deviceName_; }

protected:
    const char* DeviceName() const override { return deviceName_; }
    [[nodiscard]] AudioStreamRuntimeCaps DeviceCaps() const override { return caps_; }
    [[nodiscard]] std::vector<uint32_t> SupportedRates() const override { return supportedRates_; }
    void ReadClockHealth(HealthCallback callback) override;

private:
    // Derive supported rates (Hz) from the discovery formation list.
    static std::vector<uint32_t> MakeSupportedRates(const DeviceModel& model) noexcept;

    const char* deviceName_{nullptr};
    AudioStreamRuntimeCaps caps_{};
    std::vector<uint32_t> supportedRates_;
};

} // namespace ASFW::Audio::BeBoB
