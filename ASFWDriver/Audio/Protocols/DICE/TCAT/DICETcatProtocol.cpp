// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DICETcatProtocol.cpp - Generic DICE/TCAT protocol state and duplex control

#include "DICETcatProtocol.hpp"

#include "../../Duplex/FamilyStageWait.hpp"
#include "../../../Runtime/RateValidation.hpp"
#include "../Focusrite/SaffireproCommon.hpp"

#include "../../../../Logging/Logging.hpp"

#include <memory>
#include <utility>

namespace ASFW::Audio::DICE::TCAT {

namespace {

[[nodiscard]] bool HasUsableRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept {
    // CoreAudio visibility is not a wire-topology signal. A Weiss INT202, for
    // example, deliberately has zero HAL input channels while still reporting
    // and using a device->host DICE stream. Validate the physical DICE sections
    // through their slot counts instead.
    return caps.sampleRateHz != 0 &&
        caps.hostOutputPcmChannels != 0 &&
        caps.deviceToHostAm824Slots != 0 &&
        caps.hostToDeviceAm824Slots != 0;
}

void LogRuntimeCaps(const char* source, const AudioStreamRuntimeCaps& caps) {
    ASFW_LOG(DICE,
             "DICETcatProtocol: runtime caps source=%{public}s rate=%u in=%u out=%u d2hSlots=%u h2dSlots=%u usable=%u",
             source,
             caps.sampleRateHz,
             caps.hostInputPcmChannels,
             caps.hostOutputPcmChannels,
             caps.deviceToHostAm824Slots,
             caps.hostToDeviceAm824Slots,
             HasUsableRuntimeCaps(caps) ? 1U : 0U);
}

void LogStreamConfigSummary(const char* label, const StreamConfig& config) {
    ASFW_LOG(DICE,
             "DICETcatProtocol: %{public}s stream summary count=%u pcm=%u midi=%u am824=%u entrySize=%u parsedEntrySize=%u",
             label,
             config.numStreams,
             config.TotalPcmChannels(),
             config.TotalMidiPorts(),
             config.TotalAm824Slots(),
             config.entrySizeBytes,
             config.parsedEntrySizeBytes);
}

} // namespace

// Initial device setup runs on callbacks, including all waits. Saffire 4.3
// StartDev -> TimerFired uses two 200 ms ticks before RestartStreaming;
// RestartStreaming selects the clock before creating CoreAudio streams.
// Evidence (Saffire 4.3.0.41017 kext): StartDev 0xb654 arms the timer at
// 0xb872; initHardware sets the 200 ms interval at 0xa853; TimerFired counts
// two ticks at 0xaad9 before RestartStreaming at 0xa9e8, which writes
// CLOCK_SELECT at 0xe3fe before allocating streams at 0xe656.
struct DICETcatProtocol::InitialClockPreparation {
    std::atomic<bool> active{true};
    Discovery::DeviceRouteToken route;
    std::vector<VoidCallback> callbacks;
    Scheduling::TimerToken timer{Scheduling::kInvalidTimerToken};
    DiceClockConfiguration clock{};
    uint64_t owner{0};
    uint64_t startedNs{0};
    uint64_t acceptedNs{0};
    uint32_t notifications{0};
    uint32_t priorGlobalNotifications{0};
    uint32_t dspBase{0};
    bool accepted{false};
};

bool DICETcatProtocol::InitialClockCurrent(const std::shared_ptr<InitialClockPreparation>& state) {
    if (!state->active.load(std::memory_order_acquire)) return false;
    if (!initialized_ || (teardownCancel_ && teardownCancel_->load(std::memory_order_acquire))) {
        FinishInitialClock(state, kIOReturnAborted);
        return false;
    }
    if (io_.Route() != state->route || !io_.IsRouteCurrent()) {
        FinishInitialClock(state, kIOReturnOffline);
        return false;
    }
    return true;
}

void DICETcatProtocol::FinishInitialClock(const std::shared_ptr<InitialClockPreparation>& state,
                                        IOReturn status) {
    if (!state->active.exchange(false, std::memory_order_acq_rel)) return;
    if (state->timer != Scheduling::kInvalidTimerToken) preparationScheduler_->Cancel(state->timer);
    state->timer = Scheduling::kInvalidTimerToken;
    if (status == kIOReturnSuccess) {
        initializedRoute_ = state->route;
        driver_->AdoptPreparedClock(state->clock);
        selectedClock_ = {.sampleRateHz = state->clock.sampleRateHz};
    }
    if (initialClock_ == state) initialClock_.reset();
    ASFW_LOG(DICE, "[DiceInit] completed guid=%016llx gen=%u rate=%u kr=0x%x",
             guid_, state->route.generation.value, state->clock.sampleRateHz, status);
    auto callbacks = std::move(state->callbacks);
    for (auto& callback : callbacks) callback(status);
}

void DICETcatProtocol::CancelInitialClock() {
    if (initialClock_) {
        auto state = initialClock_;
        FinishInitialClock(state, kIOReturnAborted);
    }
}

void DICETcatProtocol::PrepareInitialClock(VoidCallback callback) {
    if (!initialized_ || !driver_ || !preparationScheduler_) {
        callback(kIOReturnNotReady);
        return;
    }
    const auto route = io_.Route();
    if (initializedRoute_ && *initializedRoute_ == route && io_.IsRouteCurrent()) {
        callback(kIOReturnSuccess);
        return;
    }
    if (initialClock_ && initialClock_->route == route) {
        initialClock_->callbacks.push_back(std::move(callback));
        return;
    }
    CancelInitialClock();
    ResetRuntimeCaps();
    // A reset invalidates the live observation, not previously measured rate
    // layouts for this protocol instance. Shutdown clears the retained catalog.
    sectionsLoaded_ = false;
    auto state = std::make_shared<InitialClockPreparation>();
    state->route = route;
    state->callbacks.push_back(std::move(callback));
    initialClock_ = state;
    EnsureSectionsLoaded([this, state](IOReturn status) {
        if (!state->active.load(std::memory_order_acquire)) return;
        if (!InitialClockCurrent(state)) return;
        if (status != kIOReturnSuccess) { FinishInitialClock(state, status); return; }
        const auto token = preparationScheduler_->ScheduleAfter(400'000'000ULL, [this, state] {
            if (!state->active.load(std::memory_order_acquire)) return;
            state->timer = Scheduling::kInvalidTimerToken;
            if (InitialClockCurrent(state)) InitialClockOwner(state);
        });
        state->timer = token;
        if (token == Scheduling::kInvalidTimerToken) FinishInitialClock(state, kIOReturnNoResources);
    });
}

void DICETcatProtocol::InitialClockOwner(const std::shared_ptr<InitialClockPreparation>& state) {
    diceReader_.ReadGlobalStateFull(sections_, [this, state](IOReturn status, GlobalState global) {
        if (!state->active.load(std::memory_order_acquire)) return;
        if (!InitialClockCurrent(state)) return;
        if (status != kIOReturnSuccess) { FinishInitialClock(state, status); return; }
        if (global.hasVersion && (global.version >> 24U) != kDiceSupportedMajorVersion) {
            FinishInitialClock(state, kIOReturnUnsupported); return;
        }
        // Inspect the observed mode before applying streaming admission. Its
        // geometry is needed to build the rate formations, including high rates.
        // Linux dice-stream.c:608-646 also reselects the current rate first.
        const uint32_t rate = DiceRateMode(global.sampleRate)
            ? global.sampleRate : DiceInitialRate(DiceDeviceRateMask(global.hasClockCaps, global.clockCaps));
        uint32_t clockSelect = 0;
        if (!DiceClockSelectForRate(rate, ClockSource::Internal, clockSelect)) {
            FinishInitialClock(state, kIOReturnUnsupported); return;
        }
        state->clock = {.sampleRateHz = rate, .clockSelect = clockSelect};
        if (!busInfo_.GetLocalNodeID().IsValid()) { FinishInitialClock(state, kIOReturnNotReady); return; }
        state->owner = ((0xFFC0ULL | (busInfo_.GetLocalNodeID().value & 0x3FU)) << kOwnerNodeShift) |
                       kNotificationHandlerOffset;
        (void)io_.CompareSwap64BE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kOwnerHi),
            kOwnerNoOwner, state->owner, [this, state](Async::AsyncStatus status, uint64_t previous) {
                if (!state->active.load(std::memory_order_acquire)) return;
                if (!InitialClockCurrent(state)) return;
                if (status != Async::AsyncStatus::kSuccess) {
                    FinishInitialClock(state, Protocols::Ports::MapAsyncStatusToIOReturn(status)); return;
                }
                if (previous != kOwnerNoOwner && previous != state->owner) {
                    FinishInitialClock(state, kIOReturnExclusiveAccess); return;
                }
                diceReader_.ReadGlobalStateFull(sections_, [this, state](IOReturn status, GlobalState global) {
                    if (!state->active.load(std::memory_order_acquire)) return;
                    if (!InitialClockCurrent(state)) return;
                    if (status != kIOReturnSuccess || global.owner != state->owner) {
                        FinishInitialClock(state, status != kIOReturnSuccess ? status : kIOReturnExclusiveAccess);
                        return;
                    }
                    state->priorGlobalNotifications = global.notification;
                    if (!runtimePolicy_.waitForInitialDsp) { InitialClockDisable(state); return; }
                    diceReader_.ReadExtensionSections([this, state](IOReturn status, ExtensionSections ext) {
                        if (!state->active.load(std::memory_order_acquire)) return;
                        if (!InitialClockCurrent(state)) return;
                        state->dspBase = ExtensionAbsoluteOffset(ext.application);
                        if (status != kIOReturnSuccess || state->dspBase == kDICEExtensionOffset ||
                            ext.application.size <= Focusrite::Offsets::kDspStatus) {
                            InitialClockDisable(state); return;
                        }
                        state->startedNs = preparationScheduler_->NowNs();
                        InitialClockDsp(state);
                    });
                });
            });
    });
}

