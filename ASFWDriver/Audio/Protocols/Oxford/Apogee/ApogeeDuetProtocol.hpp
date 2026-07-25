// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProtocol.hpp - Protocol implementation for Apogee Duet FireWire
// Reference: snd-firewire-ctl-services/protocols/oxfw/src/apogee.rs
//
// What is left here after the FW-124 decomposition is composition plus the
// device's own control surface:
//
//   ApogeeVendorCodec     command table and operand encoding   (FW-126)
//   ApogeeTransport       FCP dispatch and meter register IO   (FW-129)
//   ApogeeParamsSerdes    parameter serialization              (FW-128)
//   OxfordCsr             chip-common FW970/971 ID registers   (FW-137)
//   ApogeeDuetDuplex      duplex lifecycle and clock FSM       (FW-127)
//
// The duplex controller is a member rather than a base class, so this type no
// longer carries the IDuplexDeviceControl vtable; AsDuplexDeviceControl hands
// out the member. The IDeviceProtocol methods that overlap the duplex surface
// stay here as forwarders, because callers reach them through IDeviceProtocol.

#pragma once

#include "ApogeeDuetDuplex.hpp"
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

class ApogeeDuetProtocol final : public IDeviceProtocol {
public:
    // The command table and operand encoding moved to ApogeeVendorCodec (FW-126);
    // this alias keeps every existing ApogeeDuetProtocol::VendorCommand use valid.
    using VendorCommand = ApogeeVendorCommand;

    // Params serialization moved to ApogeeParamsSerdes (FW-128). It is pure and
    // static, so it is tested directly rather than through this class.

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
    ~ApogeeDuetProtocol() override = default;

    // IDeviceProtocol implementation
    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    [[nodiscard]] const char* GetName() const override { return "Apogee Duet FireWire"; }
    [[nodiscard]] bool HasDsp() const override { return true; } // Has mixer/DSP features
    [[nodiscard]] bool HasMixer() const override { return true; }
    IDuplexDeviceControl* AsDuplexDeviceControl() noexcept override { return &duplex_; }
    [[nodiscard]] const IDuplexDeviceControl* AsDuplexDeviceControl() const noexcept override {
        return &duplex_;
    }

    // IDeviceProtocol members the duplex controller answers. Kept here because
    // callers hold an IDeviceProtocol, not an IDuplexDeviceControl.
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override {
        return duplex_.GetRuntimeAudioStreamCaps(outCaps);
    }
    [[nodiscard]] IOReturn StopDuplex() override { return duplex_.StopDuplex(); }
    [[nodiscard]] IRM::IRMClient* GetIRMClient() const override { return runtime_.irmClient; }
    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              Protocols::AVC::FCPTransport* transport) override {
        duplex_.UpdateRuntimeContext(route, transport);
    }

    /// Discovery applies the 48 kHz formation before publishing, holding the
    /// concrete type rather than the duplex seam.
    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          IDuplexDeviceControl::ClockApplyCallback callback) {
        duplex_.ApplyClockConfig(desiredClock, std::move(callback));
    }

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

private:
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

    /// Route-liveness policy handed to the chip-common CSR reads.
    [[nodiscard]] Oxford::RouteValidator MakeRouteValidator() const;

    // Declaration order matters: duplex_ binds a reference to runtime_.
    DuetRuntime runtime_;
    ApogeeDuetDuplex duplex_;
};

} // namespace ASFW::Audio::Oxford::Apogee
