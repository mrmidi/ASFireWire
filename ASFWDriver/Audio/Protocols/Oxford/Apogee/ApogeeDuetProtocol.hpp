// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProtocol.hpp - Protocol implementation for Apogee Duet FireWire
// Reference: snd-firewire-ctl-services/protocols/oxfw/src/apogee.rs

#pragma once

#include "ApogeeTypes.hpp"
#include "ApogeeVendorCodec.hpp"
#include "../OxfordCsr.hpp"
#include "../../IDeviceProtocol.hpp"
#include "../../Duplex/IDuplexDeviceControl.hpp"
#include "../../../../Protocols/Ports/FireWireBusPort.hpp"
#include "../../../../Scheduling/ITimerScheduler.hpp"
#include <DriverKit/IOReturn.h>
#include <vector>
#include <functional>
#include <cstdint>
#include <memory>
#include <span>

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

class ApogeeDuetProtocol final : public IDeviceProtocol,
                                 public IDuplexDeviceControl {
public:
    // The command table and operand encoding moved to ApogeeVendorCodec (FW-126);
    // this alias keeps every existing ApogeeDuetProtocol::VendorCommand use valid.
    using VendorCommand = ApogeeVendorCommand;

    static std::vector<VendorCommand> BuildKnobStateQuery();
    static VendorCommand BuildKnobStateControl(const KnobState& state);
    static KnobState ParseKnobState(const VendorCommand& command);

    static std::vector<VendorCommand> BuildOutputParamsQuery();
    static std::vector<VendorCommand> BuildOutputParamsControl(const OutputParams& params);
    static OutputParams ParseOutputParams(const std::vector<VendorCommand>& commands);

    static std::vector<VendorCommand> BuildInputParamsQuery();
    static std::vector<VendorCommand> BuildInputParamsControl(const InputParams& params);
    static InputParams ParseInputParams(const std::vector<VendorCommand>& commands);

    static std::vector<VendorCommand> BuildMixerParamsQuery();
    static std::vector<VendorCommand> BuildMixerParamsControl(const MixerParams& params);
    static MixerParams ParseMixerParams(const std::vector<VendorCommand>& commands);

    static std::vector<VendorCommand> BuildDisplayParamsQuery();
    static std::vector<VendorCommand> BuildDisplayParamsControl(const DisplayParams& params);
    static DisplayParams ParseDisplayParams(const std::vector<VendorCommand>& commands);

    using VoidCallback = std::function<void(IOReturn)>;
    template<typename T> using ResultCallback = std::function<void(IOReturn, T)>;

    ApogeeDuetProtocol(Protocols::Ports::FireWireBusOps& busOps,
                       Protocols::Ports::FireWireBusInfo& busInfo,
                       Discovery::DeviceRouteToken route,
                       Protocols::AVC::FCPTransport* fcpTransport = nullptr,
                       IRM::IRMClient* irmClient = nullptr,
                       CMP::CMPClient* cmpClient = nullptr,
                       uint32_t formatSettleDelayMs = 100U,
                       Scheduling::ITimerScheduler* timerScheduler = nullptr);
    virtual ~ApogeeDuetProtocol() = default;

    // IDeviceProtocol implementation
    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override { return "Apogee Duet FireWire"; }
    bool HasDsp() const override { return true; } // Has mixer/DSP features
    bool HasMixer() const override { return true; }
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    IDuplexDeviceControl* AsDuplexDeviceControl() noexcept override { return this; }
    const IDuplexDeviceControl* AsDuplexDeviceControl() const noexcept override { return this; }

    // IDuplexDeviceControl: the generic lifecycle owns IRM + host isoch;
    // this adapter owns only AV/C signal-format and CMP/PCR operations.
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
    [[nodiscard]] IRM::IRMClient* GetIRMClient() const override { return irmClient_; }

    // ========================================================================
    // Parameter Access (Async)
    // ========================================================================

    // Knob Parameters
    void GetKnobState(ResultCallback<KnobState> callback);
    void SetKnobState(const KnobState& state, VoidCallback callback);

    // Output Parameters
    void GetOutputParams(ResultCallback<OutputParams> callback);
    void SetOutputParams(const OutputParams& params, VoidCallback callback);

    // Input Parameters
    void GetInputParams(ResultCallback<InputParams> callback);
    void SetInputParams(const InputParams& params, VoidCallback callback);

    // Mixer Parameters
    void GetMixerParams(ResultCallback<MixerParams> callback);
    void SetMixerParams(const MixerParams& params, VoidCallback callback);

    // Display Parameters
    void GetDisplayParams(ResultCallback<DisplayParams> callback);
    void SetDisplayParams(const DisplayParams& params, VoidCallback callback);
    void ClearDisplay(VoidCallback callback);

    // ========================================================================
    // Meters (Async)
    // ========================================================================

    void GetInputMeter(ResultCallback<InputMeterState> callback);
    void GetMixerMeter(ResultCallback<MixerMeterState> callback);

    // ========================================================================
    // Oxford ID Registers (Async)
    // ========================================================================

    // Register map, decode and ASIC classification are chip-common and live in
    // Oxford/OxfordCsr.hpp (FW-137). These remain as the Duet's entry points.
    void GetFirmwareId(ResultCallback<uint32_t> callback);
    void GetHardwareId(ResultCallback<uint32_t> callback);

    // Runtime integration hook. Not wired by factory yet.
    void SetFCPTransport(Protocols::AVC::FCPTransport* fcpTransport) noexcept {
        // The cache describes commands accepted through one transport epoch;
        // a replacement transport must re-establish the device formation.
        if (fcpTransport_ != fcpTransport) {
            clockConfigApplied_ = false;
        }
        fcpTransport_ = fcpTransport;
    }

    // IDeviceProtocol boolean control overrides
    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              Protocols::AVC::FCPTransport* transport) override;
    bool SupportsBooleanControl(uint32_t classIdFourCC,
                                uint32_t element) const override;
    IOReturn GetBooleanControlValue(uint32_t classIdFourCC,
                                    uint32_t element,
                                    bool& outValue) override;
    IOReturn SetBooleanControlValue(uint32_t classIdFourCC,
                                    uint32_t element,
                                    bool value) override;

    // Mapping helper for tests and call sites.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static bool TryMapBooleanControl(uint32_t classIdFourCC,
                                     uint32_t element,
                                     uint8_t& outChannelIndex) noexcept {
        const bool supportedClass =
            (classIdFourCC == static_cast<uint32_t>('phan')) ||
            (classIdFourCC == static_cast<uint32_t>('phsi'));
        if (!supportedClass) {
            return false;
        }
        if (element == 1u) {
            outChannelIndex = 0u;
            return true;
        }
        if (element == 2u) {
            outChannelIndex = 1u;
            return true;
        }
        return false;
    }

