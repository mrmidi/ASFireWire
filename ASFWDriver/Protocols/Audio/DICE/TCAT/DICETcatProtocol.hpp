// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DICETcatProtocol.hpp - Generic DICE/TCAT protocol state and duplex control

#pragma once

#include "../Core/IDICEDuplexProtocol.hpp"
#include "../Core/DICEDuplexBringupController.hpp"
#include "../Core/DICETransaction.hpp"
#include "../Core/DICETypes.hpp"
#include "../../IDeviceProtocol.hpp"
#include "../../../Ports/ProtocolRegisterIO.hpp"

#include <atomic>
#include <functional>
#include <optional>

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Audio::DICE::TCAT {

class DICETcatProtocol final : public Audio::IDeviceProtocol,
                               public Audio::DICE::IDICEDuplexProtocol {
public:
    using VoidCallback = std::function<void(IOReturn)>;
    using PrepareCallback = IDICEDuplexProtocol::PrepareCallback;
    using StageCallback = IDICEDuplexProtocol::StageCallback;
    using ConfirmCallback = IDICEDuplexProtocol::ConfirmCallback;
    using ClockApplyCallback = IDICEDuplexProtocol::ClockApplyCallback;
    using HealthCallback = IDICEDuplexProtocol::HealthCallback;

    DICETcatProtocol(Protocols::Ports::FireWireBusOps& busOps,
                     Protocols::Ports::FireWireBusInfo& busInfo,
                     uint16_t nodeId,
                     ::ASFW::IRM::IRMClient* irmClient = nullptr);

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override { return "TCAT DICE"; }
    Audio::DICE::IDICEDuplexProtocol* AsDiceDuplexProtocol() noexcept override { return this; }
    const Audio::DICE::IDICEDuplexProtocol* AsDiceDuplexProtocol() const noexcept override { return this; }

    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    void RefreshRuntimeAudioStreamCaps(VoidCallback callback) override;

    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const DiceDesiredClockConfig& desiredClock,
                       PrepareCallback callback) override;
    void ProgramRx(StageCallback callback) override;
    void ProgramTxAndEnableDuplex(StageCallback callback) override;
    void ConfirmDuplexStart(ConfirmCallback callback) override;
    void ApplyClockConfig(const DiceDesiredClockConfig& desiredClock,
                          ClockApplyCallback callback) override;
    void ReadDuplexHealth(HealthCallback callback) override;
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
    GeneralSections sections_{};
    bool initialized_{false};
    bool sectionsLoaded_{false};

    std::atomic<uint32_t> runtimeSampleRateHz_{0};
    std::atomic<uint32_t> hostInputPcmChannels_{0};
    std::atomic<uint32_t> hostOutputPcmChannels_{0};
    std::atomic<uint32_t> deviceToHostAm824Slots_{0};
    std::atomic<uint32_t> hostToDeviceAm824Slots_{0};
    std::atomic<uint32_t> deviceToHostActiveStreams_{0};
    std::atomic<uint32_t> hostToDeviceActiveStreams_{0};
    std::atomic<uint32_t> deviceToHostIsoChannel_{AudioStreamRuntimeCaps::kInvalidIsoChannel};
    std::atomic<uint32_t> hostToDeviceIsoChannel_{AudioStreamRuntimeCaps::kInvalidIsoChannel};
    std::atomic<bool> runtimeCapsValid_{false};
};

} // namespace ASFW::Audio::DICE::TCAT
