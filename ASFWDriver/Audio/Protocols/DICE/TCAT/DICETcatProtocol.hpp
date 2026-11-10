// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DICETcatProtocol.hpp - Generic DICE/TCAT protocol state and duplex control

#pragma once

#include "../../Duplex/FamilyDriver.hpp"
#include "../Core/DiceClockChangeObserver.hpp"
#include "../Core/DiceDeviceIo.hpp"
#include "../Core/DiceFamilyDriver.hpp"
#include "../Core/DiceNotificationMailbox.hpp"
#include "../Core/DiceNotificationRouter.hpp"
#include "../Core/DiceWaitClock.hpp"
#include "../Core/DICETransaction.hpp"
#include "../Core/DICETypes.hpp"
#include "../../IDeviceProtocol.hpp"
#include "../../../Controls/IControlProvider.hpp"
#include "../../../Runtime/ResolvedAudioConfiguration.hpp"
#include "../../../../Protocols/Ports/ProtocolRegisterIO.hpp"
#include "../../../../Scheduling/ITimerScheduler.hpp"

#include <DriverKit/IOLib.h>
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
// One mixer cell of a startup program: the cell index and its gain.
// Index is output * 18 + input; unity is 0x4000.
struct DiceStartupMixerCell final {
    uint16_t index{0};
    uint16_t gain{0};
};

struct DICETcatRuntimePolicy final {
    bool exposeDeviceToHostToCoreAudio{true};
    // Receiver-register dumps through the firmware loader's PEEK. Set only by
    // a protocol whose firmware TEST_ACTION table was verified (DICEBringupPolicy).
    bool verifiedLoaderPeek{false};
    // Keep the target-rate transition strict, but do not require a GLOBAL
    // source-lock indication before/just after host IT starts. This is for
    // devices whose selected receive-clock path only locks once host packets
    // are flowing; it does not select ARX1 as a clock source.
    bool requireSourceLockBeforeStreamEnable{true};
    bool requireSourceLockAtConfirm{true};

    // A router program to write at bring-up. Some devices power up with no
    // usable router program and stay silent until a host writes one: the Avid
    // Mbox Pro streams correctly and passes nothing to its analog stage.
    // Entries are (source << 8) | destination, each byte (block << 4) | channel.
    const uint16_t* startupRouterEntries{nullptr};
    uint32_t startupRouterEntryCount{0};

    // Mixer coefficients to write alongside it. A route table on its own is
    // half a configuration on devices whose outputs are fed from MIXo: without
    // coefficients the mixer path carries silence and nothing reaches the
    // outputs, which is how an earlier experiment was misread as "the mixer
    // path does not work".
    const DiceStartupMixerCell* startupMixerCells{nullptr};
    uint32_t startupMixerCellCount{0};

    // Cells the device's own firmware fills in at power-on and a host
    // configuration wants silent. NOT part of the startup program: on the Mbox
    // Pro, writing them there silences the analog outputs. They are cleared
    // once the stream is confirmed, when the firmware has placed them and no
    // longer touches them. Plain cell indices, indexed like startupMixerCells.
    const uint16_t* postStartMixerZeros{nullptr};
    uint32_t postStartMixerZeroCount{0};

    // Offset in the application section of a front-panel LED register that
    // the firmware only lights once a host writes it. The Mbox Pro's
    // UILEDState (+0x14) powers up holding its value but leaves the speaker
    // LED dark; writing that same value back lights it. 0 = none.
    uint32_t frontPanelLedRewriteOffset{0};

    // Passed to DiceFamilyDriver (DICEBringupPolicy): device steps around a
    // clock change. Off by default.
    DiceClockChangeObserver* clockChangeObserver{nullptr};
    bool waitForInitialDsp{false};

    // Explicit opt-in only. Reading unchanged stored tables is not a request
    // to reload them: MixControl skips identical images, and FFADO updates the
    // requested/current mode (dice_eap.cpp:528-554). Preserve firmware routing
    // during ordinary attachment and clock changes.
    bool reloadRouterAfterClockChange{false};
};

