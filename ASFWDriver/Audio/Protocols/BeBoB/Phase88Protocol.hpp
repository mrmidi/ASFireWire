// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Exact TerraTec PHASE 88 Rack FW BeBoB/CMP adapter.

#pragma once

#include "../Duplex/IDuplexDeviceControl.hpp"
#include "../IDeviceProtocol.hpp"
#include "../../../Protocols/Ports/FireWireBusPort.hpp"
#include "../../../Protocols/AVC/CMP/CMPClient.hpp"

#include <cstdint>
#include <functional>

namespace ASFW::Protocols::AVC {
class FCPTransport;
}

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Scheduling {
class ITimerScheduler;
} // namespace ASFW::Scheduling

#include "../../../Protocols/AVC/IAVCCommandSubmitter.hpp"

namespace ASFW::Audio::BeBoB {

class Phase88Protocol final : public IDeviceProtocol, public IDuplexDeviceControl, public Protocols::AVC::IAVCCommandSubmitter {
public:
    Phase88Protocol(Protocols::Ports::FireWireBusOps& busOps,
                    Protocols::Ports::FireWireBusInfo& busInfo,
                    uint16_t nodeId,
                    IRM::IRMClient* irmClient,
                    CMP::CMPClient* cmpClient,
                    uint64_t deviceGuid,
                    Scheduling::ITimerScheduler* timerScheduler) noexcept;

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override { return "TerraTec PHASE 88 Rack FW"; }
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    IDuplexDeviceControl* AsDuplexDeviceControl() noexcept override { return this; }
    const IDuplexDeviceControl* AsDuplexDeviceControl() const noexcept override { return this; }
    void UpdateRuntimeContext(uint16_t nodeId,
                              Protocols::AVC::FCPTransport* transport) override;

    // IAVCCommandSubmitter
    void SubmitCommand(const Protocols::AVC::AVCCdb& cdb, Protocols::AVC::AVCCompletion completion) override;

    // BeBoB starts the device's input PCR first, then its output PCR. The
    // generic coordinator supplies the Linux-equivalent ordering around these
    // two stages: reserve both channels -> IPCR -> OPCR -> host IR/IT start.
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
    void BreakBothConnections(VoidCallback callback) override;
    [[nodiscard]] IOReturn StopDuplex() override;
    [[nodiscard]] IRM::IRMClient* GetIRMClient() const override { return irmClient_; }

private:
    void EnsurePlugFree(CMP::PCRDirection dir, uint8_t plug, std::function<void(IOReturn)> cb);
    [[nodiscard]] CMP::CMPDevice CurrentCMPDevice() const noexcept;
    [[nodiscard]] IOReturn ResetEpochIfNeeded() noexcept;

    Protocols::Ports::FireWireBusInfo& busInfo_;
    uint16_t nodeId_{0};
    IRM::IRMClient* irmClient_{nullptr};
    CMP::CMPClient* cmpClient_{nullptr};
    // The runtime owns this transport. It is refreshed on every discovery
    // epoch and used only for the profile's AV/C unit-plug format commands.
    Protocols::AVC::FCPTransport* fcpTransport_{nullptr};
    uint64_t deviceGuid_{0};
    Scheduling::ITimerScheduler* timerScheduler_{nullptr};
    FW::Generation preparedGeneration_{0};
    AudioDuplexChannels duplexChannels_{};
    AudioClockConfig appliedClock_{.sampleRateHz = 48000};
    bool inputConnected_{false};
    bool outputConnected_{false};
};

} // namespace ASFW::Audio::BeBoB
