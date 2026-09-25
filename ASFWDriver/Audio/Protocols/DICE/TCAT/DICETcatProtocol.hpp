// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DICETcatProtocol.hpp - Generic DICE/TCAT protocol state and duplex control

#pragma once

#include "../../Duplex/FamilyDriver.hpp"
#include "../Core/DiceDeviceIo.hpp"
#include "../Core/DiceFamilyDriver.hpp"
#include "../Core/DiceNotificationMailbox.hpp"
#include "../Core/DiceNotificationRouter.hpp"
#include "../Core/DiceWaitClock.hpp"
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

// Separates DICE's operational stream topology from the channels the HAL
// publishes. Some hardware needs both DICE directions active for its clock or
// firmware protocol while exposing only one analog/audio direction to users.
struct DICETcatRuntimePolicy final {
    bool exposeDeviceToHostToCoreAudio{true};
    // Keep the target-rate transition strict, but do not require a GLOBAL
    // source-lock indication before/just after host IT starts. This is for
    // devices whose selected receive-clock path only locks once host packets
    // are flowing; it does not select ARX1 as a clock source.
    bool requireSourceLockBeforeStreamEnable{true};
    bool requireSourceLockAtConfirm{true};
};

class DICETcatProtocol final : public Audio::IDeviceProtocol,
                               public Audio::FamilyDriver {
public:
    using VoidCallback = std::function<void(IOReturn)>;

    DICETcatProtocol(Protocols::Ports::FireWireBusOps& busOps,
                     Protocols::Ports::FireWireBusInfo& busInfo,
                     Discovery::DeviceRegistry& routeRegistry,
                     const Discovery::DeviceRouteToken& route,
                     ::ASFW::IRM::IRMClient* irmClient,
                     DiceWaitClock& waitClock,
                     DiceNotificationRouter* notifications,
                     DICETcatRuntimePolicy runtimePolicy = {});
    ~DICETcatProtocol() override;

    DICETcatProtocol(const DICETcatProtocol&) = delete;
    DICETcatProtocol& operator=(const DICETcatProtocol&) = delete;

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override { return "TCAT DICE"; }
    Audio::FamilyDriver* AsFamilyDriver() noexcept override { return this; }

    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    bool GetChannelLabels(std::vector<std::string>& inNames,
                          std::vector<std::string>& outNames) const override;

    // The GLOBAL clock and lock state, read asynchronously (safe on any queue).
    void ReadDuplexHealth(HealthCallback callback);
    void EnsureRuntimeStreamGeometry(VoidCallback callback) override;
    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept override;

    IOReturn StopDuplex() override;

    // FamilyDriver. The stages run DiceFamilyDriver synchronously; geometry and
    // health await the same asynchronous reads the Default-queue publication
    // path uses.
    [[nodiscard]] IOReturn LoadGeometry() override;
    [[nodiscard]] std::optional<AudioStreamRuntimeCaps> RuntimeCaps() const override;
    [[nodiscard]] std::expected<DuplexPrepareResult, IOReturn> Configure(
        const AudioDuplexChannels& channels, const AudioClockConfig& clock) override;
    void AssignChannels(const AudioDuplexChannels& channels) override;
    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t timeoutMs) override;
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceRx() override;
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceTxAndEnable() override;
    [[nodiscard]] std::expected<DuplexConfirmResult, IOReturn> Confirm() override;
    [[nodiscard]] std::expected<DuplexClockApplyResult, IOReturn> ApplyClockIdle(
        const AudioClockConfig& clock) override;
    [[nodiscard]] IOReturn DisconnectPlayback() override;
    [[nodiscard]] IOReturn DisconnectCapture() override;
    [[nodiscard]] IOReturn BreakConnections() override;
    [[nodiscard]] IOReturn Stop() override;

    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
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
    [[nodiscard]] bool DeviceSupportsRate(uint32_t rateHz) const noexcept;

    Protocols::Ports::FireWireBusInfo& busInfo_;
    ::ASFW::IRM::IRMClient* irmClient_{nullptr};
    Protocols::Ports::ProtocolRegisterIO io_;
    DICETransaction diceReader_;
    DiceDeviceIo deviceIo_;
    // This device's notification bits. Registered with the router for the
    // protocol's lifetime; null router: no notification reaches it (tests).
    DiceNotificationMailbox notifications_;
    DiceNotificationRouter* notificationRouter_{nullptr};
    uint64_t guid_{0};
    std::optional<DiceFamilyDriver> driver_;
    const std::atomic<bool>* teardownCancel_{nullptr};
    DICETcatRuntimePolicy runtimePolicy_{};
    GeneralSections sections_{};
    bool initialized_{false};
    bool sectionsLoaded_{false};

    // The user-selected device clock, remembered across StartIO cycles so the
    // per-StartIO bring-up (PrepareDuplex48k) targets the live rate instead of a
    // hardcoded 48 kHz. Updated whenever a real clock is applied (ApplyClockConfig
    // for idle rate changes, PrepareDuplex for restarts). Default {0} means
    // "nothing selected yet" → PrepareDuplex48k falls back to 48 kHz. Without this
    // every StartIO rewrites CLOCK_SELECT back to 48 kHz and fights a 44.1 kHz
    // selection, flapping the device PLL and starving audio.
    AudioClockConfig selectedClock_{};

    std::atomic<uint32_t> runtimeSampleRateHz_{0};
    std::atomic<uint32_t> deviceRateMask_{0};
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