class DICETcatProtocol final : public Audio::IDeviceProtocol,
                               public Audio::FamilyDriver,
                               private Controls::IControlProvider,
                               private DiceClockChangeObserver {
public:
    using VoidCallback = std::function<void(IOReturn)>;

    DICETcatProtocol(Protocols::Ports::FireWireBusOps& busOps,
                     Protocols::Ports::FireWireBusInfo& busInfo,
                     Discovery::DeviceRegistry& routeRegistry,
                     const Discovery::DeviceRouteToken& route,
                     ::ASFW::IRM::IRMClient* irmClient,
                     DiceWaitClock& waitClock,
                     DiceNotificationRouter* notifications,
                     DICETcatRuntimePolicy runtimePolicy = {},
                     Scheduling::ITimerScheduler* preparationScheduler = nullptr);
    ~DICETcatProtocol() override;

    DICETcatProtocol(const DICETcatProtocol&) = delete;
    DICETcatProtocol& operator=(const DICETcatProtocol&) = delete;

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override { return "TCAT DICE"; }
    Audio::FamilyDriver* AsFamilyDriver() noexcept override { return this; }
    Controls::IControlProvider* Controls() noexcept override {
        return this;
    }

    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    std::shared_ptr<const std::vector<Runtime::RateFormation>> RateFormations() const override {
        if (!rateFormatsLock_) return {};
        IOLockLock(rateFormatsLock_);
        auto snapshot = rateFormations_;
        IOLockUnlock(rateFormatsLock_);
        return snapshot;
    }
    void ReadRateObservation(std::function<void(IOReturn, RateHardwareObservation)> callback) override;
    bool GetChannelLabels(std::vector<std::string>& inNames,
                          std::vector<std::string>& outNames) const override;

    // The GLOBAL clock and lock state, read asynchronously (safe on any queue).
    void ReadDuplexHealth(HealthCallback callback);
    void EnsureRuntimeStreamGeometry(VoidCallback callback) override;
    void CancelPendingPreparation() override {
        if (driver_) driver_->CloseReceiverLockWatch();
        CancelInitialClock();
    }
    bool CopyDiceReceiverLock(Diagnostics::DiceLockSnapshot& out) const override {
        if (!driver_) return false;
        out = driver_->ReceiverLockSnapshot();
        return out.guid != 0;
    }
    IOReturn RequestDiceReceiverDump(bool fifoEvents) override {
        return driver_ ? driver_->RequestReceiverDump(fifoEvents) : kIOReturnNotReady;
    }
    bool CopyDiceReceiverDump(Diagnostics::DiceReceiverDumpSnapshot& out) const override {
        if (!driver_) return false;
        out = driver_->ReceiverDumpSnapshot();
        return out.guid != 0;
    }
    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept override;

    IOReturn StopDuplex() override;

    // FamilyDriver. The stages run DiceFamilyDriver synchronously; geometry and
    // health await the same asynchronous reads the Default-queue publication
    // path uses.
    [[nodiscard]] IOReturn LoadGeometry() override;
    [[nodiscard]] std::optional<AudioStreamRuntimeCaps> RuntimeCaps() const override;
    [[nodiscard]] std::expected<DuplexPrepareResult, IOReturn> Configure(
        const AudioDuplexChannels& channels, const AudioClockConfig& clock) override;
    std::expected<AudioDuplexChannels, IOReturn> AssignChannels(const AudioDuplexChannels& channels) override;
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
    [[nodiscard]] StopPolicy GetStopPolicy() const noexcept override {
        // Quiesce locally, then disable/disarm the device before releasing IRM
        // resources. Linux dice-stream.c:465-466 uses this error-stop order;
        // :478-486 likewise keeps resource release after finish_session.
        return {.stopHostContextsBeforeDevice = true};
    }

    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) override;

    [[nodiscard]] Protocols::Ports::ProtocolRegisterIO& IO() noexcept { return io_; }
    [[nodiscard]] DICETransaction& Transaction() noexcept { return diceReader_; }