void DICETcatProtocol::ScheduleInitialClockPoll(const std::shared_ptr<InitialClockPreparation>& state,
                                              uint32_t delayMs, bool dsp) {
    const auto token = preparationScheduler_->ScheduleAfter(uint64_t{delayMs} * 1'000'000ULL,
        [this, state, dsp] {
            if (!state->active.load(std::memory_order_acquire)) return;
            state->timer = Scheduling::kInvalidTimerToken;
            if (!InitialClockCurrent(state)) return;
            if (dsp) InitialClockDsp(state); else InitialClockPoll(state);
        });
    state->timer = token;
    if (token == Scheduling::kInvalidTimerToken) FinishInitialClock(state, kIOReturnNoResources);
}

void DICETcatProtocol::InitialClockDsp(const std::shared_ptr<InitialClockPreparation>& state) {
    (void)io_.ReadQuadBE(MakeDICEAddress(state->dspBase + Focusrite::Offsets::kDspStatus),
        [this, state](Async::AsyncStatus status, uint32_t value) {
            if (!state->active.load(std::memory_order_acquire)) return;
            if (!InitialClockCurrent(state)) return;
            const bool ready = status == Async::AsyncStatus::kSuccess &&
                (value & Focusrite::DspStatus::kRunning) && !(value & Focusrite::DspStatus::kBusy);
            if (ready) { InitialClockDisable(state); return; }
            if (preparationScheduler_->NowNs() - state->startedNs >= 5'000'000'000ULL) {
                FinishInitialClock(state, kIOReturnTimeout); return;
            }
            ScheduleInitialClockPoll(state, 50, true);
        });
}

void DICETcatProtocol::InitialClockDisable(const std::shared_ptr<InitialClockPreparation>& state) {
    if (!InitialClockCurrent(state)) return;
    // NEVER DELETE OR SKIP THIS WRITE, EVEN IF GLOBAL_ENABLE ALREADY READS 0.
    // It is a firmware command: DDR_MSG_DISABLE clears the board's started
    // latch (Pro24DSP 0x31278). A stale latch suppresses unmute on a same-rate
    // cold start. Linux dice-stream.c:256-294 also clears enable before clock
    // selection on the first reservation. Regression: DiceInitialPreparation.
    // DisablesDeviceBeforeColdClockEvenWhenAlreadyDisabled.
    (void)io_.WriteQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kEnable),
        0U, [this, state](Async::AsyncStatus status) {
            if (!InitialClockCurrent(state)) return;
            if (status != Async::AsyncStatus::kSuccess) {
                FinishInitialClock(state, Protocols::Ports::MapAsyncStatusToIOReturn(status)); return;
            }
            ASFW_LOG(DICE, "[DiceInit] GLOBAL_ENABLE=0 before clock rate=%u gen=%u",
                     state->clock.sampleRateHz, state->route.generation.value);
            InitialClockWrite(state);
        });
}

void DICETcatProtocol::InitialClockWrite(const std::shared_ptr<InitialClockPreparation>& state) {
    if (!InitialClockCurrent(state)) return;
    notifications_.Reset();
    state->startedNs = preparationScheduler_->NowNs();
    (void)io_.WriteQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kClockSelect),
        state->clock.clockSelect, [this, state](Async::AsyncStatus status) {
            if (!state->active.load(std::memory_order_acquire)) return;
            if (!InitialClockCurrent(state)) return;
            if (status != Async::AsyncStatus::kSuccess) {
                FinishInitialClock(state, Protocols::Ports::MapAsyncStatusToIOReturn(status)); return;
            }
            InitialClockPoll(state);
        });
}

