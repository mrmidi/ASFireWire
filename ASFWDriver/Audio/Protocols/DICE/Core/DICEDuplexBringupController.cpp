// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// DICEDuplexBringupController.cpp - Raw-reference duplex startup for generic DICE devices

#include "DICEDuplexBringupController.hpp"

#include "DICENotificationMailbox.hpp"
#include "../../../../Common/WireFormat.hpp"
#include "../../../../Logging/Logging.hpp"

#include <atomic>
#include <limits>
#include <memory>
#include <utility>

namespace ASFW::Audio::DICE {

namespace {

// Linux DICE waits for CLOCK_ACCEPTED for NOTIFICATION_TIMEOUT_MS (150 ms) after
// GLOBAL_CLOCK_SELECT. Source-lock policy is a separate, device-profile decision.
// Cross-validated with linux-sound-firewire-stack/firewire/dice/dice-stream.c:60-98.
constexpr uint32_t kClockAcceptedTimeoutMs = 150;
constexpr uint32_t kStreamingClockLockTimeoutMs = 2000;
constexpr uint32_t kPollIntervalMs = 10;
constexpr uint32_t kReadyTimeoutMs = 200;
constexpr uint32_t kStopSyncTimeoutMs = 5000;
constexpr uint32_t kStopSyncPollMs = 10;

constexpr uint32_t kDisabledIsoChannel = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kRxSeqStartDefault = 0;
constexpr uint32_t kTxSpeedS400 = 2;

[[nodiscard]] IOReturn MapTransportStatus(Async::AsyncStatus status) noexcept {
    return Protocols::Ports::MapAsyncStatusToIOReturn(status);
}

void RecordFirstError(IOReturn& slot, IOReturn status) noexcept {
    if (slot == kIOReturnSuccess && status != kIOReturnSuccess) {
        slot = status;
    }
}

uint64_t DecodeOwnerOctlet(const uint8_t* data, size_t size) noexcept {
    if (data == nullptr || size < 8) {
        return 0;
    }
    return ASFW::FW::ReadBE64(data);
}

void CacheRuntimeCaps(AudioStreamRuntimeCaps& caps,
                      const GlobalState& global,
                      const StreamConfig& tx,
                      const StreamConfig& rx) noexcept {
    caps.hostInputPcmChannels = tx.TotalPcmChannels();
    caps.deviceToHostAm824Slots = tx.TotalAm824Slots();
    caps.hostOutputPcmChannels = rx.TotalPcmChannels();
    caps.hostToDeviceAm824Slots = rx.TotalAm824Slots();
    caps.sampleRateHz = global.sampleRate;
    caps.deviceToHostIsoChannel =
        tx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel);
    caps.hostToDeviceIsoChannel =
        rx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel);

    // Per-stream wire geometry. Stream count comes from the DICE stream-format
    // header (TX_NUMBER/RX_NUMBER), which includes streams the device reports
    // with iso=-1 (disabled) that the host must still arm for a multi-stream
    // device such as the Venice F32 (2×16 channels). The discovered isoChannel
    // is carried through but the host reassigns it during channel resolution.
    auto fillPerStream = [](const StreamConfig& sc,
                            uint32_t& outCount,
                            AudioStreamWireInfo* outStreams) noexcept {
        const uint32_t count =
            (sc.numStreams < kMaxAudioStreamsPerDirection)
                ? sc.numStreams
                : kMaxAudioStreamsPerDirection;
        outCount = count;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& entry = sc.streams[i];
            outStreams[i].isoChannel =
                (entry.isoChannel >= 0 && entry.isoChannel <= 0x3F)
                    ? static_cast<uint8_t>(entry.isoChannel)
                    : AudioStreamWireInfo::kInvalidIsoChannel;
            outStreams[i].pcmChannels =
                static_cast<uint16_t>(entry.pcmChannels);
            outStreams[i].am824Slots =
                static_cast<uint16_t>(entry.Am824Slots());
            outStreams[i].midiPorts =
                static_cast<uint16_t>(entry.midiPorts);
        }
    };
    fillPerStream(tx, caps.deviceToHostStreamCount, caps.deviceToHostStreams);
    fillPerStream(rx, caps.hostToDeviceStreamCount, caps.hostToDeviceStreams);
}

} // namespace

DICEDuplexBringupController::DICEDuplexBringupController(
    DICETransaction& diceReader,
    Protocols::Ports::ProtocolRegisterIO& io,
    Protocols::Ports::FireWireBusInfo& busInfo,
    IODispatchQueue* workQueue,
    GeneralSections sections,
    Scheduling::ITimerScheduler* timerScheduler,
    DICEBringupPolicy bringupPolicy)
    : diceReader_(diceReader)
    , io_(io)
    , busInfo_(busInfo)
    , workQueue_(workQueue)
    , timerScheduler_(timerScheduler)
    , sections_(sections)
    , bringupPolicy_(bringupPolicy) {
}

DICEDuplexBringupController::~DICEDuplexBringupController() {
    CancelScheduledRetry();
}

DICEDuplexBringupController::Operation* DICEDuplexBringupController::ActiveOperation() noexcept {
    return std::visit(
        [](auto& state) -> Operation* {
            if constexpr (!requires { state.operation; }) {
                return nullptr;
            } else {
                return &state.operation;
            }
        },
        state_);
}

const DICEDuplexBringupController::Operation*
DICEDuplexBringupController::ActiveOperation() const noexcept {
    return std::visit(
        [](const auto& state) -> const Operation* {
            if constexpr (!requires { state.operation; }) {
                return nullptr;
            } else {
                return &state.operation;
            }
        },
        state_);
}

bool DICEDuplexBringupController::HasActiveOperation() const noexcept {
    return ActiveOperation() != nullptr;
}

DiceRestartPhase DICEDuplexBringupController::CurrentPhase() const noexcept {
    return std::visit(
        [](const auto& state) -> DiceRestartPhase {
            if constexpr (!requires { state.operation; }) {
                return DiceRestartPhase::kIdle;
            } else if constexpr (std::same_as<decltype(state), const PreparingState&>) {
                return DiceRestartPhase::kPreparingDevice;
            } else if constexpr (std::same_as<decltype(state), const AwaitingClockAcceptedState&>) {
                return DiceRestartPhase::kWaitingGlobalClock;
            } else if constexpr (std::same_as<decltype(state), const PreparedState&>) {
                return DiceRestartPhase::kPrepared;
            } else if constexpr (std::same_as<decltype(state), const ProgrammingRxState&>) {
                return DiceRestartPhase::kProgrammingDeviceRx;
            } else if constexpr (std::same_as<decltype(state), const RxProgrammedState&>) {
                return DiceRestartPhase::kDeviceRxProgrammed;
            } else if constexpr (std::same_as<decltype(state), const ProgrammingTxState&>) {
                return DiceRestartPhase::kProgrammingDeviceTx;
            } else if constexpr (std::same_as<decltype(state), const TxArmedState&>) {
                return DiceRestartPhase::kDeviceTxArmed;
            } else if constexpr (std::same_as<decltype(state), const ConfirmingState&>) {
                return DiceRestartPhase::kConfirmingDeviceStart;
            } else if constexpr (std::same_as<decltype(state), const RunningState&>) {
                return DiceRestartPhase::kRunning;
            } else if constexpr (std::same_as<decltype(state), const StoppingState&>) {
                return DiceRestartPhase::kStopping;
            } else {
                return DiceRestartPhase::kFailed;
            }
        },
        state_);
}

bool DICEDuplexBringupController::IsPrepared() const noexcept {
    return std::holds_alternative<PreparedState>(state_) ||
           std::holds_alternative<ProgrammingRxState>(state_) ||
           std::holds_alternative<RxProgrammedState>(state_) ||
           std::holds_alternative<ProgrammingTxState>(state_) ||
           std::holds_alternative<TxArmedState>(state_) ||
           std::holds_alternative<ConfirmingState>(state_) ||
           std::holds_alternative<RunningState>(state_);
}

bool DICEDuplexBringupController::IsArmed() const noexcept {
    return std::holds_alternative<TxArmedState>(state_) ||
           std::holds_alternative<ConfirmingState>(state_) ||
           std::holds_alternative<RunningState>(state_);
}

bool DICEDuplexBringupController::IsRunning() const noexcept {
    return std::holds_alternative<RunningState>(state_);
}

