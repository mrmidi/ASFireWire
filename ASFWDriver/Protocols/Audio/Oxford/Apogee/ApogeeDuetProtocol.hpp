// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProtocol.hpp - Protocol implementation for Apogee Duet FireWire
// Reference: snd-firewire-ctl-services/protocols/oxfw/src/apogee.rs

#pragma once

#include "ApogeeTypes.hpp"
#include "../../IDeviceProtocol.hpp"
#include "../../../../Async/AsyncTypes.hpp"
#include <DriverKit/IOReturn.h>
#include <vector>
#include <functional>
#include <memory>

// Forward declaration
namespace ASFW::Async {
    class AsyncSubsystem;
}

namespace ASFW::Audio::Oxford::Apogee {

class ApogeeDuetProtocol : public IDeviceProtocol {
public:
    using VoidCallback = std::function<void(IOReturn)>;
    template<typename T> using ResultCallback = std::function<void(IOReturn, T)>;

    ApogeeDuetProtocol(Async::AsyncSubsystem& subsystem, uint16_t nodeId);
    virtual ~ApogeeDuetProtocol() = default;

    // IDeviceProtocol implementation
    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override { return "Apogee Duet FireWire"; }
    bool HasDsp() const override { return true; } // Has mixer/DSP features
    bool HasMixer() const override { return true; }

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

    // ========================================================================
    // Meters (Async)
    // ========================================================================

    void GetInputMeter(ResultCallback<InputMeterState> callback);
    void GetMixerMeter(ResultCallback<MixerMeterState> callback);

private:
    Async::AsyncSubsystem& subsystem_;
    uint16_t nodeId_;

    // Helpers
    void SendVendorCommand(const std::vector<uint8_t>& payload, bool isStatus, VoidCallback callback);
    void SendVendorCommand(const std::vector<uint8_t>& payload, bool isStatus, std::function<void(IOReturn, const std::vector<uint8_t>&)> callback);

    // FCP Constants
    static constexpr uint64_t kFCPCommandAddress = 0xFFFFF0000B00ULL;
    static constexpr uint64_t kMeterBaseAddress = 0xFFFFF0080000ULL;
    static constexpr uint32_t kMeterInputOffset = 0x0004;
    static constexpr uint32_t kMeterMixerOffset = 0x0404;

    // Apogee Constants
    static constexpr uint8_t kOUI[3] = {0x00, 0x03, 0xDB};
    static constexpr uint8_t kPrefix[3] = {0x50, 0x43, 0x4D}; // PCM
};

} // namespace ASFW::Audio::Oxford::Apogee
