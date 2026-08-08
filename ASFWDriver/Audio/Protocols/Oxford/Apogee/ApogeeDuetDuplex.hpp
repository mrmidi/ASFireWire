// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetDuplex.hpp - The Duet's duplex lifecycle and clock-transition FSM
// (FW-127), lifted out of ApogeeDuetProtocol.
//
// This is the whole IDuplexDeviceControl implementation: stream bring-up, the
// AV/C signal-format transition machine, CMP plug programming and teardown. It
// owns every piece of state that only the duplex path reads, so the device class
// above it keeps nothing but composition.
//
// What it does *not* own is the runtime identity shared with the parameter and
// meter paths - route, transport and the bus clients. That lives in DuetRuntime,
// held by ApogeeDuetProtocol and referenced here, so a route rebind is visible
// to both sides without either copying it.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include <DriverKit/IOReturn.h>

#include "../../Duplex/IDuplexDeviceControl.hpp"
#include "../../../../Discovery/DeviceRouteToken.hpp"

namespace ASFW::Discovery {
class DeviceRegistry;
}
#include "../../../../Protocols/Ports/FireWireBusPort.hpp"
#include "../../../../Scheduling/ITimerScheduler.hpp"

namespace ASFW::Protocols::AVC {
class FCPTransport;
}

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::CMP {
class CMPClient;
struct CMPDevice;
}

namespace ASFW::Audio::Oxford::Apogee {

/// The device identity and bus services both halves of the Duet need. Held by
/// ApogeeDuetProtocol; the duplex controller mutates `route` and `fcpTransport`
/// through UpdateRuntimeContext, and the parameter path reads the same fields.
struct DuetRuntime {
    Protocols::Ports::FireWireBusOps& busOps;
    Protocols::Ports::FireWireBusInfo& busInfo;
    Discovery::DeviceRouteToken route{};
    /// Source of truth for the *current* route. Register reads resolve through
    /// this per use rather than trusting `route`, which is a construction-time
    /// snapshot (FW-142).
    Discovery::DeviceRegistry* routeRegistry{nullptr};
    Protocols::AVC::FCPTransport* fcpTransport{nullptr};
    IRM::IRMClient* irmClient{nullptr};
    CMP::CMPClient* cmpClient{nullptr};
    Scheduling::ITimerScheduler* timerScheduler{nullptr};
    uint32_t formatSettleDelayMs{100U};
};

class ApogeeDuetDuplex final : public IDuplexDeviceControl {
public:
    explicit ApogeeDuetDuplex(DuetRuntime& runtime) noexcept : runtime_(runtime) {}

    ApogeeDuetDuplex(const ApogeeDuetDuplex&) = delete;
    ApogeeDuetDuplex& operator=(const ApogeeDuetDuplex&) = delete;

    /// Cancels any in-flight clock transition and drops the connection state.
    /// Called from the device class's Shutdown.
    void Shutdown() noexcept;

    [[nodiscard]] bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const;

    // IDuplexDeviceControl
    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const AudioClockConfig& desiredClock,
                       PrepareCallback callback) override;
    void SetAssignedChannels(const AudioDuplexChannels& channels) noexcept override;
    void ProgramRx(StageCallback callback) override;
    void ProgramTxAndEnableDuplex(StageCallback callback) override;
    void ConfirmDuplexStart(ConfirmCallback callback) override;
    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          ClockApplyCallback callback) override;
    void ReadDuplexHealth(HealthCallback callback) override;
    void DisconnectPlayback(VoidCallback callback) override;
    void DisconnectCapture(VoidCallback callback) override;
    [[nodiscard]] IOReturn StopDuplex() override;
    [[nodiscard]] IRM::IRMClient* GetIRMClient() const override { return runtime_.irmClient; }

    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              Protocols::AVC::FCPTransport* transport);

private:
    struct ClockTransition;

    [[nodiscard]] bool IsActive(const ClockTransition& transition) const noexcept;
    [[nodiscard]] CMP::CMPDevice CurrentCMPDevice() const noexcept;

    void AdvanceClockTransition(const std::shared_ptr<ClockTransition>& transition);
    void FailClockTransition(const std::shared_ptr<ClockTransition>& transition, IOReturn status);
    void CancelClockTransition(IOReturn status);
    void CompleteClockTransition(const std::shared_ptr<ClockTransition>& transition,
                                 IOReturn status);
    void FinishClockTransition(const std::shared_ptr<ClockTransition>& transition, IOReturn status);

    DuetRuntime& runtime_;

    AudioDuplexChannels duplexChannels_{};
    AudioClockConfig appliedClock_{};
    uint64_t preparedRouteEpoch_{0};
    bool clockConfigApplied_{false};
    bool outputConnected_{false};
    bool inputConnected_{false};

    std::shared_ptr<ClockTransition> activeClockTransition_{nullptr};
    uint64_t activeClockTransitionEpoch_{0};
    uint64_t nextClockTransitionEpoch_{0};
};

} // namespace ASFW::Audio::Oxford::Apogee