void DICEDuplexBringupController::RefreshPreparedSectionLayout(
    SectionLayoutCallback callback) {
    if (!std::holds_alternative<PreparedState>(state_)) {
        callback(kIOReturnNotReady, {});
        return;
    }
    if (!EnsureRouteCurrent()) {
        callback(kIOReturnOffline, {});
        return;
    }

    const GeneralSections prior = sections_;
    diceReader_.ReadGeneralSections(
        [this, prior, callback = std::move(callback)](IOReturn status,
                                                       GeneralSections refreshed) mutable {
            if (status != kIOReturnSuccess) {
                callback(status, {});
                return;
            }

            sections_ = refreshed;
            ASFW_LOG(DICE,
                     "DICE prepared stream layout refreshed after stopped hook: "
                     "tx %u/%u -> %u/%u rx %u/%u -> %u/%u",
                     prior.txStreamFormat.offset, prior.txStreamFormat.size,
                     sections_.txStreamFormat.offset, sections_.txStreamFormat.size,
                     prior.rxStreamFormat.offset, prior.rxStreamFormat.size,
                     sections_.rxStreamFormat.offset, sections_.rxStreamFormat.size);
            callback(kIOReturnSuccess, sections_);
        });
}

bool DICEDuplexBringupController::IsOwnerClaimed() const noexcept {
    const Operation* operation = ActiveOperation();
    return operation != nullptr && operation->ownerClaimed;
}

void DICEDuplexBringupController::ResetOperation() noexcept {
    state_ = IdleState{};
}

void DICEDuplexBringupController::BeginOperation(
    FW::Generation generation,
    const AudioDuplexChannels& channels,
    DiceRestartReason reason,
    const DiceClockConfiguration& clock,
    FlowMode flow,
    bool forceClockSelectWrite) noexcept {
    state_ = PreparingState{.operation = Operation{
                                 .generation = generation,
                                 .channels = channels,
                                 .reason = reason,
                                 .desiredClock = AudioClockConfig{.sampleRateHz = clock.sampleRateHz},
                                 .flow = flow,
                                 .forceClockSelectWrite = forceClockSelectWrite,
                             }};
}

void DICEDuplexBringupController::ProgramRx(StageCallback callback) {
    ProgramRxForDuplex48k(
        [this, callback = std::move(callback)](IOReturn status) mutable {
            DiceDuplexStageResult result{};
            if (status == kIOReturnSuccess) {
                const Operation* operation = ActiveOperation();
                result.generation = operation->generation;
                result.channels = operation->channels;
                result.phase = CurrentPhase();
                result.runtimeCaps = runtimeCaps_;
            }
            callback(status, result);
        });
}

void DICEDuplexBringupController::ProgramTxAndEnableDuplex(StageCallback callback) {
    ProgramTxAndEnableDuplex48k(
        [this, callback = std::move(callback)](IOReturn status) mutable {
            DiceDuplexStageResult result{};
            if (status == kIOReturnSuccess) {
                const Operation* operation = ActiveOperation();
                result.generation = operation->generation;
                result.channels = operation->channels;
                result.phase = CurrentPhase();
                result.runtimeCaps = runtimeCaps_;
            }
            callback(status, result);
        });
}

void DICEDuplexBringupController::ConfirmDuplexStart(ConfirmCallback callback) {
    ConfirmDuplex48kStart(
        [this, callback = std::move(callback)](IOReturn status) mutable {
            DiceDuplexConfirmResult result{};
            if (status == kIOReturnSuccess) {
                const Operation* operation = ActiveOperation();
                result.generation = operation->generation;
                result.channels = operation->channels;
                result.appliedClock = operation->appliedClock;
                result.runtimeCaps = runtimeCaps_;
                result.notification = confirmNotification_;
                result.status = confirmStatus_;
                result.extStatus = confirmExtStatus_;
            }
            callback(status, result);
        });
}

void DICEDuplexBringupController::ApplyClockConfig(
    const DiceClockConfiguration& desiredClock, ClockApplyCallback callback) {
    BeginClockApply(desiredClock, false, std::move(callback));
}

void DICEDuplexBringupController::RefineClockForStreamGeometry(
    const DiceClockConfiguration& currentClock, ClockApplyCallback callback) {
    BeginClockApply(currentClock, true, std::move(callback));
}

void DICEDuplexBringupController::BeginClockApply(
    const DiceClockConfiguration& desiredClock,
    bool forceClockSelectWrite,
    ClockApplyCallback callback) {
    if (!IsSupportedDiceClockConfiguration(desiredClock)) {
        callback(kIOReturnUnsupported, {});
        return;
    }

    if (!busInfo_.GetLocalNodeID().IsValid()) {
        callback(kIOReturnNotReady, {});
        return;
    }

    if (HasActiveOperation()) {
        callback(kIOReturnBusy, {});
        return;
    }

    CancelScheduledRetry();
    NotificationMailbox::Reset();
    stopSequenceError_ = kIOReturnSuccess;
    diceClock_ = desiredClock;
    const FW::Generation generation = busInfo_.GetGeneration();
    BeginOperation(busInfo_.GetGeneration(),
                   AudioDuplexChannels{},
                   DiceRestartReason::kManualReconfigure,
                   desiredClock,
                   FlowMode::kClockApply,
                   forceClockSelectWrite);
    runtimeCaps_ = {};
    confirmNotification_ = 0;
    confirmStatus_ = 0;
    confirmExtStatus_ = 0;

    const AudioDuplexChannels channels{};
    DoReadGlobalStatus(channels,
                       [generation, desiredClock, this, callback = std::move(callback)](IOReturn status) mutable {
                           DiceClockApplyResult result{};
                           if (status == kIOReturnSuccess) {
                               result.generation = generation;
                               result.appliedClock = AudioClockConfig{.sampleRateHz = desiredClock.sampleRateHz};
                               result.runtimeCaps = runtimeCaps_;
                           }
                           callback(status, result);
                       });
}

bool DICEDuplexBringupController::ScheduleRetry(uint64_t delayMs, std::function<void()> work) {
    if (!work || timerScheduler_ == nullptr) {
        return false;
    }

    CancelScheduledRetry();
    const uint64_t epoch = scheduledRetryEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const auto token = timerScheduler_->ScheduleAfter(
        delayMs * 1'000'000ULL,
        [this, epoch, work = std::move(work)]() mutable {
            if (scheduledRetryEpoch_.load(std::memory_order_acquire) != epoch) {
                return;
            }
            scheduledRetry_.store(Scheduling::kInvalidTimerToken, std::memory_order_release);
            work();
        });
    if (token == Scheduling::kInvalidTimerToken) {
        return false;
    }
    scheduledRetry_.store(token, std::memory_order_release);
    return true;
}

void DICEDuplexBringupController::CancelScheduledRetry() noexcept {
    scheduledRetryEpoch_.fetch_add(1, std::memory_order_acq_rel);
    const auto token = scheduledRetry_.exchange(Scheduling::kInvalidTimerToken,
                                                std::memory_order_acq_rel);
    if (token != Scheduling::kInvalidTimerToken && timerScheduler_ != nullptr) {
        timerScheduler_->Cancel(token);
    }
}

bool DICEDuplexBringupController::EnsureRouteCurrent() const noexcept {
    if (!HasActiveOperation()) {
        return true;
    }
    return io_.IsRouteCurrent();
}

bool DICEDuplexBringupController::TeardownRequested() const noexcept {
    return teardownCancel_ != nullptr && teardownCancel_->load(std::memory_order_acquire);
}

void DICEDuplexBringupController::RecordStopTeardownAbort(const char* stage) const noexcept {
    ASFW_LOG(DICE,
             "DICEDuplexBringupController: StopDuplex aborted by teardown stage=%{public}s kr=0x%x",
             stage ? stage : "unknown",
             kIOReturnAborted);
}

bool DICEDuplexBringupController::AbortStopIfTeardown(const char* stage, VoidCallback& cb) {
    if (!TeardownRequested()) {
        return false;
    }

    stopSequenceError_ = kIOReturnAborted;
    ResetOperation();
    RecordStopTeardownAbort(stage);
    cb(stopSequenceError_);
    return true;
}

uint64_t DICEDuplexBringupController::OwnerValue() const noexcept {
    const uint64_t localNodeId =
        0xFFC0ULL | static_cast<uint64_t>(busInfo_.GetLocalNodeID().value & 0x3FU);
    return (localNodeId << kOwnerNodeShift) | NotificationMailbox::kHandlerOffset;
}

void DICEDuplexBringupController::PrepareDuplex48k(
    const AudioDuplexChannels& channels,
    VoidCallback callback) {
    if (channels.deviceToHostIsoChannel > 63 || channels.hostToDeviceIsoChannel > 63) {
        callback(kIOReturnBadArgument);
        return;
    }
    if (!busInfo_.GetLocalNodeID().IsValid()) {
        callback(kIOReturnNotReady);
        return;
    }
    CancelScheduledRetry();
    if (HasActiveOperation()) {
        const IOReturn stopStatus = StopDuplex();
        if (stopStatus != kIOReturnSuccess) {
            callback(stopStatus);
            return;
        }
    }

    refreshRuntimeCapsOnPrepare_ = false;
    NotificationMailbox::Reset();
    stopSequenceError_ = kIOReturnSuccess;
    diceClock_ = DiceClockConfiguration{
        .sampleRateHz = 48000U,
        .clockSelect = kDiceClockSelect48kInternal,
    };
    BeginOperation(busInfo_.GetGeneration(),
                   channels,
                   DiceRestartReason::kInitialStart,
                   diceClock_,
                   FlowMode::kPrepareDuplex,
                   false);
    runtimeCaps_ = {};
    confirmNotification_ = 0;
    confirmStatus_ = 0;
    confirmExtStatus_ = 0;

    ASFW_LOG(DICE,
             "PrepareDuplex48k: raw parity start gen=%u localNode=0x%02x rxIso=%u txIso=%u",
             ActiveOperation()->generation.value,
             busInfo_.GetLocalNodeID().value,
             channels.hostToDeviceIsoChannel,
             channels.deviceToHostIsoChannel);

    DoReadGlobalStatus(channels, std::move(callback));
}