void DICETcatProtocol::InitialClockPoll(const std::shared_ptr<InitialClockPreparation>& state) {
    // Both waits after the CLOCK_SELECT write are advisory, as in the TCAT
    // driver (PaeFireStudio RestartStreaming 0xd8e2): it waits up to 150 ms
    // for CLOCK_ACCEPTED and up to 1 s for lock, keeps waiting through a
    // failed STATUS read, logs a timeout and creates the streams anyway. Only
    // a failed write stops it. Stream start keeps its own lock gate.
    // Events arriving during a GLOBAL read overtake its snapshot: consume
    // them, but require another read before accepting that older locked state.
    state->notifications |= notifications_.Consume();
    diceReader_.ReadGlobalStateFull(sections_, [this, state](IOReturn status, GlobalState global) {
        if (!state->active.load(std::memory_order_acquire)) return;
        if (!InitialClockCurrent(state)) return;
        const uint64_t now = preparationScheduler_->NowNs();
        const bool expired = now - state->startedNs >= 3'000'000'000ULL;
        if (status != kIOReturnSuccess) {
            // A bus reset or a cancellation ends this preparation; the new
            // generation prepares again.
            if (status == kIOReturnOffline || status == kIOReturnAborted) {
                FinishInitialClock(state, status); return;
            }
            if (expired) {
                ASFW_LOG(DICE, "[DiceInit] clock unconfirmed guid=%016llx: GLOBAL read kr=0x%x at deadline; "
                         "continuing", guid_, status);
                FinishInitialClock(state, kIOReturnSuccess); return;
            }
            ScheduleInitialClockPoll(state, 10, false);
            return;
        }
        const uint32_t late = notifications_.Consume();
        // GLOBAL can retain flags from before this write. Only newly set
        // register bits or fresh mailbox writes belong to this transition.
        state->notifications |= (global.notification & ~state->priorGlobalNotifications) | late;
        const bool atTarget = global.clockSelect == state->clock.clockSelect &&
            NominalRateHz(global.status) == state->clock.sampleRateHz &&
            global.sampleRate == state->clock.sampleRateHz;
        const bool ready = atTarget &&
            (!runtimePolicy_.requireSourceLockBeforeStreamEnable || IsSourceLocked(global.status));
        if (!state->accepted && ((state->notifications & Notify::kClockAccepted) ||
                                now - state->startedNs >= 150'000'000ULL)) {
            if (!(state->notifications & Notify::kClockAccepted)) {
                ASFW_LOG(DICE, "[DiceInit] no CLOCK_ACCEPTED within 150 ms guid=%016llx; continuing", guid_);
            }
            state->accepted = true;
            state->acceptedNs = now;
        }
        const bool reconfigured = state->notifications & (Notify::kRxConfigChange | Notify::kTxConfigChange);
        const bool relocked = (state->notifications & Notify::kLockChange) ||
            (state->accepted && now - state->acceptedNs >= 1'000'000'000ULL);
        if (state->accepted && ready && (!reconfigured || relocked) &&
            !(late & (Notify::kLockChange | Notify::kRxConfigChange | Notify::kTxConfigChange))) {
            FinishInitialClock(state, kIOReturnSuccess); return;
        }
        if (expired) {
            ASFW_LOG(DICE, "[DiceInit] clock unconfirmed guid=%016llx: select=0x%08x status=0x%08x "
                     "rate=%u notify=0x%x; continuing", guid_, global.clockSelect, global.status,
                     global.sampleRate, state->notifications);
            FinishInitialClock(state, kIOReturnSuccess); return;
        }
        ScheduleInitialClockPoll(state, 10, false);
    });
}

bool DICETcatProtocol::MakeDiceClockConfiguration(
    const AudioClockConfig& requested, DiceClockConfiguration& out) noexcept {
    if (!IsSupportedAudioClockConfig(requested) &&
        !(Runtime::kDiceHardwareBatch && DiceRateMode(requested.sampleRateHz))) {
        return false;
    }
    // The DICE adapter owns the register encoding: Linux selects the requested
    // rate by updating GLOBAL_CLOCK_SELECT while preserving the source bits
    // (dice-stream.c:60-85; dice-interface.h:80-95). Encode the requested rate
    // via the standard table; source stays Internal (bring-up policy).
    uint32_t clockSelect = 0;
    if (!DiceClockSelectForRate(requested.sampleRateHz, ClockSource::Internal,
                                clockSelect)) {
        return false;
    }
    out = DiceClockConfiguration{
        .sampleRateHz = requested.sampleRateHz,
        .clockSelect = clockSelect,
    };
    return true;
}

DICETcatProtocol::DICETcatProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                   Protocols::Ports::FireWireBusInfo& busInfo,
                                   Discovery::DeviceRegistry& routeRegistry,
                                   const Discovery::DeviceRouteToken& route,
                                   ::ASFW::IRM::IRMClient* irmClient,
                                   DiceWaitClock& waitClock,
                                   DiceNotificationRouter* notifications,
                                   DICETcatRuntimePolicy runtimePolicy,
                                   Scheduling::ITimerScheduler* preparationScheduler)
    : busInfo_(busInfo)
    , irmClient_(irmClient)
    , io_(busOps, busInfo, routeRegistry, route)
    , diceReader_(io_)
    , deviceIo_(io_, diceReader_, waitClock)
    , notificationRouter_(notifications)
    , guid_(route.guid)
    , runtimePolicy_(runtimePolicy)
    , preparationScheduler_(preparationScheduler ? preparationScheduler : waitClock.Scheduler()) {
    if (notificationRouter_) {
        notificationRouter_->Register(guid_, notifications_);
    }
}

DICETcatProtocol::~DICETcatProtocol() {
    if (driver_) driver_->CancelReceiverLockWatch();
    CancelInitialClock();
    if (rateFormatsLock_) IOLockFree(rateFormatsLock_);
    if (notificationRouter_) {
        notificationRouter_->Unregister(guid_, notifications_);
    }
}

IOReturn DICETcatProtocol::Initialize() {
    if (!rateFormatsLock_) return kIOReturnNoMemory;
    if (!driver_) {
        driver_.emplace(deviceIo_, busInfo_, notifications_,
                        DICEBringupPolicy{
                            .requireSourceLockBeforeStreamEnable =
                                runtimePolicy_.requireSourceLockBeforeStreamEnable,
                            .requireSourceLockAtConfirm =
                                runtimePolicy_.requireSourceLockAtConfirm,
                            .clockChangeObserver =
                                runtimePolicy_.reloadRouterAfterClockChange ||
                                        runtimePolicy_.clockChangeObserver != nullptr
                                    ? static_cast<DiceClockChangeObserver*>(this)
                                    : nullptr,
                            .verifiedLoaderPeek = runtimePolicy_.verifiedLoaderPeek,
                        }, preparationScheduler_);
        driver_->SetTeardownCancelToken(teardownCancel_);
    }

    initialized_ = true;
    ASFW_LOG(DICE, "DICETcatProtocol::Initialize defers generic discovery until runtime");
    return kIOReturnSuccess;
}

IOReturn DICETcatProtocol::Shutdown() {
    if (driver_) driver_->CancelReceiverLockWatch();
    CancelInitialClock();
    initializedRoute_.reset();
    if (driver_) {
        if (driver_->IsPrepared() || driver_->IsRunning()) {
            const IOReturn stopStatus = driver_->Stop();
            if (stopStatus != kIOReturnSuccess && stopStatus != kIOReturnUnsupported) {
                ASFW_LOG(DICE, "DICETcatProtocol::Shutdown duplex stop failed: 0x%x (%{public}s)", stopStatus, ASFW::Logging::IOReturnName(stopStatus));
            }
        }
    }

    sections_ = {};
    sectionsLoaded_ = false;
    initialized_ = false;
    ResetRuntimeCaps();
    PublishRateFormations({});
    return kIOReturnSuccess;
}

