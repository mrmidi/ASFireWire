// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "MOTU828Mk3Protocol.hpp"

#include "../Duplex/FamilyStageWait.hpp"
#include "../../../Logging/Logging.hpp"

#include <utility>

namespace ASFW::Audio::MOTU {

struct MOTU828Mk3Protocol::SettleState final {
    Scheduling::TimerToken timer{Scheduling::kInvalidTimerToken};
    std::atomic<bool> completed{false};
    StageCallback completion;
};

MOTU828Mk3Protocol::MOTU828Mk3Protocol(
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    Discovery::DeviceRegistry& routeRegistry,
    const Discovery::DeviceRouteToken& route,
    IRM::IRMClient* irmClient,
    Scheduling::ITimerScheduler* timerScheduler) noexcept
    : busInfo_(busInfo), guid_(route.guid),
      registers_(busOps, busInfo, routeRegistry, route),
      irmClient_(irmClient), timerScheduler_(timerScheduler) {}

MOTU828Mk3Protocol::~MOTU828Mk3Protocol() noexcept {
    CancelSettle();
}

IOReturn MOTU828Mk3Protocol::Initialize() {
    return (irmClient_ != nullptr && timerScheduler_ != nullptr)
        ? kIOReturnSuccess
        : kIOReturnNotReady;
}

IOReturn MOTU828Mk3Protocol::Shutdown() {
    CancelSettle();
    runtimeCapsValid_.store(false, std::memory_order_release);
    pendingIsocWords_.reset();
    preparedGeneration_ = FW::Generation{0};
    return kIOReturnSuccess;
}

void MOTU828Mk3Protocol::UpdateRuntimeContext(
    const Discovery::DeviceRouteToken& route,
    std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) {
    (void)avcUnit;
    CancelSettle();
    registers_.UpdateRoute(route);
    pendingIsocWords_.reset();
    preparedGeneration_ = FW::Generation{0};
}

bool MOTU828Mk3Protocol::TeardownRequested() const noexcept {
    return teardownCancel_ != nullptr &&
           teardownCancel_->load(std::memory_order_acquire);
}

void MOTU828Mk3Protocol::CancelSettle() {
    auto state = std::move(activeSettle_);
    if (!state) {
        return;
    }
    if (timerScheduler_ != nullptr) {
        timerScheduler_->Cancel(state->timer);
    }
    if (!state->completed.exchange(true, std::memory_order_acq_rel) &&
        state->completion) {
        auto completion = std::move(state->completion);
        completion(kIOReturnAborted, {});
    }
}

void MOTU828Mk3Protocol::PublishGeometry(const StreamGeometry& geometry) noexcept {
    capsSequence_.fetch_add(1, std::memory_order_acq_rel);
    sampleRateHz_.store(geometry.sampleRateHz, std::memory_order_relaxed);
    hostInputPcm_.store(geometry.deviceToHostPcm, std::memory_order_relaxed);
    hostOutputPcm_.store(geometry.hostToDevicePcm, std::memory_order_relaxed);
    deviceToHostSlots_.store(geometry.deviceToHostDbs, std::memory_order_relaxed);
    hostToDeviceSlots_.store(geometry.hostToDeviceDbs, std::memory_order_relaxed);
    capsSequence_.fetch_add(1, std::memory_order_release);
    runtimeCapsValid_.store(true, std::memory_order_release);
}

void MOTU828Mk3Protocol::PublishChannels(const AudioDuplexChannels& channels) noexcept {
    capsSequence_.fetch_add(1, std::memory_order_acq_rel);
    deviceToHostChannel_.store(channels.deviceToHostIsoChannel, std::memory_order_relaxed);
    hostToDeviceChannel_.store(channels.hostToDeviceIsoChannel, std::memory_order_relaxed);
    capsSequence_.fetch_add(1, std::memory_order_release);
}

bool MOTU828Mk3Protocol::GetRuntimeAudioStreamCaps(
    AudioStreamRuntimeCaps& outCaps) const {
    if (!runtimeCapsValid_.load(std::memory_order_acquire)) {
        return false;
    }

    for (;;) {
        const uint32_t before = capsSequence_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }

        AudioStreamRuntimeCaps caps{};
        caps.sampleRateHz = sampleRateHz_.load(std::memory_order_relaxed);
        caps.hostInputPcmChannels = hostInputPcm_.load(std::memory_order_relaxed);
        caps.hostOutputPcmChannels = hostOutputPcm_.load(std::memory_order_relaxed);
        caps.deviceToHostAm824Slots = deviceToHostSlots_.load(std::memory_order_relaxed);
        caps.hostToDeviceAm824Slots = hostToDeviceSlots_.load(std::memory_order_relaxed);
        caps.deviceToHostIsoChannel =
            deviceToHostChannel_.load(std::memory_order_relaxed);
        caps.hostToDeviceIsoChannel =
            hostToDeviceChannel_.load(std::memory_order_relaxed);
        caps.deviceToHostStreamCount = 1;
        caps.hostToDeviceStreamCount = 1;
        caps.deviceToHostStreams[0] = AudioStreamWireInfo{
            .isoChannel = caps.deviceToHostIsoChannel,
            .pcmChannels = static_cast<uint16_t>(caps.hostInputPcmChannels),
            .am824Slots = static_cast<uint16_t>(caps.deviceToHostAm824Slots),
            .midiPorts = 1,
        };
        caps.hostToDeviceStreams[0] = AudioStreamWireInfo{
            .isoChannel = caps.hostToDeviceIsoChannel,
            .pcmChannels = static_cast<uint16_t>(caps.hostOutputPcmChannels),
            .am824Slots = static_cast<uint16_t>(caps.hostToDeviceAm824Slots),
            .midiPorts = 1,
        };

        const uint32_t after = capsSequence_.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0) {
            outCaps = caps;
            return true;
        }
    }
}