void DICEDuplexBringupController::PrepareDuplex(
    const AudioDuplexChannels& channels,
    const DiceClockConfiguration& desiredClock,
    PrepareCallback callback) {
    if (channels.deviceToHostIsoChannel > 63 || channels.hostToDeviceIsoChannel > 63) {
        callback(kIOReturnBadArgument, {});
        return;
    }
    if (!busInfo_.GetLocalNodeID().IsValid()) {
        callback(kIOReturnNotReady, {});
        return;
    }
    if (!IsSupportedDiceClockConfiguration(desiredClock)) {
        callback(kIOReturnUnsupported, {});
        return;
    }
    CancelScheduledRetry();
    if (HasActiveOperation()) {
        const IOReturn stopStatus = StopDuplex();
        if (stopStatus != kIOReturnSuccess) {
            callback(stopStatus, {});
            return;
        }
    }

    refreshRuntimeCapsOnPrepare_ = true;
    NotificationMailbox::Reset();
    stopSequenceError_ = kIOReturnSuccess;
    diceClock_ = desiredClock;
    BeginOperation(busInfo_.GetGeneration(),
                   channels,
                   DiceRestartReason::kInitialStart,
                   desiredClock,
                   FlowMode::kPrepareDuplex,
                   false);
    runtimeCaps_ = {};
    confirmNotification_ = 0;
    confirmStatus_ = 0;
    confirmExtStatus_ = 0;

    ASFW_LOG(DICE,
             "PrepareDuplex48k: raw parity start gen=%u localNode=0x%02x rxIso=%u txIso=%u",
             ActiveOperation()->generation.value,
             busInfo_.GetLocalNodeID().value,
             channels.hostToDeviceIsoChannel,
             channels.deviceToHostIsoChannel);

    DoReadGlobalStatus(channels,
                       [this, channels, callback = std::move(callback)](IOReturn status) mutable {
                           DiceDuplexPrepareResult result{};
                           if (status == kIOReturnSuccess) {
                               const Operation* operation = ActiveOperation();
                               result.generation = operation->generation;
                               result.channels = channels;
                               result.appliedClock = operation->appliedClock;
                               result.runtimeCaps = runtimeCaps_;
                           }
                           callback(status, result);
                       });
}

void DICEDuplexBringupController::DoReadGlobalStatus(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    (void)io_.ReadQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kStatus),
                   [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                        const IOReturn status = MapTransportStatus(transportStatus);
                        if (status != kIOReturnSuccess) {
                            DoRollback(status, std::move(cb));
                            return;
                        }
                        DoRefreshSectionLayout(channels, std::move(cb));
                    });
}

void DICEDuplexBringupController::DoRefreshSectionLayout(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    diceReader_.ReadGeneralSections([this, channels, cb = std::move(cb)](IOReturn status, GeneralSections sections) mutable {
        if (status != kIOReturnSuccess) {
            DoRollback(status, std::move(cb));
            return;
        }

        sections_ = sections;
        DoReadGlobalBeforeClaim(channels, std::move(cb));
    });
}

void DICEDuplexBringupController::DoReadGlobalBeforeClaim(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    diceReader_.ReadGlobalStateFull(sections_,
                                    [this, channels, cb = std::move(cb)](IOReturn status, GlobalState state) mutable {
                                if (status != kIOReturnSuccess) {
                                    DoRollback(status, std::move(cb));
                                    return;
                                }

                                Operation* operation = ActiveOperation();
                                operation->preClaimClockSelect = state.clockSelect;
                                operation->preClaimDeviceEnabled = state.enabled;
                                ASFW_LOG(DICE,
                                         "PrepareDuplex48k: global pre-claim owner=0x%016llx enable=%u notify=0x%08x clockSelect=0x%08x",
                                         state.owner,
                                         state.enabled ? 1U : 0U,
                                         state.notification,
                                         state.clockSelect);
                                DoReadOwnerBeforeClaim(channels, std::move(cb));
                            });
}

void DICEDuplexBringupController::DoReadOwnerBeforeClaim(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    (void)io_.ReadBlock(MakeDICEAddress(sections_.global.offset + GlobalOffset::kOwnerHi),
                  8,
                  [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, std::span<const uint8_t> payload) mutable {
                      const IOReturn status = MapTransportStatus(transportStatus);
                      if (status != kIOReturnSuccess || payload.size() < 8) {
                          DoRollback((status == kIOReturnSuccess) ? kIOReturnUnderrun : status, std::move(cb));
                          return;
                      }

                      ASFW_LOG(DICE,
                               "PrepareDuplex48k: owner before claim=0x%016llx",
                               DecodeOwnerOctlet(payload.data(), payload.size()));
                      DoClaimOwner(channels, std::move(cb));
                  });
}

void DICEDuplexBringupController::DoClaimOwner(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    const uint64_t ownerValue = OwnerValue();
    (void)io_.CompareSwap64BE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kOwnerHi),
                        kOwnerNoOwner,
                        ownerValue,
                        [this, channels, ownerValue, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint64_t previous) mutable {
                              const IOReturn status = MapTransportStatus(transportStatus);
                              if (status != kIOReturnSuccess) {
                                  DoRollback(status, std::move(cb));
                                  return;
                              }
                              if (previous != kOwnerNoOwner && previous != ownerValue) {
                                  DoRollback(kIOReturnExclusiveAccess, std::move(cb));
                                  return;
                              }

                              ActiveOperation()->ownerClaimed = true;
                              DoReadOwnerAfterClaim(channels, std::move(cb));
                          });
}

void DICEDuplexBringupController::DoReadOwnerAfterClaim(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    (void)io_.ReadBlock(MakeDICEAddress(sections_.global.offset + GlobalOffset::kOwnerHi),
                  8,
                  [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, std::span<const uint8_t> payload) mutable {
                      const IOReturn status = MapTransportStatus(transportStatus);
                      if (status != kIOReturnSuccess || payload.size() < 8) {
                          DoRollback((status == kIOReturnSuccess) ? kIOReturnUnderrun : status, std::move(cb));
                          return;
                      }

                      const uint64_t ownerReadback = DecodeOwnerOctlet(payload.data(), payload.size());
                      if (ownerReadback != OwnerValue()) {
                          DoRollback(kIOReturnExclusiveAccess, std::move(cb));
                          return;
                      }

                      DoNormalizeGlobalEnable(channels, std::move(cb));
                  });
}

void DICEDuplexBringupController::DoNormalizeGlobalEnable(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    // Saffire.kext's restart path always stops the remote engine before it
    // reports the new channels and starts it again.  During dext teardown our
    // cancellation guard can intentionally abandon remote I/O, leaving
    // GLOBAL_ENABLE asserted for the next driver instance.  A second write of
    // 1 does not provide a restart edge for the DICE formatter.  The observed
    // failure mode was correctly paced packets containing MIDI placeholders
    // but zero/unlabelled PCM, consistent with that inherited running state.
    // Normalize only a stream bring-up.  An idle clock-apply operation must not
    // unexpectedly stop a stream owned by another control flow.
    Operation* operation = ActiveOperation();
    if (operation->flow != FlowMode::kPrepareDuplex || !operation->preClaimDeviceEnabled) {
        DoWriteClockSelect(channels, std::move(cb));
        return;
    }

    ASFW_LOG(DICE,
             "PrepareDuplex48k: inherited GLOBAL_ENABLE=1; forcing disabled -> enabled restart edge");
    (void)io_.WriteQuadBE(
        MakeDICEAddress(sections_.global.offset + GlobalOffset::kEnable),
        0,
        [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus) mutable {
            const IOReturn status = MapTransportStatus(transportStatus);
            if (status != kIOReturnSuccess) {
                DoRollback(status, std::move(cb));
                return;
            }
            ActiveOperation()->preClaimDeviceEnabled = false;
            DoWriteClockSelect(channels, std::move(cb));
        });
}