bool DICETcatProtocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    if (!runtimeCapsValid_.load(std::memory_order_acquire)) {
        return false;
    }

    outCaps.sampleRateHz = runtimeSampleRateHz_.load(std::memory_order_relaxed);
    outCaps.deviceRateMask = deviceRateMask_.load(std::memory_order_relaxed);
    outCaps.hostInputPcmChannels = hostInputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.hostOutputPcmChannels = hostOutputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.deviceToHostAm824Slots = deviceToHostAm824Slots_.load(std::memory_order_relaxed);
    outCaps.hostToDeviceAm824Slots = hostToDeviceAm824Slots_.load(std::memory_order_relaxed);
    outCaps.deviceToHostIsoChannel =
        static_cast<uint8_t>(deviceToHostIsoChannel_.load(std::memory_order_relaxed));
    outCaps.hostToDeviceIsoChannel =
        static_cast<uint8_t>(hostToDeviceIsoChannel_.load(std::memory_order_relaxed));

    // Per-stream geometry: the runtimeCapsValid_ acquire-load above establishes
    // happens-before with the writer's release-store, so the plain arrays are
    // safe to read here.
    outCaps.deviceToHostStreamCount = deviceToHostStreamCount_.load(std::memory_order_relaxed);
    outCaps.hostToDeviceStreamCount = hostToDeviceStreamCount_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        outCaps.deviceToHostStreams[i] = deviceToHostStreams_[i];
        outCaps.hostToDeviceStreams[i] = hostToDeviceStreams_[i];
    }
    return true;
}

void DICETcatProtocol::EnsureRuntimeStreamGeometry(VoidCallback callback) {
    PrepareInitialClock([this, callback = std::move(callback)](IOReturn status) mutable {
        if (status != kIOReturnSuccess) { callback(status); return; }
        EnsureRuntimeCapsLoaded(std::move(callback));
    });
}

void DICETcatProtocol::ReadRateObservation(
    std::function<void(IOReturn, RateHardwareObservation)> callback) {
    if (!initialized_ || !sectionsLoaded_) { callback(kIOReturnNotReady, {}); return; }
    diceReader_.ReadCapabilities([this, callback = std::move(callback)](
        IOReturn status, DICECapabilities observed) mutable {
        if (status != kIOReturnSuccess) { callback(status, {}); return; }
        // The clock can move externally between GLOBAL and stream reads.
        // Read it again: a mixed snapshot cannot authorize a HAL projection.
        diceReader_.ReadGlobalState(sections_,
            [this, observed, callback = std::move(callback)](IOReturn status, GlobalState after) mutable {
                if (status != kIOReturnSuccess) { callback(status, {}); return; }
                const bool stable = after.sampleRate == observed.global.sampleRate &&
                    after.clockSelect == observed.global.clockSelect &&
                    NominalRateHz(after.status) == after.sampleRate &&
                    std::ranges::any_of(kDiceRateTable, [&](const auto& rate) {
                        return rate.hz == after.sampleRate && rate.rateIndex ==
                            ((after.clockSelect & ClockSelect::kRateMask) >> ClockSelect::kRateShift);
                    });
                const bool locked = (!runtimePolicy_.requireSourceLockAtConfirm || IsSourceLocked(after.status)) &&
                    ((after.clockSelect & ClockSelect::kSourceMask) != static_cast<uint32_t>(ClockSource::ARX1) ||
                     (IsArx1Locked(after.extStatus) && !HasArx1Slip(after.extStatus)));
                const auto caps = MakeDiceRuntimeCaps(after, observed.txStreams,
                    observed.rxStreams, runtimePolicy_.exposeDeviceToHostToCoreAudio);
                if (stable && locked) {
                    const auto measured = DiceObservedRateFormation(caps,
                        runtimePolicy_.exposeDeviceToHostToCoreAudio);
                    const auto known = RateFormations();
                    if (measured && (!known || std::ranges::find(*known, measured->sampleRateHz,
                        &Runtime::RateFormation::sampleRateHz) == known->end()))
                        PublishRateFormations(std::make_shared<const std::vector<Runtime::RateFormation>>(
                            std::vector<Runtime::RateFormation>{*measured}));
                }
                callback(kIOReturnSuccess, {caps, stable && locked});
            });
    });
}

void DICETcatProtocol::SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept {
    teardownCancel_ = cancel;
    if (driver_) {
        driver_->SetTeardownCancelToken(cancel);
    }
}

// ---------------------------------------------------------------------------
// FamilyDriver
// ---------------------------------------------------------------------------

IOReturn DICETcatProtocol::LoadGeometry() {
    // Publication owns callback-driven initial preparation. This synchronous
    // session stage must never wait for a timer on its own dispatch queue.
    return AwaitStageStatus([&](auto callback) { EnsureRuntimeCapsLoaded(std::move(callback)); },
                            teardownCancel_);
}

std::optional<AudioStreamRuntimeCaps> DICETcatProtocol::RuntimeCaps() const {
    AudioStreamRuntimeCaps caps{};
    if (!GetRuntimeAudioStreamCaps(caps)) {
        return std::nullopt;
    }
    return caps;
}

std::expected<DuplexPrepareResult, IOReturn> DICETcatProtocol::Configure(
    const AudioDuplexChannels& channels, const AudioClockConfig& clock) {
    if (!initialized_ || !driver_) {
        return std::unexpected(kIOReturnNotReady);
    }

    DiceClockConfiguration diceClock{};
    if (!MakeDiceClockConfiguration(clock, diceClock)) {
        return std::unexpected(kIOReturnUnsupported);
    }

    // Remember the live clock so a later per-StartIO PrepareDuplex48k targets it
    // rather than reverting the device to 48 kHz (see selectedClock_).
    if (clock.sampleRateHz != 0) {
        selectedClock_ = clock;
    }

    const auto result = driver_->Prepare(channels, diceClock, /*refreshRuntimeCaps=*/true);
    if (result) {
        CacheRuntimeCaps(result->runtimeCaps);
        // Device configuration, not a bring-up gate: a device that needs a
        // router program comes up silent without one, but the streams are
        // sound either way, so a failure here is logged and not propagated.
        ApplyStartupProgram();
        // MixControl downloads the router once when it attaches to a device
        // (its cache starts empty); a start that changed the rate did it already.
        if (runtimePolicy_.reloadRouterAfterClockChange && !routerReloadedSinceAttach_) {
            ReloadRouterPerRateMode(deviceIo_, "first start");
        }
    }
    return result;
}

// --- Startup router program ---------------------------------------------------

std::expected<uint32_t, IOReturn> DICETcatProtocol::ReadRouterEntryCount(
    const ExtensionSections& ext) {
    // The *current* configuration is what the device is really holding; the
    // router section is only a staging buffer, and reading it on a freshly
    // attached device returns zero even though the device is working.
    const uint32_t base = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.currentConfig);
    if (base == kDICEExtensionOffset) {
        return std::unexpected(kIOReturnUnsupported);
    }
    return deviceIo_.ReadQuad(base);
}

std::expected<void, IOReturn> DICETcatProtocol::WriteRouterProgram(
    const ExtensionSections& ext) {
    const uint32_t routerBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.router);
    const uint32_t commandBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.command);
    if (routerBase == kDICEExtensionOffset || commandBase == kDICEExtensionOffset) {
        ASFW_LOG(DICE, "Startup router: device exposes no TCAT router/command section");
        return std::unexpected(kIOReturnUnsupported);
    }

    const uint32_t count = runtimePolicy_.startupRouterEntryCount;
    // Wire image: quadlet 0 is the entry count, then one quadlet per entry.
    std::vector<uint8_t> buffer(static_cast<size_t>(count + 1U) * 4U, 0U);
    FW::WriteBE32(buffer.data(), count);
    for (uint32_t i = 0; i < count; ++i) {
        FW::WriteBE32(buffer.data() + (i + 1U) * 4U, runtimePolicy_.startupRouterEntries[i]);
    }

    if (const auto written = deviceIo_.WriteBlock(routerBase, buffer); !written) {
        ASFW_LOG(DICE, "Startup router: write failed (0x%08x)", written.error());
        return std::unexpected(written.error());
    }

    // Commit for every rate mode, as the vendor driver does. A commit of the
    // low mode alone is accepted and does not take effect.
    const uint32_t opcode = ExtensionCommandOpcode::kExecute |
                            ExtensionCommandOpcode::kRateLow |
                            ExtensionCommandOpcode::kRateMiddle |
                            ExtensionCommandOpcode::kRateHigh |
                            ExtensionCommandOpcode::kLoadRouter;
    if (const auto committed =
            deviceIo_.WriteQuad(commandBase + ExtensionCommandOffset::kOpcode, opcode);
        !committed) {
        ASFW_LOG(DICE, "Startup router: commit failed (0x%08x)", committed.error());
        return std::unexpected(committed.error());
    }

    ASFW_LOG(DICE, "Startup router: programmed %u entries (opcode=0x%08x)", count, opcode);
    return {};
}

