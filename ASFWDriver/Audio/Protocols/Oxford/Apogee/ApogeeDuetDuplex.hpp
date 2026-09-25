// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetDuplex.hpp - The Duet's duplex lifecycle and clock-transition FSM
// (FW-127), lifted out of ApogeeDuetProtocol.
//
// This is the Duet's whole FamilyDriver: stream bring-up, the
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

#include "../../Duplex/FamilyDriver.hpp"
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

class ApogeeDuetDuplex final : public FamilyDriver {
public:
    explicit ApogeeDuetDuplex(DuetRuntime& runtime) noexcept : runtime_(runtime) {}

    ApogeeDuetDuplex(const ApogeeDuetDuplex&) = delete;
    ApogeeDuetDuplex& operator=(const ApogeeDuetDuplex&) = delete;

    /// Cancels any in-flight clock transition and drops the connection state.
    /// Called from the device class's Shutdown.
    void Shutdown() noexcept;

    [[nodiscard]] bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const;

    using VoidCallback = std::function<void(IOReturn)>;

    // The Duet's stages, as callback chains over CMP and AV/C. The
    // FamilyDriver steps below start them and wait; tests drive them directly.
    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const AudioClockConfig& desiredClock,
                       PrepareCallback callback);
    void SetAssignedChannels(const AudioDuplexChannels& channels) noexcept;
    void ProgramRx(StageCallback callback);
    void ProgramTxAndEnableDuplex(StageCallback callback);
    void ConfirmDuplexStart(ConfirmCallback callback);
    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          ClockApplyCallback callback);
    void ReadDuplexHealth(HealthCallback callback);
    void DisconnectPlayback(VoidCallback callback);
    void DisconnectCapture(VoidCallback callback);
    [[nodiscard]] IOReturn StopDuplex();

    // FamilyDriver: each step starts the callback chain above and waits for it
    // (FamilyStageWait.hpp), so the chains and their wire traffic are unchanged.
    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept override;
    [[nodiscard]] IOReturn LoadGeometry() override;
    [[nodiscard]] std::optional<AudioStreamRuntimeCaps> RuntimeCaps() const override;
    [[nodiscard]] std::expected<DuplexPrepareResult, IOReturn> Configure(
        const AudioDuplexChannels& channels, const AudioClockConfig& clock) override;
    void AssignChannels(const AudioDuplexChannels& channels) override;
    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t timeoutMs) override;
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceRx() override;
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceTxAndEnable() override;
    [[nodiscard]] std::expected<DuplexConfirmResult, IOReturn> Confirm() override;
    [[nodiscard]] std::expected<DuplexClockApplyResult, IOReturn> ApplyClockIdle(
        const AudioClockConfig& clock) override;
    [[nodiscard]] IOReturn DisconnectPlayback() override;
    [[nodiscard]] IOReturn DisconnectCapture() override;
    [[nodiscard]] IOReturn BreakConnections() override;
    [[nodiscard]] IOReturn Stop() override;

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
    // Service teardown: the stage waits give up once it reads true.
    const std::atomic<bool>* teardownCancel_{nullptr};

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