void DICEDuplexBringupController::DoWriteClockSelect(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    // DICE clock selection is a transaction, not a set-if-different register
    // update. Linux writes GLOBAL_CLOCK_SELECT for every stream start, including
    // an equal value, then waits for CLOCK_ACCEPTED (or confirms the unchanged
    // clock after that wait expires). Some cold devices need this edge to arm
    // their device-to-host sample generator even though their stream geometry
    // already looks valid.
    // Cross-validated with Linux firewire/dice/dice-stream.c:60-98.
    const Operation* operation = ActiveOperation();
    if (operation->forceClockSelectWrite &&
        operation->preClaimClockSelect == diceClock_.clockSelect) {
        ASFW_LOG(DICE,
                 "[DiceGeom] re-selecting equal CLOCK_SELECT 0x%08x to refine cold stream geometry",
                 diceClock_.clockSelect);
    }

    NotificationMailbox::Reset();
    (void)io_.WriteQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kClockSelect),
                    diceClock_.clockSelect,
                    [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus) mutable {
                         const IOReturn status = MapTransportStatus(transportStatus);
                         if (status != kIOReturnSuccess) {
                             DoRollback(status, std::move(cb));
                             return;
                         }
                         // A CLOCK_SELECT write is transactional.  The device acknowledges it
                         // asynchronously; an already-locked status is stale evidence and must
                         // not complete this operation before CLOCK_ACCEPTED arrives.
                         const uint32_t earlyBits = NotificationMailbox::Consume();
                         if ((earlyBits & Notify::kClockAccepted) != 0) {
                             ASFW_LOG(DICE,
                                      "PrepareDuplex48k: CLOCK_ACCEPTED arrived during write, bits=0x%08x",
                                      earlyBits);
                             TransitionTo<AwaitingClockAcceptedState>();
                             DoReadGlobalAfterClockAccepted(
                                 channels, earlyBits, kIOReturnNotReady, std::move(cb));
                             return;
                         }
                         TransitionTo<AwaitingClockAcceptedState>();
                         DoWaitClockAccepted(channels, 0, std::move(cb));
                     });
}

void DICEDuplexBringupController::DoActiveClockCheck(
    AudioDuplexChannels channels,
    uint32_t accumulatedNotify,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    diceReader_.ReadGlobalStateFull(sections_,
                                    [this, channels, accumulatedNotify,
                                     cb = std::move(cb)](IOReturn status, GlobalState state) mutable {
                                if (status != kIOReturnSuccess) {
                                    // Read failed — fall through to mailbox polling
                                    ASFW_LOG(DICE,
                                             "PrepareDuplex48k: active clock check read failed (0x%08x), falling back to mailbox poll",
                                             status);
                                    DoWaitClockAccepted(channels, 0, std::move(cb));
                                    return;
                                }

                                // Accumulate any mailbox bits that arrived during the read
                                const uint32_t combinedNotify = accumulatedNotify | NotificationMailbox::Consume();

                                const bool clockAccepted =
                                    (combinedNotify & Notify::kClockAccepted) != 0;
                                const bool sourceLockedAtTarget =
                                    IsSourceLocked(state.status) &&
                                    NominalRateHz(state.status) == ActiveOperation()->desiredClock.sampleRateHz;
                                const bool sampleRateAtTarget =
                                    state.sampleRate == ActiveOperation()->desiredClock.sampleRateHz;

                                if (clockAccepted || (sourceLockedAtTarget && sampleRateAtTarget)) {
                                    ASFW_LOG(DICE,
                                             "PrepareDuplex48k: clock confirmed via active check "
                                             "(notify=0x%08x status=0x%08x rate=%u locked=%u)",
                                             combinedNotify, state.status, state.sampleRate,
                                             sourceLockedAtTarget ? 1U : 0U);
                                    if (ActiveOperation()->flow == FlowMode::kClockApply) {
                                        DoCompleteClockApply(std::move(cb));
                                        return;
                                    }
                                    DoAwaitStreamingClockLock(channels, 0, std::move(cb));
                                    return;
                                }

                                ASFW_LOG(DICE,
                                         "PrepareDuplex48k: active check not yet locked "
                                         "(notify=0x%08x status=0x%08x rate=%u), entering mailbox poll",
                                         combinedNotify, state.status, state.sampleRate);
                                DoWaitClockAccepted(channels, 0, std::move(cb));
                            });
}

void DICEDuplexBringupController::DoWaitClockAccepted(
    AudioDuplexChannels channels,
    uint32_t attempt,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    const uint32_t mailboxBits = NotificationMailbox::Consume();
    if ((mailboxBits & Notify::kClockAccepted) != 0) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: observed async CLOCK_ACCEPTED bits=0x%08x",
                 mailboxBits);
        DoReadGlobalAfterClockAccepted(
            channels, mailboxBits, kIOReturnNotReady, std::move(cb));
        return;
    }

    if (attempt * kPollIntervalMs >= kClockAcceptedTimeoutMs) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: CLOCK_ACCEPTED wait reached %u ms; performing final confirmation",
                 kClockAcceptedTimeoutMs);
        DoConfirmClockAccepted(channels, mailboxBits, std::move(cb));
        return;
    }

    if (!ScheduleRetry(kPollIntervalMs,
                       [this, channels, attempt, cb]() mutable {
                           DoWaitClockAccepted(channels, attempt + 1, std::move(cb));
                       })) {
        ASFW_LOG_ERROR(DICE,
                       "PrepareDuplex48k: cannot schedule CLOCK_ACCEPTED deadline continuation");
        DoRollback(kIOReturnNotReady, std::move(cb));
    }
}

void DICEDuplexBringupController::DoConfirmClockAccepted(
    AudioDuplexChannels channels,
    uint32_t observedNotify,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    const uint32_t lateMailboxBits = NotificationMailbox::Consume();
    if ((lateMailboxBits & Notify::kClockAccepted) != 0) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: observed late async CLOCK_ACCEPTED bits=0x%08x",
                 lateMailboxBits);
    }

    DoReadGlobalAfterClockAccepted(
        channels, observedNotify | lateMailboxBits, kIOReturnTimeout, std::move(cb));
}

void DICEDuplexBringupController::DoReadGlobalAfterClockAccepted(
    AudioDuplexChannels channels,
    uint32_t observedNotify,
    IOReturn failureStatus,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    diceReader_.ReadGlobalStateFull(sections_,
                                    [this,
                                     channels,
                                     observedNotify,
                                     failureStatus,
                                     cb = std::move(cb)](IOReturn status, GlobalState state) mutable {
                                if (status != kIOReturnSuccess) {
                                    DoRollback(status, std::move(cb));
                                    return;
                                }

                                // GLOBAL_NOTIFICATION describes device state; it is not an
                                // acknowledgement delivered to this host.  The latter is the
                                // asynchronous write captured by NotificationMailbox.  Do not
                                // let a stale/global bit satisfy the post-write barrier.
                                const uint32_t mailboxNotify =
                                    observedNotify | NotificationMailbox::Consume();
                                const uint32_t reportedNotify = mailboxNotify | state.notification;
                                const bool clockAccepted =
                                    (mailboxNotify & Notify::kClockAccepted) != 0;
                                const bool sourceLockedAtTarget =
                                    IsSourceLocked(state.status) &&
                                    NominalRateHz(state.status) == ActiveOperation()->desiredClock.sampleRateHz;
                                const bool sampleRateAtTarget =
                                    state.sampleRate == ActiveOperation()->desiredClock.sampleRateHz;

                                if (state.clockSelect != diceClock_.clockSelect) {
                                    ASFW_LOG(DICE,
                                             "PrepareDuplex48k: clock confirm failed, clockSelect=0x%08x notify=0x%08x status=0x%08x sampleRate=%u",
                                             state.clockSelect,
                                             reportedNotify,
                                             state.status,
                                             state.sampleRate);
                                    DoRollback(failureStatus, std::move(cb));
                                    return;
                                }

                                // An equal-value CLOCK_SELECT write is still issued. DICE does
                                // not require it to raise CLOCK_ACCEPTED, however; Linux accepts
                                // a post-timeout status confirmation in that case.
                                const bool canAcceptStatusWithoutNotify =
                                    ActiveOperation()->preClaimClockSelect == diceClock_.clockSelect;
                                if (!clockAccepted &&
                                    !(canAcceptStatusWithoutNotify && sourceLockedAtTarget && sampleRateAtTarget)) {
                                    ASFW_LOG(DICE,
                                             "PrepareDuplex48k: CLOCK_ACCEPTED not confirmed, notify=0x%08x status=0x%08x sampleRate=%u",
                                             reportedNotify,
                                             state.status,
                                             state.sampleRate);
                                    DoRollback(failureStatus, std::move(cb));
                                    return;
                                }

                                if (!clockAccepted) {
                                    ASFW_LOG(DICE,
                                             "PrepareDuplex48k: confirmed unchanged clock via global state after timeout, status=0x%08x sampleRate=%u",
                                             state.status,
                                             state.sampleRate);
                                }

                                if (ActiveOperation()->flow == FlowMode::kClockApply) {
                                    DoCompleteClockApply(std::move(cb));
                                    return;
                                }

                                DoAwaitStreamingClockLock(channels, 0, std::move(cb));
                            });
}