void DICETcatProtocol::WriteStartupMixerCells(const ExtensionSections& ext) {
    if (runtimePolicy_.startupMixerCells == nullptr ||
        runtimePolicy_.startupMixerCellCount == 0) {
        return;
    }
    const uint32_t mixerBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.mixer);
    if (mixerBase == kDICEExtensionOffset) {
        return;
    }
    // Coefficients start one quadlet past the section base.
    uint32_t written = 0;
    for (uint32_t i = 0; i < runtimePolicy_.startupMixerCellCount; ++i) {
        const auto& cell = runtimePolicy_.startupMixerCells[i];
        if (deviceIo_.WriteQuad(mixerBase + 4U + cell.index * 4U, cell.gain)) {
            ++written;
        } else {
            // One bad cell should not abort the rest.
            ASFW_LOG(DICE, "Startup mixer: cell %u failed", cell.index);
        }
    }
    ASFW_LOG(DICE, "Startup mixer: wrote %u of %u coefficients", written,
             runtimePolicy_.startupMixerCellCount);
}

// --- Firmware monitor cells ---------------------------------------------------
//
// The firmware does not put these back while the device stays powered (watched
// for 70 s) but restores all of them on the next power-on, which builds a new
// protocol instance; so this runs once per instance.
//
// A write that reports success is not proof the cell is clear, the same way a
// router commit is not proof the table is there: read them back and retry the
// set once. A leak that survives both passes is logged and left, because a loop
// here would fight the firmware on every bring-up.
void DICETcatProtocol::ClearFirmwareMonitorCells() {
    const uint16_t* cells = runtimePolicy_.postStartMixerZeros;
    const uint32_t count = runtimePolicy_.postStartMixerZeroCount;
    if (cells == nullptr || count == 0 || firmwareMonitorCellsCleared_) {
        return;
    }
    const auto ext = deviceIo_.ReadExtensionSections();
    if (!ext) {
        ASFW_LOG(DICE, "Monitor cells: extension sections unreadable (0x%08x)", ext.error());
        return;
    }
    const uint32_t mixerBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext->mixer);
    if (mixerBase == kDICEExtensionOffset) {
        return;
    }

    for (uint32_t attempt = 0; attempt < 2U; ++attempt) {
        // Coefficients start one quadlet past the section base.
        for (uint32_t i = 0; i < count; ++i) {
            if (!deviceIo_.WriteQuad(mixerBase + 4U + cells[i] * 4U, 0U)) {
                ASFW_LOG(DICE, "Monitor-cell clear %u failed", cells[i]);
            }
        }
        uint32_t stillSet = 0;
        uint32_t unread = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto gain = deviceIo_.ReadQuad(mixerBase + 4U + cells[i] * 4U);
            if (!gain) {
                ++unread;
            } else if ((*gain & 0xFFFFU) != 0) {
                ++stillSet;
            }
        }
        if (stillSet == 0 && unread == 0) {
            ASFW_LOG(DICE, "Monitor cells cleared: %u of %u", count, count);
            firmwareMonitorCellsCleared_ = true;
            return;
        }
        ASFW_LOG(DICE, "Monitor cells: %u still set, %u unread (of %u)%{public}s",
                 stillSet, unread, count, attempt == 0U ? "; retrying once" : "; leaving them");
    }
}

// --- Front-panel LED ----------------------------------------------------------
//
// The Mbox Pro powers up with UILEDState (application section +0x14) already
// holding its value - 0x121, the speaker LED on green - but the LED stays dark
// until a host writes the register. Writing the very value it holds back lights
// it, without choosing a speaker set on the user's behalf. Once per instance,
// like the monitor cells: the firmware keeps it lit until the next power-on.
void DICETcatProtocol::RewriteFrontPanelLed() {
    const uint32_t offset = runtimePolicy_.frontPanelLedRewriteOffset;
    if (offset == 0 || frontPanelLedRewritten_) {
        return;
    }
    const auto ext = deviceIo_.ReadExtensionSections();
    if (!ext) {
        return;
    }
    const uint32_t appBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext->application);
    if (appBase == kDICEExtensionOffset) {
        return;
    }
    const auto value = deviceIo_.ReadQuad(appBase + offset);
    if (!value) {
        ASFW_LOG(DICE, "Front-panel LED: read failed (0x%08x)", value.error());
        return;
    }
    if (!deviceIo_.WriteQuad(appBase + offset, *value)) {
        ASFW_LOG(DICE, "Front-panel LED: write failed (value=0x%08x)", *value);
        return;
    }
    frontPanelLedRewritten_ = true;
    ASFW_LOG(DICE, "Front-panel LED rewritten (value=0x%08x)", *value);
}

void DICETcatProtocol::BeforeClockSelectWrite(DiceDeviceIo& io) noexcept {
    if (runtimePolicy_.clockChangeObserver != nullptr) {
        runtimePolicy_.clockChangeObserver->BeforeClockSelectWrite(io);
    }
}

void DICETcatProtocol::AfterClockChange(DiceDeviceIo& io, uint32_t rateHz, bool rateChanged) noexcept {
    if (runtimePolicy_.clockChangeObserver != nullptr) {
        runtimePolicy_.clockChangeObserver->AfterClockChange(io, rateHz, rateChanged);
    }
    if (runtimePolicy_.reloadRouterAfterClockChange && rateChanged) {
        ReloadRouterPerRateMode(io, "rate change");
    }
}

namespace {

// MixControl tcat::dice::eap_impl_dice2::executeOpcode gives up after 400 ms
// (kEAP_CMD_TIMEOUT); it waits for a previous command the same way first.
constexpr uint32_t kExtensionCommandTimeoutMs = 400;
constexpr uint32_t kExtensionCommandPollMs = 10;

// Polls the command register until EXECUTE clears. Returns the last opcode
// read, or nothing on a read failure or timeout.
[[nodiscard]] std::optional<uint32_t> AwaitExtensionCommandIdle(DiceDeviceIo& io,
                                                                uint32_t commandBase) {
    const uint64_t startMs = io.Clock().NowMs();
    for (;;) {
        const auto opcode = io.ReadQuad(commandBase + ExtensionCommandOffset::kOpcode);
        if (!opcode) {
            return std::nullopt;
        }
        if ((*opcode & ExtensionCommandOpcode::kExecute) == 0) {
            return *opcode;
        }
        if (io.Clock().NowMs() - startMs >= kExtensionCommandTimeoutMs) {
            return std::nullopt;
        }
        io.Clock().SleepMs(kExtensionCommandPollMs);
    }
}

} // namespace

