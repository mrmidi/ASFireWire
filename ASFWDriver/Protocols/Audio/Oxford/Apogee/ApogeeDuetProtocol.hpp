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
#include <cstdint>

// Forward declaration
namespace ASFW::Async {
    class AsyncSubsystem;
}

namespace ASFW::Protocols::AVC {
    class FCPTransport;
}

namespace ASFW::Audio::Oxford::Apogee {

class ApogeeDuetProtocol : public IDeviceProtocol {
public:
    using VoidCallback = std::function<void(IOReturn)>;
    template<typename T> using ResultCallback = std::function<void(IOReturn, T)>;

    ApogeeDuetProtocol(Async::AsyncSubsystem& subsystem,
                       uint16_t nodeId,
                       Protocols::AVC::FCPTransport* fcpTransport = nullptr);
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
    void ClearDisplay(VoidCallback callback);

    // ========================================================================
    // Meters (Async)
    // ========================================================================

    void GetInputMeter(ResultCallback<InputMeterState> callback);
    void GetMixerMeter(ResultCallback<MixerMeterState> callback);

    // ========================================================================
    // Oxford ID Registers (Async)
    // ========================================================================

    void GetFirmwareId(ResultCallback<uint32_t> callback);
    void GetHardwareId(ResultCallback<uint32_t> callback);

    // Oxford hardware identifiers (from snd-firewire-ctl-services/oxford.rs).
    static constexpr uint32_t kHardwareIdFw970 = 0x39443841; // '9''D''8''A'
    static constexpr uint32_t kHardwareIdFw971 = 0x39373100; // '9''7''1''\0'

    // Runtime integration hook. Not wired by factory yet.
    void SetFCPTransport(Protocols::AVC::FCPTransport* fcpTransport) noexcept {
        fcpTransport_ = fcpTransport;
    }

    // IDeviceProtocol boolean control overrides
    void UpdateRuntimeContext(uint16_t nodeId,
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
    struct VendorCommand;

    Async::AsyncSubsystem& subsystem_;
    uint16_t nodeId_;
    Protocols::AVC::FCPTransport* fcpTransport_{nullptr};

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

    static uint32_t ReadQuadletBE(const uint8_t* data) noexcept;

    // Meter and Oxford CSR constants.
    static constexpr uint64_t kMeterBaseAddress = 0xFFFFF0080000ULL;
    static constexpr uint32_t kMeterInputOffset = 0x0004;
    static constexpr uint32_t kMeterMixerOffset = 0x0404;
    static constexpr uint64_t kOxfordCsrBase = 0xFFFFF0000000ULL;
    static constexpr uint64_t kOxfordFirmwareIdOffset = 0x50000ULL;
    static constexpr uint64_t kOxfordHardwareIdOffset = 0x90020ULL;

    // Apogee Constants
    static constexpr uint8_t kOUI[3] = {0x00, 0x03, 0xDB};
    static constexpr uint8_t kPrefix[3] = {0x50, 0x43, 0x4D}; // PCM
};

} // namespace ASFW::Audio::Oxford::Apogee