void DICEDuplexBringupController::DoAwaitStreamingClockLock(
    AudioDuplexChannels channels,
    uint32_t attempt,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    // CLOCK_ACCEPTED means the device received CLOCK_SELECT, not that the PLL
    // has reached the requested rate. Never enable streams at the old rate.
    // Most DICE products additionally require GLOBAL source lock here. The
    // narrowly scoped playback-only policy still waits for the target rate,
    // but lets host IT establish an otherwise unavailable receive-clock lock.
    diceReader_.ReadGlobalStateFull(
        sections_,
        [this, channels, attempt, cb = std::move(cb)](IOReturn status, GlobalState state) mutable {
            if (status != kIOReturnSuccess) {
                DoRollback(status, std::move(cb));
                return;
            }

            const bool rateAtTarget =
                NominalRateHz(state.status) == ActiveOperation()->desiredClock.sampleRateHz &&
                state.sampleRate == ActiveOperation()->desiredClock.sampleRateHz;
            const bool sourceLocked = IsSourceLocked(state.status);
            const bool readyForEnable = rateAtTarget &&
                                        (sourceLocked || !bringupPolicy_.requireSourceLockBeforeStreamEnable);

            if (readyForEnable) {
                if (attempt > 0) {
                    ASFW_LOG(DICE,
                             "PrepareDuplex48k: target rate ready at %u Hz after %u ms; sourceLock=%u; enabling streams",
                             state.sampleRate, attempt * kPollIntervalMs, sourceLocked ? 1U : 0U);
                }
                DoDiscoverStreams(channels, 0, std::move(cb));
                return;
            }

            if (attempt * kPollIntervalMs >= kStreamingClockLockTimeoutMs) {
                ASFW_LOG(DICE,
                         "PrepareDuplex48k: target rate%s not ready within %u ms (status=0x%08x rate=%u target=%u); aborting bring-up",
                         bringupPolicy_.requireSourceLockBeforeStreamEnable ? " and source lock" : "",
                         kStreamingClockLockTimeoutMs, state.status, state.sampleRate,
                         ActiveOperation()->desiredClock.sampleRateHz);
                DoRollback(kIOReturnTimeout, std::move(cb));
                return;
            }

            if (!ScheduleRetry(kPollIntervalMs,
                               [this, channels, attempt, cb]() mutable {
                                   DoAwaitStreamingClockLock(channels, attempt + 1, std::move(cb));
                               })) {
                ASFW_LOG_ERROR(DICE,
                               "PrepareDuplex48k: cannot schedule streaming clock-lock continuation");
                DoRollback(kIOReturnNotReady, std::move(cb));
            }
        });
}

void DICEDuplexBringupController::DoDiscoverStreams(
    AudioDuplexChannels channels,
    uint32_t step,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    const uint32_t txBase = sections_.txStreamFormat.offset;
    const uint32_t rxBase = sections_.rxStreamFormat.offset;

    switch (step) {
    case 0:
        (void)io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kNumber),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 1, std::move(cb));
                        });
        return;
    case 1:
        (void)io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kNumber),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 2, std::move(cb));
                        });
        return;
    case 2:
        (void)io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kSize),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 3, std::move(cb));
                        });
        return;
    case 3:
        (void)io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kIsochronous),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 4, std::move(cb));
                        });
        return;
    case 4:
        (void)io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kNumberAudio),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 5, std::move(cb));
                        });
        return;
    case 5:
        (void)io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kNumberMidi),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 6, std::move(cb));
                        });
        return;
    case 6:
        (void)io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kSpeed),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 7, std::move(cb));
                        });
        return;
    case 7:
        (void)io_.ReadBlock(MakeDICEAddress(txBase + TxOffset::kNames),
                      256,
                      [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, std::span<const uint8_t> payload) mutable {
                          const IOReturn status = MapTransportStatus(transportStatus);
                          if (status != kIOReturnSuccess || payload.size() < 256) {
                              DoRollback((status == kIOReturnSuccess) ? kIOReturnUnderrun : status, std::move(cb));
                              return;
                          }
                          DoDiscoverStreams(channels, 8, std::move(cb));
                      });
        return;
    case 8:
        (void)io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kSize),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 9, std::move(cb));
                        });
        return;
    case 9:
        (void)io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kIsochronous),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 10, std::move(cb));
                        });
        return;
    case 10:
        (void)io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kNumberMidi),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 11, std::move(cb));
                        });
        return;
    case 11:
        (void)io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kSeqStart),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 12, std::move(cb));
                        });
        return;
    case 12:
        (void)io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kNumberAudio),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 13, std::move(cb));
                        });
        return;
    case 13:
        (void)io_.ReadBlock(MakeDICEAddress(rxBase + RxOffset::kNames),
                      256,
                      [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, std::span<const uint8_t> payload) mutable {
                          const IOReturn status = MapTransportStatus(transportStatus);
                          if (status != kIOReturnSuccess || payload.size() < 256) {
                              DoRollback((status == kIOReturnSuccess) ? kIOReturnUnderrun : status, std::move(cb));
                              return;
                          }
                          DoFinishPrepare(std::move(cb));
                      });
        return;
    default:
        DoFinishPrepare(std::move(cb));
        return;
    }
}

void DICEDuplexBringupController::DoProgramRx(
    AudioDuplexChannels channels,
    uint32_t streamIndex,
    uint32_t entrySizeBytes,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    if (!std::holds_alternative<ProgrammingRxState>(state_)) {
        TransitionTo<ProgrammingRxState>();
    }

    // At stream 0, read RX_SIZE first to learn the per-stream register stride,
    // then re-enter this function with the resolved entry size for every stream.
    if (streamIndex == 0 && entrySizeBytes == 0) {
        (void)io_.ReadQuadBE(MakeDICEAddress(sections_.rxStreamFormat.offset + RxOffset::kSize),
                        [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t rxSize) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            ASFW_LOG(DICE,
                                     "DoProgramRx: RX_SIZE transport status=%u value=0x%08x streams=%u",
                                     static_cast<unsigned>(transportStatus),
                                     rxSize,
                                     channels.playbackStreamCount);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            const uint32_t stride = rxSize * 4u;
                            DoProgramRx(channels, 0, stride, std::move(cb));
                        });
        return;
    }

    if (streamIndex >= channels.playbackStreamCount) {
        // All RX streams programmed.
        TransitionTo<RxProgrammedState>();
        cb(kIOReturnSuccess);
        return;
    }

    const uint8_t isoChannel = channels.PlaybackChannel(streamIndex);
    const uint32_t streamBase =
        sections_.rxStreamFormat.offset + streamIndex * entrySizeBytes;
    ASFW_LOG(DICE,
             "DoProgramRx: stream %u writing RX isoch channel %u (stride=%u)",
             streamIndex, isoChannel, entrySizeBytes);

    (void)io_.WriteQuadBE(MakeDICEAddress(streamBase + RxOffset::kIsochronous),
                    isoChannel,
                    [this, channels, streamIndex, entrySizeBytes, streamBase, cb = std::move(cb)](Async::AsyncStatus isoTransportStatus) mutable {
                         const IOReturn isoStatus = MapTransportStatus(isoTransportStatus);
                         if (isoStatus != kIOReturnSuccess) {
                             DoRollback(isoStatus, std::move(cb));
                             return;
                         }

                         (void)io_.WriteQuadBE(MakeDICEAddress(streamBase + RxOffset::kSeqStart),
                                         kRxSeqStartDefault,
                                         [this, channels, streamIndex, entrySizeBytes, cb = std::move(cb)](Async::AsyncStatus seqTransportStatus) mutable {
                                              const IOReturn seqStatus = MapTransportStatus(seqTransportStatus);
                                              if (seqStatus != kIOReturnSuccess) {
                                                  DoRollback(seqStatus, std::move(cb));
                                                  return;
                                              }
                                              // Next RX stream.
                                              DoProgramRx(channels, streamIndex + 1, entrySizeBytes, std::move(cb));
                                          });
                     });
}