void DICETcatProtocol::ReloadRouterPerRateMode(DiceDeviceIo& io, const char* why) noexcept {
    routerReloadedSinceAttach_ = true;
    const auto ext = io.ReadExtensionSections();
    if (!ext) {
        ASFW_LOG(DICE, "[RouterReload] %{public}s: extension sections unreadable (0x%08x)", why,
                 ext.error());
        return;
    }
    if (ext->router.size < 8 || ext->command.size < 8 || ext->currentConfig.size == 0) {
        ASFW_LOG(DICE, "[RouterReload] %{public}s: device exposes no TCAT router", why);
        return;
    }
    const uint32_t routerBase = ExtensionAbsoluteOffset(ext->router);
    const uint32_t commandBase = ExtensionAbsoluteOffset(ext->command);
    const uint32_t maxEntries = ext->router.size / 4U - 1U;

    struct RateMode {
        const char* name;
        uint32_t currentConfigOffset;
        uint32_t rateFlag;
    };
    constexpr RateMode kModes[] = {
        {"low", CurrentConfigOffset::kLowRouter, ExtensionCommandOpcode::kRateLow},
        {"mid", CurrentConfigOffset::kMiddleRouter, ExtensionCommandOpcode::kRateMiddle},
        {"high", CurrentConfigOffset::kHighRouter, ExtensionCommandOpcode::kRateHigh},
    };
    for (const auto& mode : kModes) {
        if (mode.currentConfigOffset + 4U > ext->currentConfig.size) {
            continue;
        }
        const uint32_t tableBase = ExtensionAbsoluteOffset(ext->currentConfig, mode.currentConfigOffset);
        const auto count = io.ReadQuad(tableBase);
        if (!count) {
            ASFW_LOG(DICE, "[RouterReload] %{public}s: %{public}s table unreadable (0x%08x)", why,
                     mode.name, count.error());
            return;
        }
        if (*count == 0) {
            continue;  // the device has no program for this rate mode
        }
        if (*count > maxEntries) {
            ASFW_LOG(DICE, "[RouterReload] %{public}s: %{public}s table has %u entries, router "
                     "section holds %u; skipped", why, mode.name, *count, maxEntries);
            continue;
        }
        const auto table = io.ReadBlock(tableBase, 4U + *count * 4U);
        if (!table || table->size() != 4U + *count * 4U) {
            ASFW_LOG(DICE, "[RouterReload] %{public}s: %{public}s table read failed", why, mode.name);
            return;
        }
        if (!AwaitExtensionCommandIdle(io, commandBase)) {
            ASFW_LOG(DICE, "[RouterReload] %{public}s: a previous extension command is still "
                     "running; stopped", why);
            return;
        }
        if (const auto written = io.WriteBlock(routerBase, *table); !written) {
            ASFW_LOG(DICE, "[RouterReload] %{public}s: %{public}s router write failed (0x%08x)", why,
                     mode.name, written.error());
            return;
        }
        const uint32_t opcode = ExtensionCommandOpcode::kExecute | mode.rateFlag |
                                ExtensionCommandOpcode::kLoadRouter;
        if (const auto sent = io.WriteQuad(commandBase + ExtensionCommandOffset::kOpcode, opcode);
            !sent) {
            ASFW_LOG(DICE, "[RouterReload] %{public}s: %{public}s load command failed (0x%08x)", why,
                     mode.name, sent.error());
            return;
        }
        if (!AwaitExtensionCommandIdle(io, commandBase)) {
            ASFW_LOG(DICE, "[RouterReload] %{public}s: %{public}s load did not complete within "
                     "%u ms", why, mode.name, kExtensionCommandTimeoutMs);
            return;
        }
        const auto result = io.ReadQuad(commandBase + ExtensionCommandOffset::kReturn);
        ASFW_LOG(DICE, "[RouterReload] %{public}s: %{public}s %u entries loaded (opcode=0x%08x "
                 "return=0x%08x)", why, mode.name, *count, opcode, result ? *result : 0xFFFFFFFFU);
    }
}

void DICETcatProtocol::ApplyStartupProgram() {
    if (runtimePolicy_.startupRouterEntries == nullptr ||
        runtimePolicy_.startupRouterEntryCount == 0) {
        return;
    }

    const auto ext = deviceIo_.ReadExtensionSections();
    if (!ext) {
        ASFW_LOG(DICE, "Startup program: extension sections unreadable (0x%08x)", ext.error());
        return;
    }

    // Do not latch on the first success. A burst of bus resets around bring-up
    // can leave the device holding nothing while every write still reports
    // success, and a latch then guarantees nobody ever puts the program back:
    // the device stays silent until someone reprograms it by hand. Read the
    // count the device is really holding and reprogram whenever it disagrees.
    const uint32_t want = runtimePolicy_.startupRouterEntryCount;
    const auto live = ReadRouterEntryCount(*ext);
    if (startupProgramApplied_ && live && *live == want) {
        return;
    }
    if (startupProgramApplied_) {
        ASFW_LOG(DICE, "Startup router lost: device holds %u of %u entries; reprogramming",
                 live ? *live : 0U, want);
        startupProgramApplied_ = false;
    }

    if (!WriteRouterProgram(*ext)) {
        return;
    }
    WriteStartupMixerCells(*ext);
    startupProgramApplied_ = true;
}

std::expected<AudioDuplexChannels, IOReturn> DICETcatProtocol::AssignChannels(const AudioDuplexChannels& channels) {
    // The IRM chose these channels; the device must be told the same ones the
    // host DMA will use. ArmDeviceRx refuses to run unprepared, so a refusal
    // here cannot leave the two sides on different channels.
    if (!driver_) {
        return std::unexpected(kIOReturnNotReady);
    }
    if (const IOReturn status = driver_->AssignChannels(channels); status != kIOReturnSuccess) {
        ASFW_LOG_ERROR(DICE, "AssignChannels: refused d2h=%u h2d=%u kr=0x%x (%{public}s)",
                       channels.deviceToHostIsoChannel, channels.hostToDeviceIsoChannel, status, ASFW::Logging::IOReturnName(status));
        return std::unexpected(status);
    }
    return channels;
}

std::expected<DuplexHealthResult, IOReturn> DICETcatProtocol::ReadHealth(uint32_t timeoutMs) {
    return AwaitStage<DuplexHealthResult>(
        [&](auto callback) { ReadDuplexHealth(std::move(callback)); }, teardownCancel_, timeoutMs);
}

std::expected<DuplexStageResult, IOReturn> DICETcatProtocol::ArmDeviceRx() {
    if (!initialized_ || !driver_) {
        return std::unexpected(kIOReturnNotReady);
    }
    return driver_->ProgramRx();
}

std::expected<DuplexStageResult, IOReturn> DICETcatProtocol::ArmDeviceTxAndEnable() {
    if (!initialized_ || !driver_) {
        return std::unexpected(kIOReturnNotReady);
    }
    return driver_->ProgramTxAndEnable();
}

std::expected<DuplexConfirmResult, IOReturn> DICETcatProtocol::Confirm() {
    if (!initialized_ || !driver_) {
        return std::unexpected(kIOReturnNotReady);
    }
    const auto result = driver_->Confirm();
    if (result) {
        CacheRuntimeCaps(result->runtimeCaps);
        // The device has its clock and the stream is confirmed: the earliest
        // moment its own monitor matrix is safe to clear.
        ClearFirmwareMonitorCells();
        RewriteFrontPanelLed();
    }
    return result;
}

