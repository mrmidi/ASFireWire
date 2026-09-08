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
#include "ApogeeDuetSemanticTopology.hpp"
#include "ApogeeTypes.hpp"
#include "ApogeeVendorCodec.hpp"
#include "../OxfordCsr.hpp"
#include "../../IDeviceProtocol.hpp"
#include "../../Duplex/IDuplexDeviceControl.hpp"
#include "../../Configuration/IAudioConfigurationControl.hpp"
#include "../../../Shared/Controls/IAudioControlSurface.hpp"
#include "../../../Shared/Metering/IAudioMetering.hpp"
#include "../../../../Protocols/Ports/FireWireBusPort.hpp"
#include "../../../../Scheduling/ITimerScheduler.hpp"
#include <DriverKit/IOReturn.h>
#include <vector>
#include <functional>
#include <cstdint>
#include <span>
#include <memory>

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
                                 public IAudioSemanticTopology,
                                 public IAudioControlSurface,
                                 public IAudioMetering,
                                 public IAudioConfigurationControl {
public:
    // The command table and operand encoding moved to ApogeeVendorCodec (FW-126);
    // this alias keeps every existing ApogeeDuetProtocol::VendorCommand use valid.
    using VendorCommand = ApogeeVendorCommand;

    // Params serialization moved to ApogeeParamsSerdes (FW-128). It is pure and
    // static, so it is tested directly rather than through this class.

    using VoidCallback = std::function<void(IOReturn)>;
    template<typename T> using ResultCallback = std::function<void(IOReturn, T)>;

    /// `routeRegistry` is what the register reads resolve their route through.
    /// It must be supplied on any path that reads CSRs or meters: the two
    /// construction sites bind different subsets of the optional clients (the
    /// discovery prefetch has an FCP transport but no CMP/IRM client; the
    /// factory has CMP/IRM but no transport), so route resolution cannot hang
    /// off any of them. See FW-142.
    ApogeeDuetProtocol(Protocols::Ports::FireWireBusOps& busOps,
                       Protocols::Ports::FireWireBusInfo& busInfo,
                       Discovery::DeviceRouteToken route,
                       Discovery::DeviceRegistry* routeRegistry = nullptr,
                       Protocols::AVC::FCPTransport* fcpTransport = nullptr,
                       IRM::IRMClient* irmClient = nullptr,
                       CMP::CMPClient* cmpClient = nullptr,
                       uint32_t formatSettleDelayMs = 100U,
                       Scheduling::ITimerScheduler* timerScheduler = nullptr);
    ~ApogeeDuetProtocol() override;

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
    IAudioSemanticTopology* AsAudioSemanticTopology() noexcept override { return this; }
    const IAudioSemanticTopology* AsAudioSemanticTopology() const noexcept override { return this; }
    [[nodiscard]] bool CopyAudioSemanticTopology(
        AudioSemanticTopologySnapshot& outSnapshot) const noexcept override {
        return BuildApogeeDuetSemanticTopology(outSnapshot);
    }
    IAudioControlSurface* AsAudioControlSurface() noexcept override { return this; }
    const IAudioControlSurface* AsAudioControlSurface() const noexcept override { return this; }
    [[nodiscard]] bool CopyAudioControlSurfaceSnapshot(
        AudioControlSurfaceSnapshot& outSnapshot) const noexcept override;
    void ApplyAudioControlValue(uint32_t controlId, int32_t value,
                                IAudioControlSurface::ApplyCallback callback) override;
    IAudioMetering* AsAudioMetering() noexcept override { return this; }
    const IAudioMetering* AsAudioMetering() const noexcept override { return this; }
    [[nodiscard]] bool CopyAudioMeterSnapshot(
        AudioMeterSnapshot& outSnapshot) const noexcept override;
    [[nodiscard]] IOReturn SetAudioMeteringEnabled(bool enabled) noexcept override;
    IAudioConfigurationControl* AsAudioConfigurationControl() noexcept override { return this; }
    const IAudioConfigurationControl* AsAudioConfigurationControl() const noexcept override {
        return this;
    }
    [[nodiscard]] bool SupportsConfiguration(
        const Configuration::DeviceConfiguration& configuration) const noexcept override;
    void ApplyConfiguration(const Configuration::DeviceConfiguration& configuration,
                            IAudioConfigurationControl::ApplyCallback callback) override;
    [[nodiscard]] AudioConfigurationApplyResult
    CurrentConfiguration() const noexcept override;

    // IDeviceProtocol members the duplex controller answers. Kept here because
    // callers hold an IDeviceProtocol, not an IDuplexDeviceControl.
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override {
        return duplex_.GetRuntimeAudioStreamCaps(outCaps);
    }
    bool GetSupportedSampleRates(std::vector<uint32_t>& outRates) const override {
        // These are exactly the rate formations exposed by the Duet console.
        // Do not advertise 32 kHz: configuration rejects it and Core Audio
        // must never select a rate that the semantic control path cannot keep.
        outRates = {44100U, 48000U, 96000U};
        return true;
    }
    [[nodiscard]] IOReturn StopDuplex() override { return duplex_.StopDuplex(); }
    [[nodiscard]] IRM::IRMClient* GetIRMClient() const override { return runtime_.irmClient; }

    /// May be null before the runtime context is bound; callers must check.
    [[nodiscard]] Protocols::AVC::FCPTransport* GetFCPTransport() const noexcept {
        return runtime_.fcpTransport;
    }
    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              Protocols::AVC::FCPTransport* transport) override;

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

    // Hardware-owned input stereo link. This is a Duet global boolean, not a
    // host-side linked-fader preference.
    void GetMicsGrouped(ResultCallback<bool> callback);
    void SetMicsGrouped(bool enabled, VoidCallback callback);

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
    [[nodiscard]] Oxford::RouteProvider MakeRouteProvider() const;
    void RefreshSemanticControlState() noexcept;
    void ScheduleMeterRead(uint64_t delayNs, uint64_t epoch) noexcept;
    void PollMeter(uint64_t epoch) noexcept;
    void CompleteMeterRead(uint64_t epoch, IOReturn status,
                           const InputMeterState& input,
                           const MixerMeterState& mixer) noexcept;
    void ApplyPolledKnobState(uint64_t epoch, const KnobState& knob) noexcept;

    // Declaration order matters: duplex_ binds a reference to runtime_.
    DuetRuntime runtime_;
    ApogeeDuetDuplex duplex_;
    struct SemanticControlState;
    struct MeteringState;
    std::shared_ptr<SemanticControlState> semanticControlState_;
    std::shared_ptr<MeteringState> meteringState_;
};

} // namespace ASFW::Audio::Oxford::Apogee