void DICEDuplexBringupController::DoProgramTx(
    AudioDuplexChannels channels,
    uint32_t streamIndex,
    uint32_t entrySizeBytes,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    if (!std::holds_alternative<ProgrammingTxState>(state_)) {
        TransitionTo<ProgrammingTxState>();
    }

    // At stream 0, read TX_SIZE first to learn the per-stream register stride.
    if (streamIndex == 0 && entrySizeBytes == 0) {
        (void)io_.ReadQuadBE(MakeDICEAddress(sections_.txStreamFormat.offset + TxOffset::kSize),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t txSize) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            const uint32_t stride = txSize * 4u;
                            DoProgramTx(channels, 0, stride, std::move(cb));
                        });
        return;
    }

    if (streamIndex >= channels.captureStreamCount) {
        // All TX streams programmed; assert the single GLOBAL_ENABLE last.
        DoEnableGlobal(channels, std::move(cb));
        return;
    }

    const uint8_t isoChannel = channels.CaptureChannel(streamIndex);
    const uint32_t streamBase =
        sections_.txStreamFormat.offset + streamIndex * entrySizeBytes;
    ASFW_LOG(DICE,
             "DoProgramTx: stream %u writing TX isoch channel %u (stride=%u)",
             streamIndex, isoChannel, entrySizeBytes);

    (void)io_.WriteQuadBE(MakeDICEAddress(streamBase + TxOffset::kIsochronous),
                    isoChannel,
                    [this, channels, streamIndex, entrySizeBytes, streamBase, cb = std::move(cb)](Async::AsyncStatus isoTransportStatus) mutable {
                         const IOReturn isoStatus = MapTransportStatus(isoTransportStatus);
                         if (isoStatus != kIOReturnSuccess) {
                             DoRollback(isoStatus, std::move(cb));
                             return;
                         }

                         (void)io_.WriteQuadBE(MakeDICEAddress(streamBase + TxOffset::kSpeed),
                                         kTxSpeedS400,
                                         [this, channels, streamIndex, entrySizeBytes, cb = std::move(cb)](Async::AsyncStatus speedTransportStatus) mutable {
                                              const IOReturn speedStatus = MapTransportStatus(speedTransportStatus);
                                              if (speedStatus != kIOReturnSuccess) {
                                                  DoRollback(speedStatus, std::move(cb));
                                                  return;
                                              }
                                              // Next TX stream.
                                              DoProgramTx(channels, streamIndex + 1, entrySizeBytes, std::move(cb));
                                          });
                     });
}

void DICEDuplexBringupController::DoEnableGlobal(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    (void)channels;
    if (!EnsureRouteCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    (void)io_.WriteQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kEnable),
                    1,
                    [this, cb = std::move(cb)](Async::AsyncStatus enableTransportStatus) mutable {
                         const IOReturn enableStatus = MapTransportStatus(enableTransportStatus);
                         if (enableStatus != kIOReturnSuccess) {
                             DoRollback(enableStatus, std::move(cb));
                             return;
                         }
                         TransitionTo<TxArmedState>();
                         cb(kIOReturnSuccess);
                     });
}

void DICEDuplexBringupController::DoFinishPrepare(VoidCallback cb) {
    if (!refreshRuntimeCapsOnPrepare_) {
        ActiveOperation()->appliedClock = ActiveOperation()->desiredClock;
        TransitionTo<PreparedState>();
        cb(kIOReturnSuccess);
        return;
    }

    RefreshRuntimeCaps([this, cb = std::move(cb)](IOReturn status) mutable {
        if (status != kIOReturnSuccess) {
            DoRollback(status, std::move(cb));
            return;
        }

        ActiveOperation()->appliedClock = ActiveOperation()->desiredClock;
        TransitionTo<PreparedState>();
        cb(kIOReturnSuccess);
    });
}

void DICEDuplexBringupController::DoCompleteClockApply(VoidCallback cb) {
    RefreshRuntimeCaps([this, cb = std::move(cb)](IOReturn status) mutable {
        if (status != kIOReturnSuccess) {
            DoRollback(status, std::move(cb));
            return;
        }

        ActiveOperation()->appliedClock = ActiveOperation()->desiredClock;
        ReleaseOwner([this, cb = std::move(cb)](IOReturn releaseStatus) mutable {
            if (releaseStatus != kIOReturnSuccess) {
                Operation failed = std::move(*ActiveOperation());
                state_ = FailedState{.operation = std::move(failed), .error = releaseStatus};
                cb(releaseStatus);
                return;
            }

            ResetOperation();
            cb(kIOReturnSuccess);
        });
    });
}

void DICEDuplexBringupController::RefreshRuntimeCaps(VoidCallback cb) {
    struct RuntimeCapsState {
        GlobalState global;
        StreamConfig tx;
        StreamConfig rx;
    };

    auto state = std::make_shared<RuntimeCapsState>();
    diceReader_.ReadGlobalState(
        sections_,
        [this, state, cb = std::move(cb)](IOReturn globalStatus, GlobalState global) mutable {
            if (globalStatus != kIOReturnSuccess) {
                cb(globalStatus);
                return;
            }

            state->global = global;
            diceReader_.ReadTxStreamConfig(
                sections_,
                [this, state, cb = std::move(cb)](IOReturn txStatus, StreamConfig tx) mutable {
                    if (txStatus != kIOReturnSuccess) {
                        cb(txStatus);
                        return;
                    }

                    state->tx = tx;
                    diceReader_.ReadRxStreamConfig(
                        sections_,
                        [this, state, cb = std::move(cb)](IOReturn rxStatus, StreamConfig rx) mutable {
                            if (rxStatus != kIOReturnSuccess) {
                                cb(rxStatus);
                                return;
                            }

                            state->rx = rx;
                            const AudioStreamRuntimeCaps previous = runtimeCaps_;
                            CacheRuntimeCaps(runtimeCaps_, state->global, state->tx, state->rx);
                            // Two geometry sources meet here. Probe time may have
                            // published the TCAT extension's rate-mode geometry
                            // (or the plain sections for whatever mode the device
                            // was sitting in); this post-clock re-read of the
                            // plain sections is what actually gets programmed.
                            // Once the clock is at the target rate the two must
                            // describe the same thing -- if they ever do not, the
                            // HAL is publishing channels the wire will not carry,
                            // which presents as a device that appears in Audio
                            // MIDI Setup and plays nothing.
                            if (previous.sampleRateHz != 0 &&
                                (previous.hostInputPcmChannels != runtimeCaps_.hostInputPcmChannels ||
                                 previous.hostOutputPcmChannels != runtimeCaps_.hostOutputPcmChannels ||
                                 previous.deviceToHostStreamCount != runtimeCaps_.deviceToHostStreamCount ||
                                 previous.hostToDeviceStreamCount != runtimeCaps_.hostToDeviceStreamCount)) {
                                ASFW_LOG(DICE,
                                         "[DiceGeom] post-clock geometry CHANGED at %u Hz: in %u->%u out %u->%u "
                                         "d2hStreams %u->%u h2dStreams %u->%u (published vs programmed now disagree)",
                                         runtimeCaps_.sampleRateHz,
                                         previous.hostInputPcmChannels, runtimeCaps_.hostInputPcmChannels,
                                         previous.hostOutputPcmChannels, runtimeCaps_.hostOutputPcmChannels,
                                         previous.deviceToHostStreamCount, runtimeCaps_.deviceToHostStreamCount,
                                         previous.hostToDeviceStreamCount, runtimeCaps_.hostToDeviceStreamCount);
                            }
                            ActiveOperation()->appliedClock = AudioClockConfig{
                                .sampleRateHz = state->global.sampleRate,
                            };
                            cb(kIOReturnSuccess);
                        });
                });
        });
}

void DICEDuplexBringupController::DoRollback(IOReturn error, VoidCallback cb) {
    CancelScheduledRetry();
    if (Operation* operation = ActiveOperation(); operation != nullptr) {
        Operation failed = std::move(*operation);
        state_ = FailedState{.operation = std::move(failed), .error = error};
    }

    if (!IsOwnerClaimed()) {
        ResetOperation();
        cb(error);
        return;
    }

    if (!EnsureRouteCurrent()) {
        ResetOperation();
        cb(error);
        return;
    }

    DoStopSequence(true, [this, error, cb = std::move(cb)](IOReturn stopStatus) mutable {
        if (stopStatus != kIOReturnSuccess) {
            ASFW_LOG(DICE, "DoRollback: cleanup reported 0x%x after start failure 0x%x", stopStatus, error);
        }
        cb(error);
    });
}