std::expected<DuplexClockApplyResult, IOReturn> DICETcatProtocol::ApplyClockIdle(
    const AudioClockConfig& clock) {
    if (!initialized_ || !driver_) {
        return std::unexpected(kIOReturnNotReady);
    }

    DiceClockConfiguration diceClock{};
    if (!MakeDiceClockConfiguration(clock, diceClock)) {
        return std::unexpected(kIOReturnUnsupported);
    }

    // Remember an idle rate change so the next StartIO's PrepareDuplex48k keeps
    // the device at this rate instead of rewriting CLOCK_SELECT back to 48 kHz
    // (see selectedClock_).
    if (clock.sampleRateHz != 0) {
        selectedClock_ = clock;
    }

    const auto result = driver_->ApplyClock(diceClock);
    if (result) {
        CacheRuntimeCaps(result->runtimeCaps);
        initializedRoute_ = io_.Route();
    }
    return result;
}

IOReturn DICETcatProtocol::DisconnectPlayback() {
    // DICE has no per-direction connection to drop: its stop is one sequence
    // (GLOBAL_ENABLE off, every stream's ISOCHRONOUS cleared). The staged-stop
    // recipe that calls this is not DICE's.
    return kIOReturnUnsupported;
}

IOReturn DICETcatProtocol::DisconnectCapture() {
    // As DisconnectPlayback.
    return kIOReturnUnsupported;
}

IOReturn DICETcatProtocol::BreakConnections() {
    // No CMP connections to break; Stop undoes everything the bring-up armed.
    return kIOReturnUnsupported;
}

IOReturn DICETcatProtocol::Stop() {
    return StopDuplex();
}

void DICETcatProtocol::ReadDuplexHealth(HealthCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    EnsureSectionsLoaded([this, callback = std::move(callback)](IOReturn sectionStatus) mutable {
        if (sectionStatus != kIOReturnSuccess) {
            callback(sectionStatus, {});
            return;
        }

        // Linux dice-interface.h:155-159: block health reads do not clear
        // slip latches. Feed this existing notification probe into the watch;
        // the watch alone issues the sparse destructive EXT_STATUS quad reads.
        auto observe = driver_ ? driver_->ReceiverLockObserver()
                               : std::function<void(IOReturn, uint32_t, uint32_t)>{};
        diceReader_.ReadGlobalState(
            sections_,
            [this, observe = std::move(observe), callback = std::move(callback)](IOReturn status, GlobalState global) mutable {
                if (observe) observe(status, global.status, global.extStatus);
                if (status != kIOReturnSuccess) {
                    callback(status, {});
                    return;
                }

                AudioStreamRuntimeCaps caps{};
                (void)GetRuntimeAudioStreamCaps(caps);
                const uint32_t clockSource =
                    global.clockSelect & ClockSelect::kSourceMask;
                const bool clockReferenceHealthy =
                    clockSource != static_cast<uint32_t>(ClockSource::ARX1) ||
                    (IsArx1Locked(global.extStatus) && !HasArx1Slip(global.extStatus));

                callback(status,
                         DuplexHealthResult{
                             .generation = busInfo_.GetGeneration(),
                             .appliedClock =
                                 AudioClockConfig{
                                     .sampleRateHz = global.sampleRate,
                                 },
                             .runtimeCaps = caps,
                             .sourceLocked = IsSourceLocked(global.status),
                             .clockReferenceHealthy = clockReferenceHealthy,
                             .nominalRateHz = NominalRateHz(global.status),
                             .notification = global.notification,
                             .status = global.status,
                             .extStatus = global.extStatus,
                         });
            });
    });
}


IOReturn DICETcatProtocol::StopDuplex() {
    if (!driver_) {
        return kIOReturnSuccess;
    }
    return driver_->Stop();
}

void DICETcatProtocol::UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                            std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) {
    (void)avcUnit; // DICE is register-based; no AV/C.
    if (driver_ && route != io_.Route()) driver_->CancelReceiverLockWatch(Diagnostics::DiceLockPhase::Reset);
    io_.UpdateRoute(route);
}

void DICETcatProtocol::EnsureSectionsLoaded(VoidCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady);
        return;
    }

    if (sectionsLoaded_) {
        callback(kIOReturnSuccess);
        return;
    }

    diceReader_.ReadGeneralSections([this, callback = std::move(callback)](IOReturn status, GeneralSections sections) mutable {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "DICETcatProtocol: failed to read general sections: 0x%x (%{public}s)", status, ASFW::Logging::IOReturnName(status));
            callback(status);
            return;
        }

        sections_ = sections;
        sectionsLoaded_ = true;
        ASFW_LOG(DICE,
                 "DICETcatProtocol: loaded sections global=%u/%u tx=%u/%u rx=%u/%u ext=%u/%u",
                 sections_.global.offset,
                 sections_.global.size,
                 sections_.txStreamFormat.offset,
                 sections_.txStreamFormat.size,
                 sections_.rxStreamFormat.offset,
                 sections_.rxStreamFormat.size,
                 sections_.extSync.offset,
                 sections_.extSync.size);
        callback(kIOReturnSuccess);
    });
}

void DICETcatProtocol::EnsureRuntimeCapsLoaded(VoidCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady);
        return;
    }

    if (runtimeCapsValid_.load(std::memory_order_acquire) && RateFormations()) {
        callback(kIOReturnSuccess);
        return;
    }

    EnsureSectionsLoaded([this, callback = std::move(callback)](IOReturn sectionStatus) mutable {
        if (sectionStatus != kIOReturnSuccess) {
            callback(sectionStatus);
            return;
        }

        struct RuntimeCapsState {
            GlobalState global;
            StreamConfig tx;
            StreamConfig rx;
        };

        auto state = std::make_shared<RuntimeCapsState>();
        diceReader_.ReadGlobalState(
            sections_,
            [this, state, callback = std::move(callback)](IOReturn globalStatus, GlobalState global) mutable {
                if (globalStatus != kIOReturnSuccess) {
                    ASFW_LOG(DICE, "DICETcatProtocol: failed to read global state: 0x%x", globalStatus);
                    callback(globalStatus);
                    return;
                }

                state->global = global;
                ASFW_LOG(DICE,
                         "DICETcatProtocol: global state rate=%u clockSelect=0x%08x status=0x%08x extStatus=0x%08x notification=0x%08x",
                         global.sampleRate,
                         global.clockSelect,
                         global.status,
                         global.extStatus,
                         global.notification);
                diceReader_.ReadTxStreamConfig(
                    sections_,
                    [this, state, callback = std::move(callback)](IOReturn txStatus, StreamConfig tx) mutable {
                        if (txStatus != kIOReturnSuccess) {
                            ASFW_LOG(DICE, "DICETcatProtocol: failed to read TX stream config: 0x%x", txStatus);
                            callback(txStatus);
                            return;
                        }

                        state->tx = tx;
                        LogStreamConfigSummary("TX", state->tx);
                        diceReader_.ReadRxStreamConfig(
                            sections_,
                            [this, state, callback = std::move(callback)](IOReturn rxStatus, StreamConfig rx) mutable {
                                if (rxStatus != kIOReturnSuccess) {
                                    ASFW_LOG(DICE, "DICETcatProtocol: failed to read RX stream config: 0x%x", rxStatus);
                                    callback(rxStatus);
                                    return;
                                }

                                state->rx = rx;
                                LogStreamConfigSummary("RX", state->rx);
                                CacheRuntimeCaps(state->global, state->tx, state->rx);
                                AudioStreamRuntimeCaps caps{};
                                (void)GetRuntimeAudioStreamCaps(caps);
                                LogRuntimeCaps("standard-dice", caps);
                                if (!HasUsableRuntimeCaps(caps)) {
                                    ASFW_LOG(DICE,
                                             "DICETcatProtocol: standard DICE discovery produced zero or partial caps; audio publication should fail closed");
                                }
                                diceReader_.ReadRateFormats(caps.deviceRateMask,
                                    [this, caps, callback = std::move(callback)](IOReturn status,
                                                                                DiceRateFormats formats) mutable {
                                        if (status == kIOReturnAborted || status == kIOReturnNoDevice ||
                                            status == kIOReturnOffline) {
                                            callback(status); return;
                                        }
                                        // EAP formats stand for every mode; the registers
                                        // only for a device without EAP. A Pro 24 DSP found
                                        // locked at 88.2 kHz with its 48 kHz layout
                                        // (2026-10-08) used to overwrite the EAP 2x entry
                                        // with 16 inputs, and every 2x switch rolled back.
                                        formats = SelectPublishedFormats(status == kIOReturnSuccess,
                                                                         formats, caps);
                                        auto formations = DiceFormations(caps.deviceRateMask, formats,
                                            runtimePolicy_.exposeDeviceToHostToCoreAudio);
                                        PublishRateFormations(
                                            std::make_shared<const std::vector<Runtime::RateFormation>>(
                                                std::move(formations)));
                                        callback(kIOReturnSuccess);
                                    });
                            });
                    });
            });
    });
}

