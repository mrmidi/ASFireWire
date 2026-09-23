// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Minimal audio protocol for M-Audio FireWire 1814 and ProjectMix special firmware.

#pragma once

#include "BeBoBProtocol.hpp"

#include <atomic>
#include <memory>

namespace ASFW::Audio::BeBoB {

/// Special-firmware clock and start choreography. This protocol intentionally
/// has no mixer, meter, LED, or control-surface operations.
class MAudioSpecialProtocol final : public BeBoBProtocol {
public:
    MAudioSpecialProtocol(Protocols::Ports::FireWireBusOps& busOps,
                          Protocols::Ports::FireWireBusInfo& busInfo,
                          Discovery::DeviceRouteToken route,
                          IRM::IRMClient* irmClient,
                          CMP::CMPClient* cmpClient,
                          Scheduling::ITimerScheduler* timerScheduler,
                          bool isFireWire1814) noexcept;
    ~MAudioSpecialProtocol() override;

    const char* GetName() const override;
    IOReturn Shutdown() override;
    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              Protocols::AVC::FCPTransport* transport) override;
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          ClockApplyCallback callback) override;
    void ConfirmDuplexStart(ConfirmCallback callback) override;

protected:
    const char* DeviceName() const override { return GetName(); }
    [[nodiscard]] AudioStreamRuntimeCaps DeviceCaps() const override;
    [[nodiscard]] std::vector<uint32_t> SupportedRates() const override;
    [[nodiscard]] uint32_t SignalFormatInterlockMs() const noexcept override { return 100; }

private:
    struct PendingPostStart final {
        std::atomic<bool> completed{false};
        ConfirmCallback callback;
    };

    void CancelPostStartTimer();
    void FinishPostStart(const std::shared_ptr<PendingPostStart>& pending,
                         IOReturn status, DuplexConfirmResult result);
    static void CompletePostStart(const std::shared_ptr<PendingPostStart>& pending,
                                  IOReturn status, DuplexConfirmResult result);
    void SetSignalFormat(uint32_t rateHz, bool input,
                         std::function<void(IOReturn)> completion);
    void SetPostStartOutput(uint32_t rateHz,
                            std::function<void(IOReturn)> completion);
    void SetPostStartInput(uint32_t rateHz,
                           std::function<void(IOReturn)> completion);

    bool isFireWire1814_{false};
    uint32_t appliedRateHz_{48000};
    std::shared_ptr<std::atomic<bool>> alive_;
    Scheduling::TimerToken postStartTimer_{Scheduling::kInvalidTimerToken};
    std::shared_ptr<PendingPostStart> postStartCompletion_;
};

} // namespace ASFW::Audio::BeBoB