private:
    struct ClockTransition;

    Protocols::Ports::FireWireBusOps& busOps_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
    Discovery::DeviceRouteToken route_{};
    Protocols::AVC::FCPTransport* fcpTransport_{nullptr};
    IRM::IRMClient* irmClient_{nullptr};
    CMP::CMPClient* cmpClient_{nullptr};
    AudioDuplexChannels duplexChannels_{};
    AudioClockConfig appliedClock_{};
    uint64_t preparedRouteEpoch_{0};
    bool clockConfigApplied_{false};
    bool outputConnected_{false};
    bool inputConnected_{false};
    uint32_t formatSettleDelayMs_{100U};
    Scheduling::ITimerScheduler* timerScheduler_{nullptr};

    std::shared_ptr<ClockTransition> activeClockTransition_{nullptr};
    uint64_t activeClockTransitionEpoch_{0};
    uint64_t nextClockTransitionEpoch_{0};

    [[nodiscard]] bool IsActive(const ClockTransition& transition) const noexcept;

    // Helpers
    using VendorResultCallback = std::function<void(IOReturn, const VendorCommand&)>;
    using VendorSequenceCallback =
        std::function<void(IOReturn, const std::vector<VendorCommand>&)>;

    void SendVendorCommand(const VendorCommand& command,
                           bool isStatus,
                           VendorResultCallback callback);
    void ExecuteVendorSequence(const std::vector<VendorCommand>& commands,
                               bool isStatus,
                               VendorSequenceCallback callback);

    [[nodiscard]] CMP::CMPDevice CurrentCMPDevice() const noexcept;

    void AdvanceClockTransition(const std::shared_ptr<ClockTransition>& transition);
    void FailClockTransition(const std::shared_ptr<ClockTransition>& transition,
                             IOReturn status);
    void CancelClockTransition(IOReturn status);
    void CompleteClockTransition(const std::shared_ptr<ClockTransition>& transition,
                                 IOReturn status);
    void FinishClockTransition(const std::shared_ptr<ClockTransition>& transition,
                                IOReturn status);

    /// Route-liveness policy handed to the chip-common CSR reads.
    [[nodiscard]] Oxford::RouteValidator MakeRouteValidator() const;

};

} // namespace ASFW::Audio::Oxford::Apogee