void DICETcatProtocol::CacheRuntimeCaps(const GlobalState& global,
                                        const StreamConfig& tx,
                                        const StreamConfig& rx) noexcept {
    const auto caps = MakeDiceRuntimeCaps(global, tx, rx,
        runtimePolicy_.exposeDeviceToHostToCoreAudio);

    // Per-channel device labels from the DICE TX/RX name sections, flattened
    // across streams in channel order. Written BEFORE CacheRuntimeCaps(caps)'s
    // release-store so GetChannelLabels readers see a consistent snapshot.
    // Host input == device TX, host output == device RX (AudioTypes.hpp).
    auto fillLabels = [](const StreamConfig& sc,
                         std::atomic<uint32_t>& outCount,
                         char (&outLabels)[kMaxChannelLabels][64]) noexcept {
        uint32_t idx = 0;
        const uint32_t streams = (sc.numStreams < kMaxAudioStreamsPerDirection)
                                     ? sc.numStreams
                                     : kMaxAudioStreamsPerDirection;
        for (uint32_t s = 0; s < streams && idx < kMaxChannelLabels; ++s) {
            for (const auto& name : SplitDiceLabels(sc.streams[s].labels)) {
                if (idx >= kMaxChannelLabels) {
                    break;
                }
                strlcpy(outLabels[idx], name.c_str(), sizeof(outLabels[idx]));
                ++idx;
            }
        }
        for (uint32_t z = idx; z < kMaxChannelLabels; ++z) {
            outLabels[z][0] = '\0';
        }
        outCount.store(idx, std::memory_order_relaxed);
    };
    fillLabels(tx, inputChannelLabelCount_, inputChannelLabels_);
    fillLabels(rx, outputChannelLabelCount_, outputChannelLabels_);

    CacheRuntimeCaps(caps);
}

bool DICETcatProtocol::GetChannelLabels(std::vector<std::string>& inNames,
                                        std::vector<std::string>& outNames) const {
    if (!runtimeCapsValid_.load(std::memory_order_acquire)) {
        return false;
    }
    const uint32_t inCount = runtimePolicy_.exposeDeviceToHostToCoreAudio
                                 ? inputChannelLabelCount_.load(std::memory_order_relaxed)
                                 : 0;
    const uint32_t outCount = outputChannelLabelCount_.load(std::memory_order_relaxed);
    inNames.clear();
    outNames.clear();
    for (uint32_t i = 0; i < inCount && i < kMaxChannelLabels; ++i) {
        inNames.emplace_back(inputChannelLabels_[i]);
    }
    for (uint32_t i = 0; i < outCount && i < kMaxChannelLabels; ++i) {
        outNames.emplace_back(outputChannelLabels_[i]);
    }
    return inCount > 0 || outCount > 0;
}

void DICETcatProtocol::CacheRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept {
    const uint32_t exposedInputChannels = runtimePolicy_.exposeDeviceToHostToCoreAudio
                                              ? caps.hostInputPcmChannels
                                              : 0;
    hostInputPcmChannels_.store(exposedInputChannels, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(caps.deviceToHostAm824Slots, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(caps.hostOutputPcmChannels, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(caps.hostToDeviceAm824Slots, std::memory_order_relaxed);
    runtimeSampleRateHz_.store(caps.sampleRateHz, std::memory_order_relaxed);
    deviceRateMask_.store(caps.deviceRateMask, std::memory_order_relaxed);
    deviceToHostIsoChannel_.store(caps.deviceToHostIsoChannel, std::memory_order_relaxed);
    hostToDeviceIsoChannel_.store(caps.hostToDeviceIsoChannel, std::memory_order_relaxed);

    // Per-stream geometry: write the plain arrays + counts BEFORE the
    // release-store of runtimeCapsValid_ so readers that pass the acquire-load
    // observe a consistent snapshot.
    deviceToHostStreamCount_.store(caps.deviceToHostStreamCount, std::memory_order_relaxed);
    hostToDeviceStreamCount_.store(caps.hostToDeviceStreamCount, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        deviceToHostStreams_[i] = caps.deviceToHostStreams[i];
        hostToDeviceStreams_[i] = caps.hostToDeviceStreams[i];
    }

    runtimeCapsValid_.store(true, std::memory_order_release);
    LogRuntimeCaps("cache", caps);
}

void DICETcatProtocol::ResetRuntimeCaps() noexcept {
    runtimeCapsValid_.store(false, std::memory_order_release);
    runtimeSampleRateHz_.store(0, std::memory_order_relaxed);
    deviceRateMask_.store(0, std::memory_order_relaxed);
    hostInputPcmChannels_.store(0, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(0, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(0, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(0, std::memory_order_relaxed);
    deviceToHostIsoChannel_.store(AudioStreamRuntimeCaps::kInvalidIsoChannel, std::memory_order_relaxed);
    hostToDeviceIsoChannel_.store(AudioStreamRuntimeCaps::kInvalidIsoChannel, std::memory_order_relaxed);
    deviceToHostStreamCount_.store(0, std::memory_order_relaxed);
    hostToDeviceStreamCount_.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        deviceToHostStreams_[i] = AudioStreamWireInfo{};
        hostToDeviceStreams_[i] = AudioStreamWireInfo{};
    }
    inputChannelLabelCount_.store(0, std::memory_order_relaxed);
    outputChannelLabelCount_.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxChannelLabels; ++i) {
        inputChannelLabels_[i][0] = '\0';
        outputChannelLabels_[i][0] = '\0';
    }
}

} // namespace ASFW::Audio::DICE::TCAT
