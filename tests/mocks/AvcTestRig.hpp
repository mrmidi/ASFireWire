// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcTestRig.hpp — Standard wiring for an AV/C device test: deferred bus,
// route registry, FWDevice, a real FCPTransport, a virtual clock, and an
// AvcTargetResponder answering FCP on the far side.
//
// Device protocols under test get the real transport, so command framing,
// response matching, retry and timeout are all exercised rather than stubbed.
//
// Host-test only.

#pragma once

#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Discovery/FWDevice.hpp"
#include "ASFWDriver/Protocols/AVC/FCPTransport.hpp"

#include "AvcTargetResponder.hpp"
#include "DeferredFireWireBus.hpp"
#include "FakeTimerScheduler.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace ASFW::Testing {

class AvcTestRig {
public:
    struct Options {
        uint64_t guid{0x3DB0A0000D112ULL};
        uint16_t nodeId{2};
        uint32_t generation{1};
        uint32_t timeoutMs{10};
        uint32_t interimTimeoutMs{25};
        uint8_t maxRetries{0};
    };

    // Defaulted `Options{}` cannot appear as a default argument here: its own
    // member initializers are not parsed until the enclosing class is complete.
    AvcTestRig() : AvcTestRig(Options{}) {}

    explicit AvcTestRig(Options options) : options_(options) {
        bus_.SetGeneration(FW::Generation{options_.generation});
        // Apogee/CMP paths compare-swap PCRs; echoing the compare half is what
        // makes those succeed by default.
        bus_.SetLockMode(Async::Testing::DeferredFireWireBus::LockMode::kEchoCompare);

        Discovery::ConfigROM rom{};
        rom.bib.guid = options_.guid;
        rom.gen = FW::Generation{options_.generation};
        rom.nodeId = options_.nodeId;
        const auto record = routes_.UpsertFromROM(rom, Discovery::LinkPolicy{});

        device_ = Discovery::FWDevice::Create(record, Discovery::ConfigROM{});

        Protocols::AVC::FCPTransportConfig config{};
        config.timeoutMs = options_.timeoutMs;
        config.interimTimeoutMs = options_.interimTimeoutMs;
        config.maxRetries = options_.maxRetries;

        transport_ = std::make_shared<Protocols::AVC::FCPTransport>();
        transportReady_ =
            transport_->init(&bus_, &bus_, device_.get(), routes_, timers_, config);

        target_ = std::make_unique<AvcTargetResponder>(bus_, transport_, options_.nodeId,
                                                       options_.generation);
    }

    ~AvcTestRig() {
        if (transport_) {
            transport_->Shutdown();
        }
    }

    AvcTestRig(const AvcTestRig&) = delete;
    AvcTestRig& operator=(const AvcTestRig&) = delete;

    [[nodiscard]] bool IsReady() const noexcept { return transportReady_ && device_ != nullptr; }

    [[nodiscard]] Async::Testing::DeferredFireWireBus& Bus() noexcept { return bus_; }
    [[nodiscard]] Discovery::DeviceRegistry& Routes() noexcept { return routes_; }
    [[nodiscard]] FakeTimerScheduler& Timers() noexcept { return timers_; }
    [[nodiscard]] AvcTargetResponder& Target() noexcept { return *target_; }
    [[nodiscard]] Protocols::AVC::FCPTransport* Transport() noexcept { return transport_.get(); }
    [[nodiscard]] uint64_t ObservedGuid() const noexcept { return options_.guid; }

    [[nodiscard]] Discovery::DeviceRouteToken Route() const {
        const auto route = device_
                               ? routes_.CurrentRoute(device_->GetInstanceId())
                               : std::nullopt;
        return route.value_or(Discovery::DeviceRouteToken{});
    }

    /// Settle every queued bus write and the AV/C responses they provoke.
    size_t Drain(size_t maxSteps = 256) { return target_->Drain(maxSteps); }

    /// Advance the virtual clock far enough to trip the FCP response timeout.
    void ExpireFcpTimeout() {
        timers_.Advance(static_cast<uint64_t>(options_.timeoutMs + 1U) * 1'000'000ULL);
    }

private:
    Options options_;
    Async::Testing::DeferredFireWireBus bus_;
    FakeTimerScheduler timers_;
    Discovery::DeviceRegistry routes_;
    std::shared_ptr<Discovery::FWDevice> device_;
    std::shared_ptr<Protocols::AVC::FCPTransport> transport_;
    std::unique_ptr<AvcTargetResponder> target_;
    bool transportReady_{false};
};

} // namespace ASFW::Testing