AudioStreamRuntimeCaps MOTU828Mk3Protocol::RuntimeCapsSnapshot() const noexcept {
    AudioStreamRuntimeCaps caps{};
    (void)GetRuntimeAudioStreamCaps(caps);
    return caps;
}

DuplexStageResult MOTU828Mk3Protocol::StageResult(
    DuplexRestartPhase phase) const noexcept {
    return DuplexStageResult{
        .generation = busInfo_.GetGeneration(),
        .channels = duplexChannels_,
        .phase = phase,
        .runtimeCaps = RuntimeCapsSnapshot(),
    };
}

void MOTU828Mk3Protocol::EnsureRuntimeStreamGeometry(VoidCallback callback) {
    if (TeardownRequested()) {
        callback(kIOReturnAborted);
        return;
    }

    const FW::Generation generation = busInfo_.GetGeneration();
    registers_.ReadQuad(
        kClockStatusOffset,
        [this, generation, callback = std::move(callback)](
            IOReturn clockStatus, uint32_t clockValue) mutable {
            if (clockStatus != kIOReturnSuccess) {
                callback(clockStatus);
                return;
            }
            const auto sampleRate = SampleRateFromClockStatus(clockValue);
            if (!sampleRate.has_value()) {
                callback(kIOReturnUnsupported);
                return;
            }

            registers_.ReadQuad(
                kAudioBankControlOffset,
                [this, generation, sampleRate, callback = std::move(callback)](
                    IOReturn bankStatus, uint32_t bankValue) mutable {
                    if (bankStatus != kIOReturnSuccess) {
                        callback(bankStatus);
                        return;
                    }
                    if (generation != busInfo_.GetGeneration()) {
                        callback(kIOReturnNotResponding);
                        return;
                    }
                    const auto geometry = Build828Mk3Geometry(*sampleRate, bankValue);
                    if (!geometry.has_value()) {
                        callback(kIOReturnUnsupported);
                        return;
                    }
                    PublishGeometry(*geometry);
                    appliedClock_ = AudioClockConfig{.sampleRateHz = *sampleRate};
                    callback(kIOReturnSuccess);
                });
        });
}

void MOTU828Mk3Protocol::PrepareDuplex(
    const AudioDuplexChannels& channels,
    const AudioClockConfig& desiredClock,
    PrepareCallback callback) {
    if (TeardownRequested()) {
        callback(kIOReturnAborted, {});
        return;
    }
    const AudioStreamRuntimeCaps caps = RuntimeCapsSnapshot();
    if (!runtimeCapsValid_.load(std::memory_order_acquire) ||
        !StreamConfigForSampleRate(desiredClock.sampleRateHz).has_value() ||
        caps.sampleRateHz != desiredClock.sampleRateHz ||
        !BuildIsocControlWords(0, channels).has_value()) {
        callback(kIOReturnUnsupported, {});
        return;
    }

    duplexChannels_ = channels;
    PublishChannels(channels);
    appliedClock_ = desiredClock;
    preparedGeneration_ = busInfo_.GetGeneration();
    registers_.RunPrepareSequence(
        [this, callback = std::move(callback)](IOReturn status) mutable {
            callback(status,
                     status == kIOReturnSuccess
                         ? DuplexPrepareResult{
                               .generation = busInfo_.GetGeneration(),
                               .channels = duplexChannels_,
                               .appliedClock = appliedClock_,
                               .runtimeCaps = RuntimeCapsSnapshot(),
                           }
                         : DuplexPrepareResult{});
        });
}

