// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProtocol.hpp - Protocol implementation for Apogee Duet FireWire
// Reference: snd-firewire-ctl-services/protocols/oxfw/src/apogee.rs

#pragma once

#include "ApogeeTypes.hpp"
#include "../../IDeviceProtocol.hpp"
#include "../../Duplex/IDuplexDeviceControl.hpp"
#include "../../../../Protocols/Ports/FireWireBusPort.hpp"
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
}

namespace ASFW::Audio::Oxford::Apogee {

class ApogeeDuetProtocol final : public IDeviceProtocol,
                                 public IDuplexDeviceControl {
public:
    struct VendorCommand {
        enum class Code : uint8_t {
            MicPolarity = 0x00,
            XlrIsMicLevel = 0x01,
            XlrIsConsumerLevel = 0x02,
            MicPhantom = 0x03,
            OutIsConsumerLevel = 0x04,
            InGain = 0x05,
            HwState = 0x07,
            OutMute = 0x09,
            InputSourceIsPhone = 0x0C,
            MixerSrc = 0x10,
            OutSourceIsMixer = 0x11,
            DisplayOverholdTwoSec = 0x13,
            DisplayClear = 0x14,
            OutVolume = 0x15,
            MuteForLineOut = 0x16,
            MuteForHpOut = 0x17,
            UnmuteForLineOut = 0x18,
            UnmuteForHpOut = 0x19,
            DisplayIsInput = 0x1B,
            InClickless = 0x1E,
            DisplayFollowToKnob = 0x22,
        };

        Code code{};
        uint8_t index{0};
        uint8_t index2{0};
        bool boolValue{false};
        uint8_t u8Value{0};
        uint16_t u16Value{0};
        std::array<uint8_t, 11> hwState{};

        static VendorCommand Bool(Code code, bool value);
        static VendorCommand IndexedBool(Code code, uint8_t index, bool value);
        static VendorCommand InGain(uint8_t index, uint8_t value);
        static VendorCommand OutVolume(uint8_t value);
        static VendorCommand MixerSrc(uint8_t source, uint8_t destination, uint16_t gain);
        static VendorCommand HwState(const std::array<uint8_t, 11>& raw);
        static VendorCommand Make(Code code);

        [[nodiscard]] std::vector<uint8_t> BuildOperandBase() const;
        void AppendControlValue(std::vector<uint8_t>& operands) const;
        [[nodiscard]] bool ParseStatusPayload(std::span<const uint8_t> payload);
    };

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
                       uint16_t nodeId,
                       Protocols::AVC::FCPTransport* fcpTransport = nullptr,
                       IRM::IRMClient* irmClient = nullptr,
                       CMP::CMPClient* cmpClient = nullptr);
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
    Protocols::Ports::FireWireBusOps& busOps_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
    uint16_t nodeId_;
    Protocols::AVC::FCPTransport* fcpTransport_{nullptr};
    IRM::IRMClient* irmClient_{nullptr};
    CMP::CMPClient* cmpClient_{nullptr};
    AudioDuplexChannels duplexChannels_{};
    AudioClockConfig appliedClock_{};
    bool outputConnected_{false};
    bool inputConnected_{false};

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
