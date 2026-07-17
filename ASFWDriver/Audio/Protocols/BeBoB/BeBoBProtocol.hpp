// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBProtocol.hpp — Abstract BridgeCo BeBoB protocol base class.
//
// Owns the reusable BeBoB/CMP lifecycle: FCP transport dispatch, signal format
// programming, CMP connect/verify/teardown, plug management, and generation-bound
// epoch tracking. Device-specific behavior (stream geometry, supported rates, clock
// health, mixer configuration) flows through virtual hooks.
//
// Cross-validated with Linux sound/firewire/bebob/bebob_stream.c:96-115 (signal
// format), 400-465 (CIP blocking), 500-523 / 593-674 (CMP ordering), 602-606
// (stop). No reference source is copied.

#pragma once

#include "../Duplex/IDuplexDeviceControl.hpp"
#include "../IDeviceProtocol.hpp"
#include "../../../Protocols/Ports/FireWireBusPort.hpp"
#include "../../../Protocols/AVC/CMP/CMPClient.hpp"
#include "../../../Scheduling/ITimerScheduler.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace ASFW::Protocols::AVC {
class FCPTransport;
}

namespace ASFW::IRM {
class IRMClient;
}

#include "../../../Protocols/AVC/IAVCCommandSubmitter.hpp"

namespace ASFW::Audio::BeBoB {

class BeBoBProtocol : public IDeviceProtocol,
                      public IDuplexDeviceControl,
                      public Protocols::AVC::IAVCCommandSubmitter {
public:
    BeBoBProtocol(Protocols::Ports::FireWireBusOps& busOps,
                  Protocols::Ports::FireWireBusInfo& busInfo,
                  uint16_t nodeId,
                  IRM::IRMClient* irmClient,
                  CMP::CMPClient* cmpClient,
                  uint64_t deviceGuid,
                  Scheduling::ITimerScheduler* timerScheduler) noexcept;

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    IDuplexDeviceControl* AsDuplexDeviceControl() noexcept override { return this; }
    const IDuplexDeviceControl* AsDuplexDeviceControl() const noexcept override { return this; }
    void UpdateRuntimeContext(uint16_t nodeId,
                              Protocols::AVC::FCPTransport* transport) override;

    // IAVCCommandSubmitter
    void SubmitCommand(const Protocols::AVC::AVCCdb& cdb, Protocols::AVC::AVCCompletion completion) override;

    // IDuplexDeviceControl — general BeBoB lifecycle
    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const AudioClockConfig& desiredClock,
                       PrepareCallback callback) override;
    void SetAssignedChannels(const AudioDuplexChannels& channels) noexcept override;
    void ProgramRx(StageCallback callback) override;
    void ProgramTxAndEnableDuplex(StageCallback callback) override;
    void ConfirmDuplexStart(ConfirmCallback callback) override;
    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          ClockApplyCallback callback) override;
    void DisconnectPlayback(VoidCallback callback) override;
    void DisconnectCapture(VoidCallback callback) override;
    void BreakBothConnections(VoidCallback callback) override;
    [[nodiscard]] IOReturn StopDuplex() override;
    [[nodiscard]] IRM::IRMClient* GetIRMClient() const override { return irmClient_; }

protected:
    // Device-specific overrides (pure virtual).
    [[nodiscard]] virtual const char* DeviceName() const = 0;
    [[nodiscard]] virtual AudioStreamRuntimeCaps DeviceCaps() const = 0;
    [[nodiscard]] virtual std::vector<uint32_t> SupportedRates() const = 0;
    virtual void ReadClockHealth(HealthCallback callback) = 0;

    // Async mixer configuration. Override in devices that need FB mixer programming
    // at stream start (e.g. Phase88 ships muted). Default: no-op (matches Linux
    // bebob_stream.c behavior — no mixer programming at start).
    using MixerCompletion = std::function<void(IOReturn)>;
    enum class MixerFailurePolicy { kRequired, kBestEffort };
    virtual void ConfigureMixer(MixerFailurePolicy policy, MixerCompletion completion);

    // FB framework helpers — async FCP operations for subclasses.
    void SetSelectorBlock(uint8_t fbId, uint8_t value, MixerCompletion completion);
    void SetFeatureMute(uint8_t fbId, uint8_t channel, bool unmute, MixerCompletion completion);
    void SetFeatureVolume(uint8_t fbId, uint8_t channel, uint16_t value, MixerCompletion completion);

    // Signal-format-to-plug hook. Default returns plug 0; override for devices with
    // non-zero stream plugs.
    [[nodiscard]] virtual uint8_t StreamPlug(bool isInput) const { return 0; }

    // Rate validation. Default rejects anything not in SupportedRates(); override for
    // devices with format-set logic.
    [[nodiscard]] virtual bool IsRateSupported(uint32_t hz) const;

    // Exactly-once completion guard for the async ApplyClockConfig chain.
    struct ClockApplyEpoch {
        FW::Generation generation{FW::Generation{0}};
        Scheduling::TimerToken settleTimer{Scheduling::kInvalidTimerToken};
        std::atomic<bool> completed{false};
        ClockApplyCallback completion;
        IOReturn status{kIOReturnSuccess};
        AudioClockConfig appliedClock{.sampleRateHz = 0};
    };

    void FinishClockApply(ClockApplyEpoch* epoch, IOReturn status);
    void CancelClockApply();

    [[nodiscard]] CMP::CMPDevice CurrentCMPDevice() const noexcept;
    [[nodiscard]] IOReturn ResetEpochIfNeeded() noexcept;

    Protocols::Ports::FireWireBusInfo& busInfo_;
    uint16_t nodeId_{0};
    IRM::IRMClient* irmClient_{nullptr};
    CMP::CMPClient* cmpClient_{nullptr};
    Protocols::AVC::FCPTransport* fcpTransport_{nullptr};
    uint64_t deviceGuid_{0};
    Scheduling::ITimerScheduler* timerScheduler_{nullptr};
    FW::Generation preparedGeneration_{0};
    AudioDuplexChannels duplexChannels_{};
    AudioClockConfig appliedClock_{.sampleRateHz = 0};
    bool inputConnected_{false};
    bool outputConnected_{false};

    // In-flight clock-apply epoch. Non-nullptr while ApplyClockConfig is settling.
    ClockApplyEpoch* activeClockApply_{nullptr};

private:
    void EnsurePlugFree(CMP::PCRDirection dir, uint8_t plug, std::function<void(IOReturn)> cb);
    void ProgramSignalFormat(const AudioClockConfig& desiredClock,
                             std::function<void(IOReturn)> completion);

    static constexpr uint32_t kFormatSettleMs = 300;
};

} // namespace ASFW::Audio::BeBoB