void DICEDuplexBringupController::DoPollSourceLock(
    uint32_t attempt,
    uint32_t accumulatedNotify,
    VoidCallback cb) {
    if (!EnsureRouteCurrent()) {
        (void)StopDuplex();
        cb(kIOReturnOffline);
        return;
    }

    uint32_t notify = accumulatedNotify | NotificationMailbox::Consume();
    (void)io_.ReadQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kStatus),
                   [this, attempt, notify, cb = std::move(cb)](Async::AsyncStatus statusTransport, uint32_t statusValue) mutable {
                        const IOReturn statusRead = MapTransportStatus(statusTransport);
                        if (statusRead != kIOReturnSuccess) {
                            (void)StopDuplex();
                            cb(statusRead);
                            return;
                        }

                        const bool sourceLocked = IsSourceLocked(statusValue);
                        if (sourceLocked || !bringupPolicy_.requireSourceLockAtConfirm) {
                            if (!sourceLocked) {
                                ASFW_LOG(DICE,
                                         "ConfirmDuplex48kStart: proceeding with advisory source lock "
                                         "status=0x%08x",
                                         statusValue);
                            }
                            (void)io_.ReadQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kNotification),
                                           [this, notify, statusValue, cb = std::move(cb)](Async::AsyncStatus notifyTransport, uint32_t nv) mutable {
                                                const IOReturn ns = MapTransportStatus(notifyTransport);
                                                if (ns == kIOReturnSuccess) {
                                                    notify |= nv;
                                                }
                                                (void)io_.ReadQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kExtStatus),
                                                               [this, notify, statusValue, cb = std::move(cb)](Async::AsyncStatus extTransport, uint32_t ev) mutable {
                                                                    const IOReturn es = MapTransportStatus(extTransport);
                                                                    const uint32_t extStatus =
                                                                        (es == kIOReturnSuccess) ? ev : 0;
                                                                    DoCompleteConfirm(
                                                                        notify, statusValue, extStatus,
                                                                        std::move(cb));
                                                                });
                                            });
                            return;
                        }

                        if (attempt * kPollIntervalMs >= kReadyTimeoutMs) {
                            ASFW_LOG_ERROR(DICE,
                                           "ConfirmDuplex48kStart: DICE clock failed to lock within %u ms (status=0x%08x)",
                                           kReadyTimeoutMs, statusValue);
                            (void)StopDuplex();
                            cb(kIOReturnTimeout);
                            return;
                        }

                        if (!ScheduleRetry(kPollIntervalMs,
                                           [this, attempt, notify, cb]() mutable {
                                               DoPollSourceLock(attempt + 1, notify, std::move(cb));
                                           })) {
                            ASFW_LOG_ERROR(DICE,
                                           "ConfirmDuplex48kStart: cannot schedule source-lock continuation");
                            DoRollback(kIOReturnNotReady, std::move(cb));
                        }
                    });
}

void DICEDuplexBringupController::DoCompleteConfirm(
    uint32_t notification,
    uint32_t status,
    uint32_t extStatus,
    VoidCallback cb) {
    confirmNotification_ = notification;
    confirmStatus_ = status;
    confirmExtStatus_ = extStatus;
    RefreshRuntimeCaps([this, notification, status, extStatus, cb = std::move(cb)](IOReturn refreshStatus) mutable {
        if (refreshStatus != kIOReturnSuccess) {
            (void)StopDuplex();
            cb(refreshStatus);
            return;
        }

        const Operation* operation = ActiveOperation();
        const bool channelsMatch =
            runtimeCaps_.deviceToHostIsoChannel == operation->channels.deviceToHostIsoChannel &&
            runtimeCaps_.hostToDeviceIsoChannel == operation->channels.hostToDeviceIsoChannel;
        if (!channelsMatch) {
            ASFW_LOG_ERROR(
                DICE,
                "ConfirmDuplex48kStart: stream channel readback mismatch expected d2h=%u h2d=%u actual d2h=%u h2d=%u",
                operation->channels.deviceToHostIsoChannel,
                operation->channels.hostToDeviceIsoChannel,
                runtimeCaps_.deviceToHostIsoChannel,
                runtimeCaps_.hostToDeviceIsoChannel);
            (void)StopDuplex();
            cb(kIOReturnNotReady);
            return;
        }

        ActiveOperation()->appliedClock = ActiveOperation()->desiredClock;
        TransitionTo<RunningState>();
        ASFW_LOG(DICE,
                 "ConfirmDuplex48kStart: sourceLock=%u notify=0x%08x status=0x%08x ext=0x%08x",
                 IsSourceLocked(status) ? 1U : 0U,
                 notification,
                 status,
                 extStatus);
        cb(kIOReturnSuccess);
    });
}

void DICEDuplexBringupController::ProgramRxForDuplex48k(VoidCallback callback) {
    if (!std::holds_alternative<PreparedState>(state_)) {
        callback(kIOReturnNotReady);
        return;
    }
    if (!EnsureRouteCurrent()) {
        callback(kIOReturnOffline);
        return;
    }

    const Operation* operation = ActiveOperation();
    if (operation->channels.deviceToHostIsoChannel > 63 ||
        operation->channels.hostToDeviceIsoChannel > 63) {
        callback(kIOReturnNotReady);
        return;
    }

    // Pass the full per-stream channel set so every advertised stream is armed.
    DoProgramRx(operation->channels, 0, 0, std::move(callback));
}

void DICEDuplexBringupController::ProgramTxAndEnableDuplex48k(VoidCallback callback) {
    if (!std::holds_alternative<RxProgrammedState>(state_)) {
        callback(kIOReturnNotReady);
        return;
    }

    if (!EnsureRouteCurrent()) {
        callback(kIOReturnOffline);
        return;
    }

    // Pass the full per-stream channel set; GLOBAL_ENABLE is asserted once after
    // every TX (and previously every RX) stream's ISOC register is written.
    DoProgramTx(ActiveOperation()->channels, 0, 0, std::move(callback));
}

void DICEDuplexBringupController::ConfirmDuplex48kStart(VoidCallback callback) {
    if (!std::holds_alternative<TxArmedState>(state_)) {
        callback(kIOReturnNotReady);
        return;
    }

    CancelScheduledRetry();
    TransitionTo<ConfirmingState>();
    NotificationMailbox::Reset();
    DoPollSourceLock(0, 0, std::move(callback));
}

IOReturn DICEDuplexBringupController::StopDuplex() {
    CancelScheduledRetry();
    if (!HasActiveOperation()) {
        return kIOReturnSuccess;
    }
    if (TeardownRequested()) {
        RecordStopTeardownAbort("Entry");
        ResetOperation();
        return kIOReturnAborted;
    }

    struct WaitState {
        std::atomic<bool> done{false};
        std::atomic<IOReturn> status{kIOReturnTimeout};
    };

    auto waitState = std::make_shared<WaitState>();
    DoStopSequence(true, [waitState](IOReturn status) {
        waitState->status.store(status, std::memory_order_relaxed);
        waitState->done.store(true, std::memory_order_release);
    });

    for (uint32_t waited = 0; waited < kStopSyncTimeoutMs; waited += kStopSyncPollMs) {
        if (waitState->done.load(std::memory_order_acquire)) {
            return waitState->status.load(std::memory_order_relaxed);
        }
        if (TeardownRequested()) {
            RecordStopTeardownAbort("Wait");
            ResetOperation();
            return kIOReturnAborted;
        }
        IOSleep(kStopSyncPollMs);
    }

    return waitState->done.load(std::memory_order_acquire)
        ? waitState->status.load(std::memory_order_relaxed)
        : kIOReturnTimeout;
}

void DICEDuplexBringupController::ReleaseOwner(VoidCallback callback) {
    CancelScheduledRetry();
    if (!IsOwnerClaimed()) {
        callback(kIOReturnSuccess);
        return;
    }
    if (TeardownRequested()) {
        RecordStopTeardownAbort("ReleaseOwnerEntry");
        callback(kIOReturnAborted);
        return;
    }

    if (!EnsureRouteCurrent()) {
        ActiveOperation()->ownerClaimed = false;
        callback(kIOReturnSuccess);
        return;
    }

    (void)io_.CompareSwap64BE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kOwnerHi),
                        OwnerValue(),
                        kOwnerNoOwner,
                        [this, callback = std::move(callback)](Async::AsyncStatus transportStatus, uint64_t previous) mutable {
                              const IOReturn status = MapTransportStatus(transportStatus);
                              if (status != kIOReturnSuccess) {
                                  callback(status);
                                  return;
                              }
                              if (previous != OwnerValue() && previous != kOwnerNoOwner) {
                                  callback(kIOReturnExclusiveAccess);
                                  return;
                              }

                              ActiveOperation()->ownerClaimed = false;
                              callback(kIOReturnSuccess);
                          });
}

void DICEDuplexBringupController::DoStopSequence(
    bool releaseOwner,
    VoidCallback cb) {
    stopSequenceError_ = kIOReturnSuccess;
    if (!std::holds_alternative<StoppingState>(state_)) {
        TransitionTo<StoppingState>();
    }
    if (AbortStopIfTeardown("SequenceEntry", cb)) {
        return;
    }
    DoStopDisableGlobal(releaseOwner, std::move(cb));
}

void DICEDuplexBringupController::DoStopDisableGlobal(
    bool releaseOwner,
    VoidCallback cb) {
    if (AbortStopIfTeardown("DisableGlobal", cb)) {
        return;
    }
    if (!EnsureRouteCurrent()) {
        RecordFirstError(stopSequenceError_, kIOReturnOffline);
        ResetOperation();
        cb(stopSequenceError_);
        return;
    }

    (void)io_.WriteQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kEnable),
                    0,
                    [this, releaseOwner, cb = std::move(cb)](Async::AsyncStatus transportStatus) mutable {
                         const IOReturn status = MapTransportStatus(transportStatus);
                         RecordFirstError(stopSequenceError_, status);
                         if (AbortStopIfTeardown("DisableGlobalComplete", cb)) {
                             return;
                         }
                         DoStopDisableTx(releaseOwner, std::move(cb));
                     });
}