void MOTU828Mk3Protocol::SetAssignedChannels(
    const AudioDuplexChannels& channels) noexcept {
    duplexChannels_ = channels;
    PublishChannels(channels);
}

void MOTU828Mk3Protocol::ProgramRx(StageCallback callback) {
    if (timerScheduler_ == nullptr || preparedGeneration_ != busInfo_.GetGeneration()) {
        callback(kIOReturnNotReady, {});
        return;
    }
    if (TeardownRequested()) {
        callback(kIOReturnAborted, {});
        return;
    }
    CancelSettle();

    registers_.ReadQuad(
        kIsocControlOffset,
        [this, callback = std::move(callback)](
            IOReturn readStatus, uint32_t currentControl) mutable {
            if (readStatus != kIOReturnSuccess) {
                callback(readStatus, {});
                return;
            }
            const auto words = BuildIsocControlWords(currentControl, duplexChannels_);
            if (!words.has_value()) {
                callback(kIOReturnBadArgument, {});
                return;
            }
            pendingIsocWords_ = words;
            registers_.WriteQuad(
                kIsocControlOffset, words->deactivate,
                [this, callback = std::move(callback)](IOReturn writeStatus) mutable {
                    if (writeStatus != kIOReturnSuccess) {
                        callback(writeStatus, {});
                        return;
                    }

                    auto state = std::make_shared<SettleState>();
                    state->completion = std::move(callback);
                    activeSettle_ = state;
                    state->timer = timerScheduler_->ScheduleAfter(
                        kDeactivateSettleNs,
                        [this, state]() mutable {
                            if (state->completed.exchange(true, std::memory_order_acq_rel)) {
                                return;
                            }
                            if (activeSettle_.get() == state.get()) {
                                activeSettle_.reset();
                            }
                            auto completion = std::move(state->completion);
                            if (TeardownRequested()) {
                                completion(kIOReturnAborted, {});
                                return;
                            }
                            completion(kIOReturnSuccess,
                                       StageResult(DuplexRestartPhase::kDeviceRxProgrammed));
                        });
                });
        });
}

void MOTU828Mk3Protocol::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (preparedGeneration_ != busInfo_.GetGeneration() || !pendingIsocWords_.has_value()) {
        callback(kIOReturnNotReady, {});
        return;
    }
    if (TeardownRequested()) {
        callback(kIOReturnAborted, {});
        return;
    }
    const auto streamConfig = StreamConfigForSampleRate(appliedClock_.sampleRateHz);
    if (!streamConfig.has_value()) {
        callback(kIOReturnUnsupported, {});
        return;
    }

    const uint32_t activate = pendingIsocWords_->activate;
    registers_.WriteQuad(
        kIsocControlOffset, activate,
        [this, streamConfig, callback = std::move(callback)](IOReturn activateStatus) mutable {
            if (activateStatus != kIOReturnSuccess) {
                callback(activateStatus, {});
                return;
            }
            registers_.WriteQuad(
                kStreamConfigOffset, *streamConfig,
                [this, callback = std::move(callback)](IOReturn configStatus) mutable {
                    callback(configStatus,
                             configStatus == kIOReturnSuccess
                                 ? StageResult(DuplexRestartPhase::kDeviceTxArmed)
                                 : DuplexStageResult{});
                });
        });
}

