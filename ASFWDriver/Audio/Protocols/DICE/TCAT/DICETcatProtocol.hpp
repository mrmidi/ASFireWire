// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DICETcatProtocol.hpp - Generic DICE/TCAT protocol state and duplex control

#pragma once

#include "../../Duplex/IDuplexDeviceControl.hpp"
#include "../Core/DICEDuplexBringupController.hpp"
#include "../Core/DICETransaction.hpp"
#include "../Core/DICETypes.hpp"
#include "../../IDeviceProtocol.hpp"
#include "../../../../Protocols/Ports/ProtocolRegisterIO.hpp"

#include <atomic>
#include <functional>
#include <optional>

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Audio::DICE::TCAT {

class DICETcatProtocol final : public Audio::IDeviceProtocol,
                               public Audio::IDuplexDeviceControl {
public:
    using VoidCallback = std::function<void(IOReturn)>;
    using PrepareCallback = IDuplexDeviceControl::PrepareCallback;
    using StageCallback = IDuplexDeviceControl::StageCallback;
    using ConfirmCallback = IDuplexDeviceControl::ConfirmCallback;
    using ClockApplyCallback = IDuplexDeviceControl::ClockApplyCallback;
    using HealthCallback = IDuplexDeviceControl::HealthCallback;

    DICETcatProtocol(Protocols::Ports::FireWireBusOps& busOps,
                     Protocols::Ports::FireWireBusInfo& busInfo,
                     uint16_t nodeId,
                     ::ASFW::IRM::IRMClient* irmClient = nullptr);

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override { return "TCAT DICE"; }
    Audio::IDuplexDeviceControl* AsDuplexDeviceControl() noexcept override { return this; }
    const Audio::IDuplexDeviceControl* AsDuplexDeviceControl() const noexcept override { return this; }

    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    bool GetChannelLabels(std::vector<std::string>& inNames,
                          std::vector<std::string>& outNames) const override;

    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const AudioClockConfig& desiredClock,
                       PrepareCallback callback) override;
    void ProgramRx(StageCallback callback) override;
    void ProgramTxAndEnableDuplex(StageCallback callback) override;
    void ConfirmDuplexStart(ConfirmCallback callback) override;
    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          ClockApplyCallback callback) override;
    void ReadDuplexHealth(HealthCallback callback) override;
    void EnsureRuntimeStreamGeometry(VoidCallback callback) override;
    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept override;
    ::ASFW::IRM::IRMClient* GetIRMClient() const override { return irmClient_; }

    void PrepareDuplex48k(const AudioDuplexChannels& channels, VoidCallback callback) override;
    void ProgramRxForDuplex48k(VoidCallback callback) override;
    void ProgramTxAndEnableDuplex48k(VoidCallback callback) override;
    void ConfirmDuplex48kStart(VoidCallback callback) override;
    IOReturn StopDuplex() override;
    void UpdateRuntimeContext(uint16_t nodeId,
                              Protocols::AVC::FCPTransport* transport) override;

    [[nodiscard]] Protocols::Ports::ProtocolRegisterIO& IO() noexcept { return io_; }
    [[nodiscard]] DICETransaction& Transaction() noexcept { return diceReader_; }

private:
    friend class DICETcatProtocolTestPeer;

    [[nodiscard]] static bool MakeDiceClockConfiguration(
        const AudioClockConfig& requested,
        DiceClockConfiguration& out) noexcept;
    void EnsureSectionsLoaded(VoidCallback callback);
    void EnsureRuntimeCapsLoaded(VoidCallback callback);
    void CacheRuntimeCaps(const GlobalState& global,
                          const StreamConfig& tx,
                          const StreamConfig& rx) noexcept;
    void CacheRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept;
    void ResetRuntimeCaps() noexcept;

    Protocols::Ports::FireWireBusInfo& busInfo_;
    ::ASFW::IRM::IRMClient* irmClient_{nullptr};
    Protocols::Ports::ProtocolRegisterIO io_;
    DICETransaction diceReader_;
    std::optional<ASFW::Audio::DICE::DICEDuplexBringupController> duplexCtrl_;
    const std::atomic<bool>* teardownCancel_{nullptr};
    GeneralSections sections_{};
    bool initialized_{false};
    bool sectionsLoaded_{false};

    std::atomic<uint32_t> runtimeSampleRateHz_{0};
    std::atomic<uint32_t> hostInputPcmChannels_{0};
    std::atomic<uint32_t> hostOutputPcmChannels_{0};
    std::atomic<uint32_t> deviceToHostAm824Slots_{0};
    std::atomic<uint32_t> hostToDeviceAm824Slots_{0};
    std::atomic<uint32_t> deviceToHostIsoChannel_{AudioStreamRuntimeCaps::kInvalidIsoChannel};
    std::atomic<uint32_t> hostToDeviceIsoChannel_{AudioStreamRuntimeCaps::kInvalidIsoChannel};

    // Per-stream wire geometry (DICE TX_NUMBER/RX_NUMBER + per-stream channels).
    // Counts are atomic; the arrays are plain and published through the
    // runtimeCapsValid_ release/acquire fence (written before the release-store,
    // read after the acquire-load), mirroring the scalar fields above.
    std::atomic<uint32_t> deviceToHostStreamCount_{0};
    std::atomic<uint32_t> hostToDeviceStreamCount_{0};
    AudioStreamWireInfo deviceToHostStreams_[kMaxAudioStreamsPerDirection]{};
    AudioStreamWireInfo hostToDeviceStreams_[kMaxAudioStreamsPerDirection]{};

    // Per-channel device labels, flattened across this direction's streams in
    // channel order (input == device TX, output == device RX). Published
    // through the runtimeCapsValid_ release/acquire fence like the arrays above;
    // only the (global, tx, rx) cache path fills them (the caps-only overload
    // leaves them intact). Covers the widest supported interface (32x32).
    static constexpr uint32_t kMaxChannelLabels = 32;
    std::atomic<uint32_t> inputChannelLabelCount_{0};
    std::atomic<uint32_t> outputChannelLabelCount_{0};
    char inputChannelLabels_[kMaxChannelLabels][64]{};
    char outputChannelLabels_[kMaxChannelLabels][64]{};

    std::atomic<bool> runtimeCapsValid_{false};
};

} // namespace ASFW::Audio::DICE::TCAT
