// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuProtocol.hpp - Shared control adapter for MOTU FireWire devices.
//
// Thin adapter: all wire encoding/decoding lives in the pure codecs of
// MotuRegisters.hpp; this class owns only transport (async register IO against
// kAddrBase + offset) and the cached device state those reads produce.
//
// Device-side streaming bring-up IS implemented here, through FamilyDriver.
// The host-side half -- the per-data-block SPH presentation times -- lives in
// Audio/Wire/MOTU (MotuRxTimingObserver measures the device clock, MotuTxTimingStamper
// stamps the transmit SPH), because it belongs with packet timing rather than
// register control.

#pragma once

#include "MotuRegisters.hpp"
#include "MotuNotificationMailbox.hpp"
#include "MotuControlModel.hpp"
#include "MotuDspMessages.hpp"
#include "../../../Scheduling/ITimerScheduler.hpp"
#include "../IDeviceProtocol.hpp"
#include "../Duplex/FamilyDriver.hpp"
#include "../Duplex/FamilyStageWait.hpp"
#include "../../../Protocols/Ports/ProtocolRegisterIO.hpp"
#include "../../Controls/IControlProvider.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

namespace ASFW::Audio::Motu {

/// Device clock state (0x0b14, or original 828's 0x0b00).
struct ClockStatus {
    uint32_t raw{0};
    uint32_t sampleRateHz{0};                ///< 0 when the rate index is unknown.
    std::optional<ClockSourceV2> source{};   ///< nullopt for reserved source codes.
};

/// Parameter id of the clock source control ('MCLK').
inline constexpr uint32_t kClockSourceParameterId = MotuControl::kClockSource;

/// Shared MOTU register adapter for FireWire-only V1, V2 and V3 models.
///
/// Also serves as its own FamilyDriver: the audio session reaches every protocol
/// through IDeviceProtocol::AsFamilyDriver(), so that is the seam a new family must
/// implement to be driven at all.
///
/// For a V3 model with a known clock source list (SelectableClockSourcesV3) or
/// known command-DSP ports (CommandDsp::DspPortsFor) it is also the control
/// provider (MotuControlModel.hpp): the clock source, which the Core Audio
/// projection publishes as the device's clock source, and everything CueMix
/// drives (MotuDspParameters.hpp).
class MotuProtocol final : public IDeviceProtocol, public FamilyDriver,
                           public ::ASFW::Audio::Controls::IControlProvider {
public:
    using ClockStatusCallback = std::function<void(IOReturn, ClockStatus)>;
    using CompletionCallback = std::function<void(IOReturn)>;

    // ---- IControlProvider (controller queue) ----
    ::ASFW::Audio::Controls::IControlProvider* Controls() noexcept override {
        return HasClockControl() || dspMessages_ ? this : nullptr;
    }
    /// Read 0x0b14 for the clock source, and on a command-DSP model register the
    /// message destination and start messaging, so the unit sends its mixer state.
    void Prepare(std::function<void(IOReturn)> done) override;
    [[nodiscard]] std::optional<::ASFW::Audio::Controls::ControlSnapshot> CopySnapshot() const override;
    /// The clock source: write the low byte of 0x0b14, then re-read the register;
    /// the completion carries the source the device reports. A DSP parameter:
    /// send its command, and record the value once the unit acknowledged it.
    void SetParameter(AudioModel::ParameterId id, const AudioModel::ParameterValue& value,
                      Completion done) override;

    MotuProtocol(Protocols::Ports::FireWireBusOps& busOps,
                   Protocols::Ports::FireWireBusInfo& busInfo,
                   Discovery::DeviceRegistry& routeRegistry,
                   const Discovery::DeviceRouteToken& route,
                   uint32_t unitSwVersion,
                   ::ASFW::IRM::IRMClient* irmClient = nullptr,
                   Scheduling::ITimerScheduler* timerScheduler = nullptr);
    ~MotuProtocol() override;

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override;

    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) override;

    void RebindNotifications(VoidCallback callback) override;

    /// Read current clock and optical registers, then publish complete model
    /// formations. Failed/reserved reads invalidate geometry; no fixed fallback.
    void EnsureRuntimeStreamGeometry(std::function<void(IOReturn)> callback) override;

    /// Authoritative geometry only after a completed read at a supported rate.
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    std::shared_ptr<const std::vector<Runtime::RateFormation>> RateFormations() const override;
    void ReadRateObservation(std::function<void(IOReturn, RateHardwareObservation)> callback) override;

    /// Port names in host channel order (MotuPortLayout.hpp). They come from the model's
    /// static table, so unlike DICE they are available before PrepareDuplex. Optical
    /// extras past the table are left unnamed for the caller to synthesize.
    bool GetChannelLabels(std::vector<std::string>& inNames,
                          std::vector<std::string>& outNames) const override;