void MOTU828Mk3Protocol::ConfirmDuplexStart(ConfirmCallback callback) {
    if (!pendingIsocWords_.has_value()) {
        callback(kIOReturnNotReady, {});
        return;
    }
    const uint32_t expectedControl = pendingIsocWords_->activate;
    constexpr uint32_t kVerifyMask = kRxIsocActivated | kRxChannelMask |
                                     kTxIsocActivated | kTxChannelMask;
    registers_.ReadQuad(
        kIsocControlOffset,
        [this, expectedControl, callback = std::move(callback)](
            IOReturn isocStatus, uint32_t isocControl) mutable {
            if (isocStatus != kIOReturnSuccess ||
                (isocControl & kVerifyMask) != (expectedControl & kVerifyMask)) {
                callback(isocStatus == kIOReturnSuccess
                             ? kIOReturnNotResponding
                             : isocStatus,
                         {});
                return;
            }
            registers_.ReadQuad(
                kClockStatusOffset,
                [this, callback = std::move(callback)](
                    IOReturn clockStatus, uint32_t clockValue) mutable {
                    const auto sampleRate = SampleRateFromClockStatus(clockValue);
                    if (clockStatus != kIOReturnSuccess || !sampleRate.has_value() ||
                        *sampleRate != appliedClock_.sampleRateHz) {
                        callback(clockStatus == kIOReturnSuccess
                                     ? kIOReturnNotResponding
                                     : clockStatus,
                                 {});
                        return;
                    }
                    registers_.WriteQuad(
                        kClockStatusOffset, SetFetchPcmFrames(clockValue),
                        [this, callback = std::move(callback)](IOReturn fetchStatus) mutable {
                            callback(fetchStatus,
                                     fetchStatus == kIOReturnSuccess
                                         ? DuplexConfirmResult{
                                               .generation = busInfo_.GetGeneration(),
                                               .channels = duplexChannels_,
                                               .appliedClock = appliedClock_,
                                               .runtimeCaps = RuntimeCapsSnapshot(),
                                           }
                                         : DuplexConfirmResult{});
                        });
                });
        });
}

void MOTU828Mk3Protocol::ApplyClockConfig(
    const AudioClockConfig& desiredClock,
    ClockApplyCallback callback) {
    const AudioStreamRuntimeCaps caps = RuntimeCapsSnapshot();
    if (!runtimeCapsValid_.load(std::memory_order_acquire) ||
        !StreamConfigForSampleRate(desiredClock.sampleRateHz).has_value() ||
        caps.sampleRateHz != desiredClock.sampleRateHz) {
        callback(kIOReturnUnsupported, {});
        return;
    }
    appliedClock_ = desiredClock;
    callback(kIOReturnSuccess,
             DuplexClockApplyResult{
                 .generation = busInfo_.GetGeneration(),
                 .appliedClock = appliedClock_,
                 .runtimeCaps = caps,
             });
}

void MOTU828Mk3Protocol::ReadDuplexHealth(HealthCallback callback) {
    registers_.ReadQuad(
        kClockStatusOffset,
        [this, callback = std::move(callback)](
            IOReturn status, uint32_t clockValue) mutable {
            const auto sampleRate = SampleRateFromClockStatus(clockValue);
            const bool rateHealthy = status == kIOReturnSuccess && sampleRate.has_value() &&
                                     *sampleRate == appliedClock_.sampleRateHz;
            callback(status,
                     DuplexHealthResult{
                         .generation = busInfo_.GetGeneration(),
                         .appliedClock = appliedClock_,
                         .runtimeCaps = RuntimeCapsSnapshot(),
                         // V3 exposes the rate state but no standalone lock bit.
                         .sourceLocked = rateHealthy,
                         .clockReferenceHealthy = rateHealthy,
                         .nominalRateHz = sampleRate.value_or(0),
                         .status = clockValue,
                     });
        });
}

void MOTU828Mk3Protocol::DisconnectPlayback(VoidCallback callback) {
    registers_.ReadQuad(
        kClockStatusOffset,
        [this, callback = std::move(callback)](
            IOReturn readStatus, uint32_t clockValue) mutable {
            if (readStatus != kIOReturnSuccess) {
                callback(readStatus);
                return;
            }
            registers_.WriteQuad(kClockStatusOffset,
                                 ClearFetchPcmFrames(clockValue),
                                 std::move(callback));
        });
}

void MOTU828Mk3Protocol::DisconnectCapture(VoidCallback callback) {
    registers_.ReadQuad(
        kIsocControlOffset,
        [this, callback = std::move(callback)](
            IOReturn readStatus, uint32_t isocControl) mutable {
            if (readStatus != kIOReturnSuccess) {
                callback(readStatus);
                return;
            }
            registers_.WriteQuad(kIsocControlOffset,
                                 BuildStopIsocControl(isocControl),
                                 std::move(callback));
        });
}

void MOTU828Mk3Protocol::BreakBothConnections(VoidCallback callback) {
    DisconnectPlayback(
        [this, callback = std::move(callback)](IOReturn playbackStatus) mutable {
            DisconnectCapture(
                [playbackStatus, callback = std::move(callback)](
                    IOReturn captureStatus) mutable {
                    callback(playbackStatus != kIOReturnSuccess
                                 ? playbackStatus
                                 : captureStatus);
                });
        });
}

// ---------------------------------------------------------------------------
// FamilyDriver
// ---------------------------------------------------------------------------