void DICEDuplexBringupController::DoStopDisableTx(
    bool releaseOwner,
    VoidCallback cb) {
    if (AbortStopIfTeardown("DisableTx", cb)) {
        return;
    }
    if (!EnsureRouteCurrent()) {
        RecordFirstError(stopSequenceError_, kIOReturnOffline);
        cb(stopSequenceError_);
        return;
    }

    // Clear EVERY TX stream's ISOC register, not just stream[0]'s (FFADO
    // stopStreamByIndex writes 0xFFFFFFFF per stream). Read TX_SIZE for the
    // per-stream register stride, then walk the streams; if the stride read
    // fails, fall back to the legacy single-stream clear.
    (void)io_.ReadQuadBE(MakeDICEAddress(sections_.txStreamFormat.offset + TxOffset::kSize),
                   [this, releaseOwner, cb = std::move(cb)](Async::AsyncStatus readTransportStatus, uint32_t txSize) mutable {
                        const IOReturn readStatus = MapTransportStatus(readTransportStatus);
                        RecordFirstError(stopSequenceError_, readStatus);
                        if (AbortStopIfTeardown("DisableTxReadComplete", cb)) {
                            return;
                        }
                        const uint32_t stride =
                            (readStatus == kIOReturnSuccess) ? txSize * 4u : 0u;
                        DoStopDisableTxStream(0, stride, releaseOwner, std::move(cb));
                    });
}

void DICEDuplexBringupController::DoStopDisableTxStream(
    uint32_t streamIndex,
    uint32_t entrySizeBytes,
    bool releaseOwner,
    VoidCallback cb) {
    uint32_t streamCount = ActiveOperation()->channels.captureStreamCount;
    if (streamCount == 0) {
        streamCount = 1;
    } else if (streamCount > kMaxAudioStreamsPerDirection) {
        streamCount = kMaxAudioStreamsPerDirection;
    }
    // Without a usable stride only stream[0]'s registers are addressable.
    if (entrySizeBytes == 0) {
        streamCount = 1;
    }

    if (streamIndex >= streamCount) {
        DoStopReleaseTx(releaseOwner, std::move(cb));
        return;
    }

    const uint32_t streamBase =
        sections_.txStreamFormat.offset + streamIndex * entrySizeBytes;
    (void)io_.WriteQuadBE(MakeDICEAddress(streamBase + TxOffset::kIsochronous),
                    kDisabledIsoChannel,
                    [this, streamIndex, entrySizeBytes, streamBase, releaseOwner, cb = std::move(cb)](Async::AsyncStatus isoTransportStatus) mutable {
                         const IOReturn isoStatus = MapTransportStatus(isoTransportStatus);
                         RecordFirstError(stopSequenceError_, isoStatus);
                         if (AbortStopIfTeardown("DisableTxIsoComplete", cb)) {
                             return;
                         }
                         (void)io_.WriteQuadBE(MakeDICEAddress(streamBase + TxOffset::kSpeed),
                                         kTxSpeedS400,
                                         [this, streamIndex, entrySizeBytes, releaseOwner, cb = std::move(cb)](Async::AsyncStatus speedTransportStatus) mutable {
                                              const IOReturn speedStatus = MapTransportStatus(speedTransportStatus);
                                              RecordFirstError(stopSequenceError_, speedStatus);
                                              if (AbortStopIfTeardown("DisableTxSpeedComplete", cb)) {
                                                  return;
                                              }
                                              DoStopDisableTxStream(streamIndex + 1, entrySizeBytes, releaseOwner, std::move(cb));
                                          });
                     });
}

void DICEDuplexBringupController::DoStopReleaseTx(
    bool releaseOwner,
    VoidCallback cb) {
    if (AbortStopIfTeardown("ReleaseTx", cb)) {
        return;
    }
    DoStopDisableRx(releaseOwner, std::move(cb));
}

void DICEDuplexBringupController::DoStopDisableRx(
    bool releaseOwner,
    VoidCallback cb) {
    if (AbortStopIfTeardown("DisableRx", cb)) {
        return;
    }
    if (!EnsureRouteCurrent()) {
        RecordFirstError(stopSequenceError_, kIOReturnOffline);
        cb(stopSequenceError_);
        return;
    }

    // Clear EVERY RX stream's ISOC register, not just stream[0]'s (see
    // DoStopDisableTx). Stride from RX_SIZE; legacy single-stream clear if the
    // read fails.
    (void)io_.ReadQuadBE(MakeDICEAddress(sections_.rxStreamFormat.offset + RxOffset::kSize),
                   [this, releaseOwner, cb = std::move(cb)](Async::AsyncStatus readTransportStatus, uint32_t rxSize) mutable {
                        const IOReturn readStatus = MapTransportStatus(readTransportStatus);
                        RecordFirstError(stopSequenceError_, readStatus);
                        if (AbortStopIfTeardown("DisableRxReadComplete", cb)) {
                            return;
                        }
                        const uint32_t stride =
                            (readStatus == kIOReturnSuccess) ? rxSize * 4u : 0u;
                        DoStopDisableRxStream(0, stride, releaseOwner, std::move(cb));
                    });
}

void DICEDuplexBringupController::DoStopDisableRxStream(
    uint32_t streamIndex,
    uint32_t entrySizeBytes,
    bool releaseOwner,
    VoidCallback cb) {
    uint32_t streamCount = ActiveOperation()->channels.playbackStreamCount;
    if (streamCount == 0) {
        streamCount = 1;
    } else if (streamCount > kMaxAudioStreamsPerDirection) {
        streamCount = kMaxAudioStreamsPerDirection;
    }
    if (entrySizeBytes == 0) {
        streamCount = 1;
    }

    if (streamIndex >= streamCount) {
        DoStopReleaseRx(releaseOwner, std::move(cb));
        return;
    }

    const uint32_t streamBase =
        sections_.rxStreamFormat.offset + streamIndex * entrySizeBytes;
    (void)io_.WriteQuadBE(MakeDICEAddress(streamBase + RxOffset::kIsochronous),
                    kDisabledIsoChannel,
                    [this, streamIndex, entrySizeBytes, streamBase, releaseOwner, cb = std::move(cb)](Async::AsyncStatus isoTransportStatus) mutable {
                         const IOReturn isoStatus = MapTransportStatus(isoTransportStatus);
                         RecordFirstError(stopSequenceError_, isoStatus);
                         if (AbortStopIfTeardown("DisableRxIsoComplete", cb)) {
                             return;
                         }
                         (void)io_.WriteQuadBE(MakeDICEAddress(streamBase + RxOffset::kSeqStart),
                                         kRxSeqStartDefault,
                                         [this, streamIndex, entrySizeBytes, releaseOwner, cb = std::move(cb)](Async::AsyncStatus seqTransportStatus) mutable {
                                              const IOReturn seqStatus = MapTransportStatus(seqTransportStatus);
                                              RecordFirstError(stopSequenceError_, seqStatus);
                                              if (AbortStopIfTeardown("DisableRxSeqComplete", cb)) {
                                                  return;
                                              }
                                              DoStopDisableRxStream(streamIndex + 1, entrySizeBytes, releaseOwner, std::move(cb));
                                          });
                     });
}

void DICEDuplexBringupController::DoStopReleaseRx(
    bool releaseOwner,
    VoidCallback cb) {
    if (AbortStopIfTeardown("ReleaseRx", cb)) {
        return;
    }
    if (releaseOwner) {
        DoStopReleaseOwner(std::move(cb));
        return;
    }

    ResetOperation();
    cb(stopSequenceError_);
}

void DICEDuplexBringupController::DoStopReleaseOwner(VoidCallback cb) {
    if (AbortStopIfTeardown("ReleaseOwner", cb)) {
        return;
    }
    if (!IsOwnerClaimed()) {
        ResetOperation();
        cb(stopSequenceError_);
        return;
    }

    if (!EnsureRouteCurrent()) {
        RecordFirstError(stopSequenceError_, kIOReturnOffline);
        ResetOperation();
        cb(stopSequenceError_);
        return;
    }

    (void)io_.CompareSwap64BE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kOwnerHi),
                        OwnerValue(),
                        kOwnerNoOwner,
                        [this, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint64_t previous) mutable {
                              if (AbortStopIfTeardown("ReleaseOwnerComplete", cb)) {
                                  return;
                              }
                              const IOReturn status = MapTransportStatus(transportStatus);
                              RecordFirstError(stopSequenceError_, status);
                              if (status == kIOReturnSuccess &&
                                  previous != OwnerValue() &&
                                  previous != kOwnerNoOwner) {
                                  RecordFirstError(stopSequenceError_, kIOReturnExclusiveAccess);
                              }

                              ResetOperation();
                              cb(stopSequenceError_);
                          });
}

} // namespace ASFW::Audio::DICE