    //==========================================================================
    // Duplex bring-up (IDeviceProtocol hooks).
    //
    // MOTU v2 activates both directions in a single write to the iso-comm
    // control register, so the device-side choreography is only two registers:
    // packet format first, then iso-comm (motu-stream.c:227-307,
    // snd_motu_stream_start_duplex). The host owns iso channel allocation and
    // hands the assignments in via AudioDuplexChannels.
    //
    // These hooks are only the register half. MOTU is duplex-always and recovers its
    // media clock from the host's stream -- the data-blocks-per-packet sequence, which
    // the host replays from the device's own, and the per-block SPH presentation times
    // (motu-stream.c:289-291). That half lives in Audio/Wire/MOTU: MotuRxTimingObserver
    // measures the device clock on receive, MotuTxTimingStamper stamps the SPH from the
    // regulator that follows it on transmit.
    //==========================================================================

    // IDeviceProtocol -> FamilyDriver. Returning `this` is what makes the audio
    // session able to drive this protocol at all.
    Audio::FamilyDriver* AsFamilyDriver() noexcept override { return this; }

    // ---- Stage chains (callback form; the FamilyDriver steps below wait on them) ----
    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const AudioClockConfig& desiredClock,
                       PrepareCallback callback);
    void SetAssignedChannels(const AudioDuplexChannels& channels) noexcept;
    void ProgramRx(StageCallback callback);
    void ProgramTxAndEnableDuplex(StageCallback callback);
    void ConfirmDuplexStart(ConfirmCallback callback);
    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          ClockApplyCallback callback);
    void ReadDuplexHealth(HealthCallback callback);
    [[nodiscard]] IOReturn StopDuplex() override;

    // ---- FamilyDriver ----
    // Each step starts the callback chain above and waits for it
    // (FamilyStageWait.hpp), so the chains and their wire traffic are unchanged.
    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept override;
    [[nodiscard]] StartReadinessPolicy GetStartReadinessPolicy() const noexcept override;
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

    /// Read and decode the clock status register.
    void ReadClockStatus(ClockStatusCallback callback);

    /// Read-modify-write the sample rate, preserving the clock source and all other bits.
    /// Fails with kIOReturnUnsupported for a rate the device does not implement.
    void SetSampleRate(uint32_t rateHz, CompletionCallback callback);

    /// Tell the device where to post its 4-byte async notifications. The caller owns the
    /// host address range (it must already be allocated and listening) and is responsible
    /// for re-registering after a bus reset, which clears the device-side value.
    /// `hostAddress` must lie inside [kAsyncMessageRegionStart, kAsyncMessageRegionEnd];
    /// anything else fails with kIOReturnBadArgument without touching the device.
    void RegisterAsyncMessageAddress(uint16_t hostNodeId,
                                     uint64_t hostAddress,
                                     CompletionCallback callback);

    /// Clear the notification address, releasing the device back to front-panel control
    /// (motu-transaction.c:121-133 writes zero to both halves on teardown).
    void ReleaseAsyncMessageAddress(CompletionCallback callback);

    /// True once a registration write has succeeded and not yet been released.
    [[nodiscard]] bool HasRegisteredAsyncAddress() const noexcept {
        return asyncAddressRegistered_.load(std::memory_order_acquire);
    }

    /// Last successfully decoded sample rate, or 0 before the first read completes.
    [[nodiscard]] uint32_t CachedSampleRateHz() const noexcept {
        return cachedSampleRateHz_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t UnitSwVersion() const noexcept { return unitSwVersion_; }

private:
    void CancelAbandonedStage(IOReturn status);
    template <typename T, typename Start>
    [[nodiscard]] std::expected<T, IOReturn> WaitRegisterStage(Start&& start,
        uint32_t timeoutMs = kFamilyStageTimeoutMs) {
        auto result = AwaitStage<T>(std::forward<Start>(start), teardownCancel_, timeoutMs);
        CancelAbandonedStage(result ? kIOReturnSuccess : result.error());
        return result;
    }
    template <typename Start>
    [[nodiscard]] IOReturn WaitRegisterStageStatus(Start&& start) {
        const auto status = AwaitStageStatus(std::forward<Start>(start), teardownCancel_);
        CancelAbandonedStage(status);
        return status;
    }
    enum class RegisterRecovery { Control, Asynchronous };
    void ReadRegister(Async::FWAddress address, Protocols::Ports::ProtocolRegisterIO::QuadReadCallback callback);
    void WriteRegister(Async::FWAddress address, uint32_t value,
                       Protocols::Ports::ProtocolRegisterIO::WriteCallback callback,
                       RegisterRecovery recovery = RegisterRecovery::Control);
    // Service teardown: the stage waits give up once it reads true.
    const std::atomic<bool>* teardownCancel_{nullptr};

    [[nodiscard]] static Async::FWAddress AddressOf(Reg reg) noexcept;

    /// Write the address-hi/address-lo pair as one logical operation. Both halves must
    /// land: the device only acts on a complete address, so a failed second write leaves
    /// it holding a half-updated value and the caller sees the failure.
    [[nodiscard]] bool IsV3() const noexcept;
    [[nodiscard]] bool IsV1() const noexcept { return unitSwVersion_ == 1 || unitSwVersion_ == 2; }
    [[nodiscard]] Reg ClockRegister() const noexcept { return unitSwVersion_ == 1 ? Reg::IsocCommControl : Reg::ClockStatusV2; }
    void EnsureAsyncAddress(CompletionCallback callback);
    void SetSampleRateV3(uint32_t rate, ClockStatus clock, CompletionCallback callback);
    [[nodiscard]] Reg OpticalRegister() const noexcept { return IsV1() ? ClockRegister() : IsV3() ? Reg::OpticalBanksV3 : Reg::InOutConfV2; }
    void ReadOpticalGeometry(CompletionCallback callback);
    void ReadDuplexConfirmation(ConfirmCallback callback);
    void WriteAsyncAddrPair(AsyncAddrValues values,
                            bool registered,
                            CompletionCallback callback);

    /// Apply the model-specific fetching-mode write around streaming, if this model
    /// needs one. A no-op success for the 828mk2 and 896HD.
    void ApplyFetchingModeIfNeeded(bool enable, CompletionCallback callback);

    /// Read-modify-write one register: read it, transform the value, write it back.
    /// Every duplex register on this device is RMW (reserved/low bits must survive), so
    /// the read failure and the write failure both surface through `callback`.
    void ModifyRegister(Reg reg,
                        std::function<uint32_t(uint32_t)> transform,
                        CompletionCallback callback);

    Protocols::Ports::ProtocolRegisterIO io_;
    /// Kept for the link speed written into the packet-format register; the register IO
    /// owns its own copy but does not expose speed resolution.
    Protocols::Ports::FireWireBusInfo& busInfo_;
    /// Iso channel/bandwidth allocation is owned by the coordinator, which reaches it
    /// through GetIRMClient(); the protocol only carries the handle.
    ::ASFW::IRM::IRMClient* irmClient_{nullptr};
    const uint32_t unitSwVersion_;
    Scheduling::ITimerScheduler* timerScheduler_{nullptr};
    Async::TransactionRetryOwner<uint32_t> registerCommands_;
    std::shared_ptr<Async::TransactionRetry<uint32_t>> confirmRead_;
    std::shared_ptr<NotificationMailbox> notifications_;
    uint64_t notificationAddress_{0};
    Discovery::DeviceRouteToken route_;
    std::atomic<uint32_t> cachedSampleRateHz_{0};
    std::atomic<bool> asyncAddressRegistered_{false};
    bool initialized_{false};
    std::atomic<bool> shuttingDown_{false};

    // Iso channels the host assigned, latched by PrepareDuplex and consumed by
    // ProgramTxAndEnableDuplex. Device-relative naming: RX is host->device
    // (playback), TX is device->host (capture).
    std::atomic<uint8_t> deviceRxChannel_{0};
    std::atomic<uint8_t> deviceTxChannel_{0};
    std::atomic<bool> duplexPrepared_{false};
    std::atomic<bool> duplexActive_{false};

    // Chunk geometry resolved during PrepareDuplex (fixed baseline plus any ADAT
    // extras), reported back through the duplex result structs.
    std::atomic<uint32_t> txPcmChunks_{0};
    std::atomic<uint32_t> rxPcmChunks_{0};
    std::atomic<uint32_t> preparedRateHz_{0};

    // One atomic snapshot: low 32 bits are the optical register, bit 32
    // proves a completed read. A reader cannot mix validity from one refresh
    // with a raw register from the next refresh.
    std::atomic<uint64_t> opticalSnapshot_{0};

    /// Fill the runtime capability block from the geometry resolved by PrepareDuplex.
    [[nodiscard]] AudioStreamRuntimeCaps MakeRuntimeCaps() const noexcept;

    [[nodiscard]] bool HasClockControl() const noexcept {
        return !SelectableClockSourcesV3(unitSwVersion_).empty();
    }
    /// Record the clock source `raw` reports, and announce a changed value.
    /// False when the source is not in the model's list (chosen elsewhere);
    /// the clock control is then withdrawn.
    bool CommitClockControl(uint32_t raw);
    void SetClockSourceParameter(const AudioModel::ParameterValue& value, Completion done);
    void SetMixerParameter(AudioModel::ParameterId id, const AudioModel::ParameterValue& value,
                           Completion done);
    void PublishParameter(uint32_t id, const AudioModel::ParameterValue& value);
    /// Register the DSP message destination, then begin messaging (tag 1, then
    /// tag 2, both empty, as command_dsp.rs begin_messaging does).
    void StartDspMessaging(CompletionCallback callback);
    /// Block-write `frames` to the DSP command address, one after another.
    void SendDspFrames(std::shared_ptr<std::vector<std::vector<uint8_t>>> frames, size_t index,
                       CompletionCallback callback);

    // Controller queue only, like every IControlProvider call.
    std::optional<int64_t> clockCode_{};
    uint64_t controlRevision_{0};
    // Command-DSP models. The mailbox is shared with the receive path.
    std::shared_ptr<CommandDsp::MessageMailbox> dspMessages_;
    uint64_t dspMessageAddress_{0};
    uint8_t dspSequence_{0};
    bool dspMessagingStarted_{false};
};

} // namespace ASFW::Audio::Motu