void MOTU828Mk3Protocol::SetTeardownCancelToken(
    const std::atomic<bool>* cancel) noexcept {
    teardownCancel_ = cancel;
}

IOReturn MOTU828Mk3Protocol::LoadGeometry() {
    // Unlike V2, the stream shape depends on device state (rate and optical
    // banks), so read it before channel planning sees the caps.
    return AwaitStageStatus(
        [&](auto callback) { EnsureRuntimeStreamGeometry(std::move(callback)); },
        teardownCancel_);
}

std::optional<AudioStreamRuntimeCaps> MOTU828Mk3Protocol::RuntimeCaps() const {
    AudioStreamRuntimeCaps caps{};
    if (!GetRuntimeAudioStreamCaps(caps)) {
        return std::nullopt;
    }
    return caps;
}

std::expected<DuplexPrepareResult, IOReturn> MOTU828Mk3Protocol::Configure(
    const AudioDuplexChannels& channels, const AudioClockConfig& clock) {
    return AwaitStage<DuplexPrepareResult>(
        [&](auto callback) { PrepareDuplex(channels, clock, std::move(callback)); },
        teardownCancel_);
}

std::expected<AudioDuplexChannels, IOReturn> MOTU828Mk3Protocol::AssignChannels(
    const AudioDuplexChannels& channels) {
    SetAssignedChannels(channels);
    return channels;
}

std::expected<DuplexHealthResult, IOReturn> MOTU828Mk3Protocol::ReadHealth(uint32_t timeoutMs) {
    return AwaitStage<DuplexHealthResult>(
        [&](auto callback) { ReadDuplexHealth(std::move(callback)); }, teardownCancel_,
        timeoutMs);
}

std::expected<DuplexStageResult, IOReturn> MOTU828Mk3Protocol::ArmDeviceRx() {
    return AwaitStage<DuplexStageResult>(
        [&](auto callback) { ProgramRx(std::move(callback)); }, teardownCancel_);
}

std::expected<DuplexStageResult, IOReturn> MOTU828Mk3Protocol::ArmDeviceTxAndEnable() {
    return AwaitStage<DuplexStageResult>(
        [&](auto callback) { ProgramTxAndEnableDuplex(std::move(callback)); }, teardownCancel_);
}

std::expected<DuplexConfirmResult, IOReturn> MOTU828Mk3Protocol::Confirm() {
    // Time anchor for the TX seed-window analysis: the device-side
    // fetch enable is written inside this step, so its position in the log
    // ring says whether the IT seed dump happened before or after it.
    ASFW_LOG(Audio, "MOTU828Mk3: ConfirmDuplexStart BEGIN GUID=%llx", guid_);
    auto confirmed = AwaitStage<DuplexConfirmResult>(
        [&](auto callback) { ConfirmDuplexStart(std::move(callback)); }, teardownCancel_);
    ASFW_LOG(Audio, "MOTU828Mk3: ConfirmDuplexStart END GUID=%llx status=0x%08x", guid_,
             static_cast<uint32_t>(confirmed ? kIOReturnSuccess : confirmed.error()));
    return confirmed;
}

std::expected<DuplexClockApplyResult, IOReturn> MOTU828Mk3Protocol::ApplyClockIdle(
    const AudioClockConfig& clock) {
    return AwaitStage<DuplexClockApplyResult>(
        [&](auto callback) { ApplyClockConfig(clock, std::move(callback)); }, teardownCancel_);
}

IOReturn MOTU828Mk3Protocol::DisconnectPlayback() {
    // The Mk3 stop recipe interleaves these with the host context stops, so
    // unlike V2 they do work: playback is the fetch bit, capture the isoc word.
    return AwaitStageStatus(
        [&](auto callback) { DisconnectPlayback(std::move(callback)); }, teardownCancel_);
}

IOReturn MOTU828Mk3Protocol::DisconnectCapture() {
    return AwaitStageStatus(
        [&](auto callback) { DisconnectCapture(std::move(callback)); }, teardownCancel_);
}

IOReturn MOTU828Mk3Protocol::BreakConnections() {
    return AwaitStageStatus(
        [&](auto callback) { BreakBothConnections(std::move(callback)); }, teardownCancel_);
}

IOReturn MOTU828Mk3Protocol::Stop() {
    // Not Unsupported: StopRoutine counts that as success, so a stop that lost
    // the interleaved recipe would leave the device streaming without an error.
    return AwaitStageStatus(
        [&](auto callback) { BreakBothConnections(std::move(callback)); }, teardownCancel_);
}

} // namespace ASFW::Audio::MOTU