private:
    friend class DICETcatProtocolTestPeer;

    void Prepare(VoidCallback done) override;
    std::optional<Controls::ControlSnapshot> CopySnapshot() const override;
    void SetParameter(AudioModel::ParameterId id, const AudioModel::ParameterValue& value,
                      Controls::IControlProvider::Completion done) override;
    void ObserveClock(IOReturn status, const GlobalState& global,
                      const Discovery::DeviceRouteToken& route, uint64_t observation);
    IOLock* controlsLock_{IOLockAlloc()};
    std::optional<Controls::ControlSnapshot> controlSnapshot_;
    std::atomic<uint64_t> controlObservation_{0};
    std::atomic<bool> duplexPreparing_{false};
    // Source and rate share CLOCK_SELECT. Serialize their read/modify/write,
    // without waiting for source lock or changing the streaming lifecycle.
    std::atomic_flag clockWriteBusy_{};
    struct ClockWriteScope {
        std::atomic_flag& busy;
        bool acquired;
        explicit ClockWriteScope(std::atomic_flag& flag)
            : busy(flag), acquired(!flag.test_and_set()) {}
        void Release() { if (acquired) { acquired = false; busy.clear(); } }
        ~ClockWriteScope() { Release(); }
    };

    [[nodiscard]] static bool MakeDiceClockConfiguration(
        const AudioClockConfig& requested,
        DiceClockConfiguration& out) noexcept;
    void EnsureSectionsLoaded(VoidCallback callback);
    void EnsureRuntimeCapsLoaded(VoidCallback callback);
    struct InitialClockPreparation;
    void PrepareInitialClock(VoidCallback callback);
    void InitialClockOwner(const std::shared_ptr<InitialClockPreparation>& state);
    void InitialClockDsp(const std::shared_ptr<InitialClockPreparation>& state);
    void InitialClockDisable(const std::shared_ptr<InitialClockPreparation>& state);
    void InitialClockWrite(const std::shared_ptr<InitialClockPreparation>& state);
    void InitialClockPoll(const std::shared_ptr<InitialClockPreparation>& state);
    void ScheduleInitialClockPoll(const std::shared_ptr<InitialClockPreparation>& state,
                                  uint32_t delayMs, bool dsp);
    bool InitialClockCurrent(const std::shared_ptr<InitialClockPreparation>& state);
    void FinishInitialClock(const std::shared_ptr<InitialClockPreparation>& state, IOReturn status);
    void CancelInitialClock();
    void CacheRuntimeCaps(const GlobalState& global,
                          const StreamConfig& tx,
                          const StreamConfig& rx) noexcept;
    void CacheRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept;
    void ResetRuntimeCaps() noexcept;

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
    Scheduling::ITimerScheduler* preparationScheduler_{nullptr};
    std::shared_ptr<InitialClockPreparation> initialClock_;
    std::optional<Discovery::DeviceRouteToken> initializedRoute_;
    bool startupProgramApplied_{false};
    bool firmwareMonitorCellsCleared_{false};
    bool frontPanelLedRewritten_{false};

    // Writes the startup router program and mixer coefficients when the policy
    // carries them. Device configuration, not a bring-up gate: a failure is
    // logged and the streams still come up (silently, on devices that need it).
    void ApplyStartupProgram();
    [[nodiscard]] std::expected<uint32_t, IOReturn> ReadRouterEntryCount(
        const ExtensionSections& ext);
    [[nodiscard]] std::expected<void, IOReturn> WriteRouterProgram(
        const ExtensionSections& ext);
    void WriteStartupMixerCells(const ExtensionSections& ext);
    void ClearFirmwareMonitorCells();
    void RewriteFrontPanelLed();

    // DiceClockChangeObserver: the policy's observer first, then the router
    // reload when the policy asks for it.
    void BeforeClockSelectWrite(DiceDeviceIo& io) noexcept override;
    void AfterClockChange(DiceDeviceIo& io, uint32_t rateHz, bool rateChanged) noexcept override;
    // Write each rate mode's current router table back to the device and load
    // it. Device configuration, not a bring-up gate: failures are logged.
    void ReloadRouterPerRateMode(DiceDeviceIo& io, const char* why) noexcept;
    bool routerReloadedSinceAttach_{false};
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
    // leaves them intact). One label per host channel.
    static constexpr uint32_t kMaxChannelLabels = Runtime::kMaxHostPcmChannels;
    std::atomic<uint32_t> inputChannelLabelCount_{0};
    std::atomic<uint32_t> outputChannelLabelCount_{0};
    char inputChannelLabels_[kMaxChannelLabels][64]{};
    char outputChannelLabels_[kMaxChannelLabels][64]{};

    std::atomic<bool> runtimeCapsValid_{false};
    void PublishRateFormations(std::shared_ptr<const std::vector<Runtime::RateFormation>> snapshot) {
        if (!rateFormatsLock_) return;
        IOLockLock(rateFormatsLock_);
        if (!snapshot) {
            rateFormations_.reset();
            IOLockUnlock(rateFormatsLock_);
            return;
        }
        auto retained = rateFormations_ ? *rateFormations_ : std::vector<Runtime::RateFormation>{};
        for (const auto& formation : *snapshot) {
            const auto existing = std::ranges::find(retained, formation.sampleRateHz,
                &Runtime::RateFormation::sampleRateHz);
            // A placeholder never replaces a layout read from the device: a
            // re-describe after a reset rebuilds them for every rate.
            if (existing == retained.end()) retained.push_back(formation);
            else if (!formation.layoutProvisional || existing->layoutProvisional) *existing = formation;
        }
        std::ranges::sort(retained, {}, &Runtime::RateFormation::sampleRateHz);
        rateFormations_ = std::make_shared<const std::vector<Runtime::RateFormation>>(std::move(retained));
        IOLockUnlock(rateFormatsLock_);
    }
    IOLock* rateFormatsLock_{IOLockAlloc()};
    std::shared_ptr<const std::vector<Runtime::RateFormation>> rateFormations_{};
};

} // namespace ASFW::Audio::DICE::TCAT
