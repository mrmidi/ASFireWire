// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceFamilyDriver.cpp - Linear DICE duplex bring-up and teardown.
//
// Each step below is the synchronous form of one step of the callback-chained
// DICEDuplexBringupController it replaced (removed in S1), in the same order, with the same transactions, route and teardown
// checks, poll intervals and timeouts. Changing the choreography is stage S3's
// job and must show up as a declared golden-trace delta.

#include "DiceFamilyDriver.hpp"
#include "DiceClockChangeObserver.hpp"
#include "../../../Runtime/RateValidation.hpp"

#include "../../../../Common/WireFormat.hpp"
#include "../../../../Logging/Logging.hpp"

#include <algorithm>
#include <limits>

namespace ASFW::Audio::DICE {

namespace {

// Linux waits NOTIFICATION_TIMEOUT_MS (100 ms) for CLOCK_ACCEPTED
// (dice-stream.c:12, 60-98); TCAT waits about 150 ms (MidasFW RestartStreaming
// 0xdb62). ASFW keeps 150 ms. Source-lock policy is a separate decision.
constexpr uint32_t kClockAcceptedTimeoutMs = 150;
// After a reconfiguration (RX/TX_CFG_CHG with CLOCK_ACCEPTED), TCAT's
// RestartStreaming waits up to 1 s for LOCK_CHG and STATUS locked (MidasFW 0xdb62).
constexpr uint32_t kReconfigureRelockTimeoutMs = 1000;
constexpr uint32_t kStreamingClockLockTimeoutMs = 2000;
constexpr uint32_t kPollIntervalMs = 10;
constexpr uint32_t kReadyTimeoutMs = 200;
// An idle clock change waits this long for the device to reach the new rate
// (TCAT RestartStreaming waits up to 1 s for lock after a clock change).
constexpr uint32_t kIdleClockRateTimeoutMs = 1000;

constexpr uint32_t kDisabledIsoChannel = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kRxSeqStartDefault = 0;
constexpr uint32_t kOwnerBytes = 8;
constexpr uint32_t kStreamNamesBytes = 256;

void RecordFirstError(IOReturn& slot, IOReturn status) noexcept {
    if (slot == kIOReturnSuccess && status != kIOReturnSuccess) {
        slot = status;
    }
}

[[nodiscard]] IOReturn ErrorOr(const auto& result, IOReturn success = kIOReturnSuccess) noexcept {
    return result ? success : result.error();
}

void ResetSession(DuplexRestartSession& session) noexcept {
    session = DuplexRestartSession{};
}

void CacheRuntimeCaps(AudioStreamRuntimeCaps& caps,
                      const GlobalState& global,
                      const StreamConfig& tx,
                      const StreamConfig& rx) noexcept {
    caps.hostInputPcmChannels = tx.TotalPcmChannels();
    caps.deviceToHostAm824Slots = tx.TotalAm824Slots();
    caps.hostOutputPcmChannels = rx.TotalPcmChannels();
    caps.hostToDeviceAm824Slots = rx.TotalAm824Slots();
    caps.sampleRateHz = DiceConfiguredRate(global.sampleRate, global.status, global.clockSelect);
    caps.deviceRateMask = DiceDeviceRateMask(global.hasClockCaps, global.clockCaps);
    caps.deviceToHostIsoChannel =
        tx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel);
    caps.hostToDeviceIsoChannel =
        rx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel);

    // Per-stream wire geometry. Stream count comes from the DICE stream-format
    // header (TX_NUMBER/RX_NUMBER), which includes streams the device reports
    // with iso=-1 (disabled) that the host must still arm for a multi-stream
    // device such as the Venice F32 (2x16 channels). The discovered isoChannel
    // is carried through but the host reassigns it during channel resolution.
    auto fillPerStream = [](const StreamConfig& sc,
                            uint32_t& outCount,
                            AudioStreamWireInfo* outStreams) noexcept {
        const uint32_t count = std::min(sc.numStreams, kMaxAudioStreamsPerDirection);
        outCount = count;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& entry = sc.streams[i];
            outStreams[i].isoChannel =
                (entry.isoChannel >= 0 && entry.isoChannel <= 0x3F)
                    ? static_cast<uint8_t>(entry.isoChannel)
                    : AudioStreamWireInfo::kInvalidIsoChannel;
            outStreams[i].pcmChannels = static_cast<uint16_t>(entry.pcmChannels);
            outStreams[i].am824Slots = static_cast<uint16_t>(entry.Am824Slots());
            outStreams[i].midiPorts = static_cast<uint16_t>(entry.midiPorts);
        }
    };
    fillPerStream(tx, caps.deviceToHostStreamCount, caps.deviceToHostStreams);
    fillPerStream(rx, caps.hostToDeviceStreamCount, caps.hostToDeviceStreams);
}

[[nodiscard]] uint32_t ClampedStreamCount(uint32_t count, uint32_t entrySizeBytes) noexcept {
    if (entrySizeBytes == 0) {
        return 1;  // without a usable stride only stream[0]'s registers are addressable
    }
    if (count == 0) {
        return 1;
    }
    return std::min(count, kMaxAudioStreamsPerDirection);
}

} // namespace

DiceFamilyDriver::DiceFamilyDriver(DiceDeviceIo& io,
                                   Protocols::Ports::FireWireBusInfo& busInfo,
                                   DiceNotificationMailbox& notifications,
                                   DICEBringupPolicy bringupPolicy,
                                   Scheduling::ITimerScheduler* scheduler) noexcept
    : receiverDump_(bringupPolicy.verifiedLoaderPeek),
      scheduler_(scheduler ? scheduler : io.Clock().Scheduler()), io_(io), busInfo_(busInfo), notifications_(notifications), bringupPolicy_(bringupPolicy) {}

// ---------------------------------------------------------------------------
// Public stages
// ---------------------------------------------------------------------------

std::expected<DuplexPrepareResult, IOReturn> DiceFamilyDriver::Prepare(
    const AudioDuplexChannels& channels,
    const DiceClockConfiguration& clock,
    bool refreshRuntimeCaps) {
    if (channels.deviceToHostIsoChannel > 63 || channels.hostToDeviceIsoChannel > 63) {
        return std::unexpected(kIOReturnBadArgument);
    }
    if (!busInfo_.GetLocalNodeID().IsValid()) {
        return std::unexpected(kIOReturnNotReady);
    }
    if (!IsSupportedDiceClockConfiguration(clock, Runtime::kDiceHardwareBatch)) {
        return std::unexpected(kIOReturnUnsupported);
    }
    if (HasDeviceRestartState(session_)) {
        const IOReturn stopStatus = Stop();
        if (stopStatus != kIOReturnSuccess) {
            return std::unexpected(stopStatus);
        }
    }

    flowMode_ = FlowMode::kPrepareDuplex;
    refreshRuntimeCapsOnPrepare_ = refreshRuntimeCaps;
    notifications_.Reset();
    stopSequenceError_ = kIOReturnSuccess;
    diceClock_ = clock;
    session_ = DuplexRestartSession{
        .generation = busInfo_.GetGeneration(),
        .channels = channels,
        .reason = DuplexRestartReason::kInitialStart,
        .desiredClock = AudioClockConfig{.sampleRateHz = clock.sampleRateHz},
        .phase = DuplexRestartPhase::kPreparingDevice,
    };
    runtimeCaps_ = {};
    confirmNotification_ = 0;
    confirmStatus_ = 0;
    confirmExtStatus_ = 0;

    ASFW_LOG(DICE,
             "PrepareDuplex48k: raw parity start gen=%u localNode=0x%02x rxIso=%u txIso=%u",
             session_.generation.value,
             busInfo_.GetLocalNodeID().value,
             channels.hostToDeviceIsoChannel,
             channels.deviceToHostIsoChannel);

    const IOReturn status = ClaimAndClock(channels);
    if (status != kIOReturnSuccess) {
        return std::unexpected(status);
    }
    return DuplexPrepareResult{
        .generation = session_.generation,
        .channels = channels,
        .appliedClock = session_.appliedClock,
        .runtimeCaps = runtimeCaps_,
    };
}

IOReturn DiceFamilyDriver::AssignChannels(const AudioDuplexChannels& channels) {
    if (!session_.devicePrepared || session_.deviceRxProgrammed) {
        return kIOReturnNotReady;
    }
    if (channels.deviceToHostIsoChannel > 63 || channels.hostToDeviceIsoChannel > 63) {
        return kIOReturnBadArgument;
    }
    session_.channels = channels;
    return kIOReturnSuccess;
}

std::expected<DuplexStageResult, IOReturn> DiceFamilyDriver::ProgramRx() {
    if (!session_.devicePrepared) {
        return std::unexpected(kIOReturnNotReady);
    }
    if (!EnsureRouteCurrent()) {
        return std::unexpected(kIOReturnOffline);
    }
    if (session_.channels.deviceToHostIsoChannel > 63 ||
        session_.channels.hostToDeviceIsoChannel > 63) {
        return std::unexpected(kIOReturnNotReady);
    }
    const IOReturn status = ProgramRxStreams();
    if (status != kIOReturnSuccess) {
        return std::unexpected(status);
    }
    return DuplexStageResult{
        .generation = session_.generation,
        .channels = session_.channels,
        .phase = session_.phase,
        .runtimeCaps = runtimeCaps_,
    };
}

std::expected<DuplexStageResult, IOReturn> DiceFamilyDriver::ProgramTxAndEnable() {
    if (!session_.devicePrepared || !session_.deviceRxProgrammed) {
        return std::unexpected(kIOReturnNotReady);
    }
    if (!EnsureRouteCurrent()) {
        return std::unexpected(kIOReturnOffline);
    }
    const IOReturn status = ProgramTxStreamsAndEnable();
    if (status != kIOReturnSuccess) {
        return std::unexpected(status);
    }
    return DuplexStageResult{
        .generation = session_.generation,
        .channels = session_.channels,
        .phase = session_.phase,
        .runtimeCaps = runtimeCaps_,
    };
}

std::expected<DuplexConfirmResult, IOReturn> DiceFamilyDriver::Confirm() {
    if (!session_.devicePrepared || !session_.deviceTxArmed) {
        return std::unexpected(kIOReturnNotReady);
    }

    session_.phase = DuplexRestartPhase::kConfirmingDeviceStart;
    notifications_.Reset();

    // Poll for source lock, then read NOTIFICATION and EXT_STATUS once.
    uint32_t notify = 0;
    for (uint32_t attempt = 0;; ++attempt) {
        if (!EnsureRouteCurrent()) {
            (void)Stop();
            return std::unexpected(kIOReturnOffline);
        }

        notify |= notifications_.Consume();
        const auto status = io_.ReadQuad(sections_.global.offset + GlobalOffset::kStatus);
        if (!status) {
            (void)Stop();
            return std::unexpected(status.error());
        }

        const bool sourceLocked = IsSourceLocked(*status);
        if (sourceLocked || !bringupPolicy_.requireSourceLockAtConfirm) {
            if (!sourceLocked) {
                ASFW_LOG(DICE,
                         "ConfirmDuplex48kStart: proceeding with advisory source lock "
                         "status=0x%08x",
                         *status);
            }
            if (const auto notification =
                    io_.ReadQuad(sections_.global.offset + GlobalOffset::kNotification)) {
                notify |= *notification;
            }
            const auto ext = io_.ReadQuad(sections_.global.offset + GlobalOffset::kExtStatus);
            const IOReturn confirmStatus = CompleteConfirm(notify, *status, ext ? *ext : 0U);
            if (confirmStatus != kIOReturnSuccess) {
                return std::unexpected(confirmStatus);
            }
            return DuplexConfirmResult{
                .generation = session_.generation,
                .channels = session_.channels,
                .appliedClock = session_.appliedClock,
                .runtimeCaps = runtimeCaps_,
                .notification = confirmNotification_,
                .status = confirmStatus_,
                .extStatus = confirmExtStatus_,
            };
        }

        if (attempt * kPollIntervalMs >= kReadyTimeoutMs) {
            ASFW_LOG_ERROR(DICE,
                           "ConfirmDuplex48kStart: DICE clock failed to lock within %u ms (status=0x%08x)",
                           kReadyTimeoutMs, *status);
            (void)Stop();
            return std::unexpected(kIOReturnTimeout);
        }
        io_.Clock().SleepMs(kPollIntervalMs);
    }
}

std::expected<DuplexClockApplyResult, IOReturn> DiceFamilyDriver::ApplyClock(
    const DiceClockConfiguration& clock) {
    if (!IsSupportedDiceClockConfiguration(clock, Runtime::kDiceHardwareBatch)) {
        return std::unexpected(kIOReturnUnsupported);
    }
    if (!busInfo_.GetLocalNodeID().IsValid()) {
        return std::unexpected(kIOReturnNotReady);
    }
    if (HasDeviceRestartState(session_)) {
        return std::unexpected(kIOReturnBusy);
    }

    flowMode_ = FlowMode::kClockApply;
    notifications_.Reset();
    stopSequenceError_ = kIOReturnSuccess;
    diceClock_ = clock;
    session_ = DuplexRestartSession{
        .generation = busInfo_.GetGeneration(),
        .reason = DuplexRestartReason::kManualReconfigure,
        .desiredClock = AudioClockConfig{.sampleRateHz = clock.sampleRateHz},
        .phase = DuplexRestartPhase::kPreparingDevice,
    };
    runtimeCaps_ = {};
    confirmNotification_ = 0;
    confirmStatus_ = 0;
    confirmExtStatus_ = 0;

    const IOReturn status = ClaimAndClock(AudioDuplexChannels{});
    if (status != kIOReturnSuccess) {
        return std::unexpected(status);
    }
    return DuplexClockApplyResult{
        .generation = session_.generation,
        .appliedClock = session_.appliedClock,
        .runtimeCaps = runtimeCaps_,
    };
}

void DiceFamilyDriver::AdoptPreparedClock(const DiceClockConfiguration& /*clock*/) noexcept {
    ownerGeneration_ = busInfo_.GetGeneration();
}

IOReturn DiceFamilyDriver::Stop() {
    lockWatch_.Cancel();
    receiverDump_.Cancel();
    if (!HasDeviceRestartState(session_)) {
        return kIOReturnSuccess;
    }
    if (TeardownRequested()) {
        RecordStopTeardownAbort("Entry");
        ResetSession(session_);
        flowMode_ = FlowMode::kNone;
        return kIOReturnAborted;
    }
    return StopSequence();
}

// ---------------------------------------------------------------------------
// Bring-up chain
// ---------------------------------------------------------------------------

IOReturn DiceFamilyDriver::ClaimAndClock(const AudioDuplexChannels& channels) {
    clockSelectWritten_ = false;
    // Section layout first: every GLOBAL access below depends on it (TCAT
    // RestartStreaming reads the 0x28-byte table before GLOBAL).
    if (!EnsureRouteCurrent()) {
        return Rollback(kIOReturnOffline);
    }
    const auto sections = io_.ReadGeneralSections();
    if (!sections) {
        return Rollback(sections.error());
    }
    sections_ = *sections;

    // GLOBAL before the claim: what was requested and what was reached.
    if (!EnsureRouteCurrent()) {
        return Rollback(kIOReturnOffline);
    }
    const auto preClaim = io_.ReadGlobalStateFull(sections_);
    if (!preClaim) {
        return Rollback(preClaim.error());
    }
    // Checked before the claim, the first write. GLOBAL_VERSION exists only
    // when GLOBAL reaches it; older firmware stops short (Linux
    // dice-transaction.c:303-322).
    if (preClaim->hasVersion &&
        (preClaim->version >> 24U) != kDiceSupportedMajorVersion) {
        ASFW_LOG(DICE, "PrepareDuplex48k: unsupported DICE version 0x%08x; refusing",
                 preClaim->version);
        return Rollback(kIOReturnUnsupported);
    }
    // Rate changes preserve the live source (Linux dice-stream.c:60-85).
    // Reuse this pre-claim read instead of adding a second GLOBAL transaction.
    const auto source = preClaim->clockSelect & ClockSelect::kSourceMask;
    if (!IsSupportedDiceClockSource(source)) return Rollback(kIOReturnUnsupported);
    diceClock_.clockSelect = (diceClock_.clockSelect & ~ClockSelect::kSourceMask) | source;
    preClaimClockSelect_ = preClaim->clockSelect;
    preClaimStatus_ = preClaim->status;
    preClaimSampleRate_ = preClaim->sampleRate;
    ASFW_LOG(DICE,
             "PrepareDuplex48k: global pre-claim owner=0x%016llx enable=%u notify=0x%08x clockSelect=0x%08x status=0x%08x rate=%u",
             preClaim->owner,
             preClaim->enabled ? 1U : 0U,
             preClaim->notification,
             preClaim->clockSelect,
             preClaim->status,
             preClaim->sampleRate);

    const IOReturn owner = EnsureOwner(preClaim->owner);
    if (owner != kIOReturnSuccess) {
        return Rollback(owner);
    }
    session_.ownerConfirmed = true;

    return WriteClockSelect(channels);
}

IOReturn DiceFamilyDriver::EnsureOwner(uint64_t currentOwner) {
    // Held since earlier in this bus generation: nothing to do on the wire. The
    // GLOBAL read just before this showed the owner is still ours. TCAT claims
    // only when the generation changed (GetOwnership 0xc624); Linux re-claims
    // after every reset (dice-transaction.c:254). A bus reset clears the owner
    // on the device, so a new generation always claims again.
    const FW::Generation generation = session_.generation;
    if (ownerGeneration_.has_value() && ownerGeneration_->value == generation.value &&
        currentOwner == OwnerValue()) {
        return kIOReturnSuccess;
    }
    ownerGeneration_.reset();

    // Owner before the claim.
    if (!EnsureRouteCurrent()) {
        return kIOReturnOffline;
    }
    const uint32_t ownerOffset = sections_.global.offset + GlobalOffset::kOwnerHi;
    const auto ownerBefore = io_.ReadBlock(ownerOffset, kOwnerBytes);
    if (!ownerBefore || ownerBefore->size() < kOwnerBytes) {
        return ownerBefore ? kIOReturnUnderrun : ownerBefore.error();
    }
    ASFW_LOG(DICE, "PrepareDuplex48k: owner before claim=0x%016llx",
             ASFW::FW::ReadBE64(ownerBefore->data()));

    // Claim: compare-swap from "no owner", accepting "already ours" (Linux
    // register_notification_address).
    if (!EnsureRouteCurrent()) {
        return kIOReturnOffline;
    }
    const uint64_t ownerValue = OwnerValue();
    const auto previous = io_.CompareSwap64(ownerOffset, kOwnerNoOwner, ownerValue);
    if (!previous) {
        return previous.error();
    }
    if (*previous != kOwnerNoOwner && *previous != ownerValue) {
        return kIOReturnExclusiveAccess;
    }

    // Owner after the claim must read back as ours.
    if (!EnsureRouteCurrent()) {
        return kIOReturnOffline;
    }
    const auto ownerAfter = io_.ReadBlock(ownerOffset, kOwnerBytes);
    if (!ownerAfter || ownerAfter->size() < kOwnerBytes) {
        return ownerAfter ? kIOReturnUnderrun : ownerAfter.error();
    }
    if (ASFW::FW::ReadBE64(ownerAfter->data()) != OwnerValue()) {
        return kIOReturnExclusiveAccess;
    }
    ownerGeneration_ = generation;
    return kIOReturnSuccess;
}

IOReturn DiceFamilyDriver::WriteClockSelect(const AudioDuplexChannels& channels) {
    if (!EnsureRouteCurrent()) {
        return Rollback(kIOReturnOffline);
    }

    // A stream start always writes CLOCK_SELECT, even when the device already
    // runs at the target. TCAT's kexts do (RestartStreaming, MidasFW 0xdb62:
    // owner claim, then CLOCK_SELECT, then CLOCK_ACCEPTED), and so does Linux on
    // every start with nothing streaming (dice-stream.c:279-294, select_clock):
    // "just after owning the unit, the unit can return invalid stream formats.
    // Selecting clock parameters have an effect for the unit to refine it."
    // Skipping it here (since dfabe0d6, to avoid a relock during bring-up) left a
    // Saffire Pro 24 DSP sending exact digital zero on every capture channel
    // after a power-up at 48 kHz, three starts out of three; one same-value
    // write restored all of them (hardware, 2026-10-11). The write makes the
    // device reconfigure (RX/TX_CFG_CHG, ~200 ms unlocked); the stream-lock wait
    // below covers that.
    //
    // An idle rate change to the rate the device already has still skips: the
    // next start writes anyway. "At target" needs both halves there. CLOCK_SELECT
    // is the rate that was requested; STATUS and SAMPLE_RATE are the rate the
    // device reached, and they disagree after a change the device did not
    // complete.
    const uint32_t targetHz = session_.desiredClock.sampleRateHz;
    const bool requestedAtTarget = preClaimClockSelect_ == diceClock_.clockSelect;
    const bool achievedAtTarget =
        DiceClockAtRate(preClaimSampleRate_, preClaimStatus_, preClaimClockSelect_, targetHz);
    if (flowMode_ == FlowMode::kClockApply && requestedAtTarget && achievedAtTarget) {
        ASFW_LOG(DICE,
                 "ApplyClock: device already at target clockSelect=0x%08x rate=%u; skipping redundant write",
                 diceClock_.clockSelect, targetHz);
        return ActiveClockCheck(channels, notifications_.Consume());
    }
    if (requestedAtTarget && achievedAtTarget) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: device already at target clockSelect=0x%08x rate=%u; writing it "
                 "again so the device re-applies it",
                 diceClock_.clockSelect, targetHz);
    } else if (requestedAtTarget) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: clockSelect=0x%08x already requests %u Hz but device reports %u Hz "
                 "(status=0x%08x); rewriting",
                 diceClock_.clockSelect, targetHz, preClaimSampleRate_, preClaimStatus_);
    }

    if (bringupPolicy_.clockChangeObserver != nullptr) {
        bringupPolicy_.clockChangeObserver->BeforeClockSelectWrite(io_);
        if (!EnsureRouteCurrent()) {
            return Rollback(kIOReturnOffline);
        }
    }

    if (flowMode_ == FlowMode::kPrepareDuplex) {
        // NEVER DELETE OR SKIP THIS WRITE, EVEN IF GLOBAL_ENABLE ALREADY READS 0.
        // DDR_MSG_DISABLE clears the firmware started latch (Pro24DSP 0x31278).
        // Reading zero does not perform that reset; skipping it can leave DSP
        // mute set on a same-rate cold start. Linux dice-stream.c:256-294.
        // Regression: FreshPrepareDisablesBeforeClockEvenWhenEnableIsZero.
        if (const auto disabled = io_.WriteQuad(sections_.global.offset + GlobalOffset::kEnable, 0U);
            !disabled) {
            return Rollback(disabled.error());
        }
        ASFW_LOG(DICE, "[DiceStart] GLOBAL_ENABLE=0 before clock rate=%u gen=%u",
                 targetHz, session_.generation.value);
    }

    notifications_.Reset();
    if (const auto written = io_.WriteQuad(sections_.global.offset + GlobalOffset::kClockSelect,
                                           diceClock_.clockSelect);
        !written) {
        return Rollback(written.error());
    }
    clockSelectWritten_ = true;

    // The notification may have arrived during the write.
    const uint32_t earlyBits = notifications_.Consume();
    if ((earlyBits & Notify::kClockAccepted) != 0) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: CLOCK_ACCEPTED arrived during write, bits=0x%08x",
                 earlyBits);
        return ReadGlobalAfterClockAccepted(channels, earlyBits, kIOReturnNotReady);
    }
    // STATUS can still describe the pre-write clock until firmware reacts.
    // Wait after every write, even if the old status already matches the target.
    // Cross-checked with Linux dice-stream.c:82-98 and TCAT's wait before stream
    // allocation (tmp/dicere/dec_Midas/V___RestartStreaming.c:423-465).
    return WaitClockAccepted(channels, earlyBits);
}

IOReturn DiceFamilyDriver::ActiveClockCheck(const AudioDuplexChannels& channels,
                                            uint32_t accumulatedNotify) {
    if (!EnsureRouteCurrent()) {
        return Rollback(kIOReturnOffline);
    }

    const auto state = io_.ReadGlobalStateFull(sections_);
    if (!state) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: active clock check read failed (0x%08x), falling back to mailbox poll",
                 state.error());
        return WaitClockAccepted(channels, accumulatedNotify);
    }

    // Include any mailbox bits that arrived during the read.
    const uint32_t combinedNotify = accumulatedNotify | notifications_.Consume();
    const bool clockAccepted = (combinedNotify & Notify::kClockAccepted) != 0;
    const bool sourceLockedAtTarget =
        IsSourceLocked(state->status) &&
        NominalRateHz(state->status) == session_.desiredClock.sampleRateHz;
    const bool sampleRateAtTarget = DiceConfiguredRate(state->sampleRate, state->status, state->clockSelect) == session_.desiredClock.sampleRateHz;

    if (clockAccepted || (sourceLockedAtTarget && sampleRateAtTarget)) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: clock confirmed via active check "
                 "(notify=0x%08x status=0x%08x rate=%u locked=%u)",
                 combinedNotify, state->status, state->sampleRate,
                 sourceLockedAtTarget ? 1U : 0U);
        return ClockConfirmed(channels);
    }

    ASFW_LOG(DICE,
             "PrepareDuplex48k: active check not yet locked "
             "(notify=0x%08x status=0x%08x rate=%u), entering mailbox poll",
             combinedNotify, state->status, state->sampleRate);
    return WaitClockAccepted(channels, combinedNotify);
}

IOReturn DiceFamilyDriver::WaitClockAccepted(const AudioDuplexChannels& channels,
                                            uint32_t observedNotify) {
    const uint64_t startMs = io_.Clock().NowMs();
    for (;;) {
        if (TeardownRequested()) {
            return Rollback(kIOReturnAborted);
        }
        if (!EnsureRouteCurrent()) {
            return Rollback(kIOReturnOffline);
        }

        // Config and lock changes may precede CLOCK_ACCEPTED in separate writes.
        observedNotify |= notifications_.Consume();
        if ((observedNotify & Notify::kClockAccepted) != 0) {
            ASFW_LOG(DICE, "PrepareDuplex48k: observed async CLOCK_ACCEPTED bits=0x%08x",
                     observedNotify);
            return ReadGlobalAfterClockAccepted(channels, observedNotify, kIOReturnNotReady);
        }

        if (io_.Clock().NowMs() - startMs >= kClockAcceptedTimeoutMs) {
            ASFW_LOG(DICE,
                     "PrepareDuplex48k: CLOCK_ACCEPTED wait reached %u ms; performing final confirmation",
                     kClockAcceptedTimeoutMs);
            if (!EnsureRouteCurrent()) {
                return Rollback(kIOReturnOffline);
            }
            const uint32_t lateMailboxBits = notifications_.Consume();
            if ((lateMailboxBits & Notify::kClockAccepted) != 0) {
                ASFW_LOG(DICE, "PrepareDuplex48k: observed late async CLOCK_ACCEPTED bits=0x%08x",
                         lateMailboxBits);
            }
            return ReadGlobalAfterClockAccepted(channels, observedNotify | lateMailboxBits,
                                                kIOReturnTimeout);
        }

        io_.Clock().SleepMs(kPollIntervalMs);
    }
}

IOReturn DiceFamilyDriver::ReadGlobalAfterClockAccepted(const AudioDuplexChannels& channels,
                                                        uint32_t observedNotify,
                                                        IOReturn failureStatus) {
    if (!EnsureRouteCurrent()) {
        return Rollback(kIOReturnOffline);
    }

    const auto state = io_.ReadGlobalStateFull(sections_);
    if (!state) {
        return Rollback(state.error());
    }

    const uint32_t combinedNotify = observedNotify | state->notification | notifications_.Consume();
    const bool clockAccepted = (combinedNotify & Notify::kClockAccepted) != 0;
    const bool sourceLockedAtTarget =
        IsSourceLocked(state->status) &&
        NominalRateHz(state->status) == session_.desiredClock.sampleRateHz;
    const bool sampleRateAtTarget = DiceConfiguredRate(state->sampleRate, state->status, state->clockSelect) == session_.desiredClock.sampleRateHz;

    if (state->clockSelect != diceClock_.clockSelect) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: clock confirm failed, clockSelect=0x%08x notify=0x%08x status=0x%08x sampleRate=%u",
                 state->clockSelect, combinedNotify, state->status, state->sampleRate);
        return Rollback(failureStatus);
    }
    if (!clockAccepted && !(sourceLockedAtTarget && sampleRateAtTarget)) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: CLOCK_ACCEPTED not confirmed, notify=0x%08x status=0x%08x sampleRate=%u",
                 combinedNotify, state->status, state->sampleRate);
        return Rollback(failureStatus);
    }
    if (!clockAccepted) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: confirmed clock via global state after timeout, status=0x%08x sampleRate=%u",
                 state->status, state->sampleRate);
    }
    if ((combinedNotify & (Notify::kRxConfigChange | Notify::kTxConfigChange)) != 0) {
        const IOReturn relocked = AwaitReconfigurationRelock(combinedNotify);
        if (relocked != kIOReturnSuccess) {
            return relocked;
        }
    }
    return ClockConfirmed(channels);
}

IOReturn DiceFamilyDriver::AwaitReconfigurationRelock(uint32_t observedNotify) {
    // The device announced RX/TX_CFG_CHG with the clock: it drops lock (LOCK_CHG)
    // and comes back (LOCK_CHG again) some 200 ms later. Nothing may touch its
    // streams before then. Cross-checked with TCAT's bounded LOCK_CHG/status
    // wait (tmp/dicere/dec_Midas/V___RestartStreaming.c:474-525). Its timeout
    // is advisory; AwaitStreamingClockLock still enforces rate/source readiness.
    const uint32_t targetHz = session_.desiredClock.sampleRateHz;
    bool lockChanged = (observedNotify & Notify::kLockChange) != 0;
    const uint64_t startMs = io_.Clock().NowMs();
    for (;;) {
        if (TeardownRequested()) {
            return Rollback(kIOReturnAborted);
        }
        if (!EnsureRouteCurrent()) {
            return Rollback(kIOReturnOffline);
        }
        const uint32_t notify = notifications_.Consume();
        lockChanged = lockChanged || (notify & Notify::kLockChange) != 0;
        const auto state = io_.ReadGlobalStateFull(sections_);
        if (!state) {
            return Rollback(state.error());
        }
        const uint32_t lateNotify = notifications_.Consume();
        lockChanged = lockChanged || ((state->notification | lateNotify) & Notify::kLockChange) != 0;
        // A LOCK_CHG arriving during this transaction can overtake a response
        // sampled before the transition. Confirm it with the next GLOBAL read;
        // never pair that new event with this potentially stale locked STATUS.
        // Observed on Pro 24 DSP cold start, 2026-10-11 (ring 526-532).
        const bool lockChangedDuringRead = (lateNotify & Notify::kLockChange) != 0;
        const bool lockedAtTarget = IsSourceLocked(state->status) &&
                                    NominalRateHz(state->status) == targetHz &&
                                    DiceConfiguredRate(state->sampleRate, state->status, state->clockSelect) == targetHz;
        const uint64_t waitedMs = io_.Clock().NowMs() - startMs;
        if (lockChanged && lockedAtTarget && !lockChangedDuringRead) {
            ASFW_LOG(DICE, "PrepareDuplex48k: device reconfigured and relocked at %u Hz after %llu ms",
                     targetHz, waitedMs);
            return kIOReturnSuccess;
        }
        if (waitedMs >= kReconfigureRelockTimeoutMs) {
            ASFW_LOG_ERROR(DICE,
                           "PrepareDuplex48k: no relock within %u ms after the device reconfigured "
                           "(lockChg=%u status=0x%08x rate=%u); continuing",
                           kReconfigureRelockTimeoutMs, lockChanged ? 1U : 0U, state->status,
                           state->sampleRate);
            return kIOReturnSuccess;
        }
        io_.Clock().SleepMs(kPollIntervalMs);
    }
}

IOReturn DiceFamilyDriver::ClockConfirmed(const AudioDuplexChannels& channels) {
    if (flowMode_ == FlowMode::kClockApply) {
        const IOReturn status = AwaitIdleClockRate();
        if (status != kIOReturnSuccess) {
            return status;
        }
        if (!AfterClockWrite()) {
            return Rollback(TeardownRequested() ? kIOReturnAborted : kIOReturnOffline);
        }
        return CompleteClockApply();
    }
    return AwaitStreamingClockLock(channels);
}

IOReturn DiceFamilyDriver::AwaitIdleClockRate() {
    // A completed stopped-IO change prepares the next StartIO. Acceptance is
    // insufficient: confirm achieved rate and the profile's source-lock gate.
    for (uint32_t attempt = 0;; ++attempt) {
        if (TeardownRequested()) return Rollback(kIOReturnAborted);
        if (!EnsureRouteCurrent()) return Rollback(kIOReturnOffline);
        const auto state = io_.ReadGlobalStateFull(sections_);
        if (!state) return Rollback(state.error());
        const uint32_t targetHz = session_.desiredClock.sampleRateHz;
        if (NominalRateHz(state->status) == targetHz && DiceConfiguredRate(state->sampleRate, state->status, state->clockSelect) == targetHz &&
            (!bringupPolicy_.requireSourceLockBeforeStreamEnable || IsSourceLocked(state->status))) {
            if (attempt > 0) {
                ASFW_LOG(DICE, "ApplyClock: prepared %u Hz after %u ms", targetHz,
                         attempt * kPollIntervalMs);
            }
            return kIOReturnSuccess;
        }
        if (attempt * kPollIntervalMs >= kIdleClockRateTimeoutMs) {
            ASFW_LOG_ERROR(DICE, "ApplyClock: %u Hz not ready within %u ms (status=0x%08x rate=%u)",
                           targetHz, kIdleClockRateTimeoutMs, state->status, state->sampleRate);
            return Rollback(kIOReturnTimeout);
        }
        io_.Clock().SleepMs(kPollIntervalMs);
    }
}

IOReturn DiceFamilyDriver::AwaitStreamingClockLock(const AudioDuplexChannels& channels) {
    (void)channels;
    // CLOCK_ACCEPTED means the device received CLOCK_SELECT, not that the PLL
    // has reached the requested rate. Never enable streams at the old rate.
    // Most DICE products additionally require GLOBAL source lock here. The
    // narrowly scoped playback-only policy still waits for the target rate,
    // but lets host IT establish an otherwise unavailable receive-clock lock.
    for (uint32_t attempt = 0;; ++attempt) {
        if (!EnsureRouteCurrent()) {
            return Rollback(kIOReturnOffline);
        }

        const auto state = io_.ReadGlobalStateFull(sections_);
        if (!state) {
            return Rollback(state.error());
        }

        const bool rateAtTarget =
            NominalRateHz(state->status) == session_.desiredClock.sampleRateHz &&
            DiceConfiguredRate(state->sampleRate, state->status, state->clockSelect) == session_.desiredClock.sampleRateHz;
        const bool sourceLocked = IsSourceLocked(state->status);
        const bool readyForEnable =
            rateAtTarget && (sourceLocked || !bringupPolicy_.requireSourceLockBeforeStreamEnable);

        if (readyForEnable) {
            if (attempt > 0) {
                ASFW_LOG(DICE,
                         "PrepareDuplex48k: target rate ready at %u Hz after %u ms; sourceLock=%u; enabling streams",
                         state->sampleRate, attempt * kPollIntervalMs, sourceLocked ? 1U : 0U);
            }
            if (!AfterClockWrite()) {
                return Rollback(TeardownRequested() ? kIOReturnAborted : kIOReturnOffline);
            }
            return DiscoverStreams();
        }

        if (attempt * kPollIntervalMs >= kStreamingClockLockTimeoutMs) {
            ASFW_LOG(DICE,
                     "PrepareDuplex48k: target rate%s not ready within %u ms (status=0x%08x rate=%u target=%u); aborting bring-up",
                     bringupPolicy_.requireSourceLockBeforeStreamEnable ? " and source lock" : "",
                     kStreamingClockLockTimeoutMs, state->status, state->sampleRate,
                     session_.desiredClock.sampleRateHz);
            return Rollback(kIOReturnTimeout);
        }

        io_.Clock().SleepMs(kPollIntervalMs);
    }
}

bool DiceFamilyDriver::AfterClockWrite() {
    if (!clockSelectWritten_) {
        return true;
    }
    clockSelectWritten_ = false;
    if (bringupPolicy_.clockChangeObserver == nullptr) {
        return true;
    }
    const uint32_t rateHz = session_.desiredClock.sampleRateHz;
    const bool rateChanged =
        NominalRateHz(preClaimStatus_) != rateHz || DiceConfiguredRate(preClaimSampleRate_, preClaimStatus_, preClaimClockSelect_) != rateHz;
    bringupPolicy_.clockChangeObserver->AfterClockChange(io_, rateHz, rateChanged);
    return !TeardownRequested() && EnsureRouteCurrent();
}

IOReturn DiceFamilyDriver::DiscoverStreams() {
    const uint32_t txBase = sections_.txStreamFormat.offset;
    const uint32_t rxBase = sections_.rxStreamFormat.offset;

    // Stream 0 of each direction, register by register, as the reference trace
    // does. A names block is 256 bytes; a short one is an underrun.
    struct Read {
        uint32_t offset;
        uint32_t blockBytes;  // 0 = quadlet
    };
    const Read reads[] = {
        {txBase + TxOffset::kNumber, 0},      {rxBase + RxOffset::kNumber, 0},
        {txBase + TxOffset::kSize, 0},        {txBase + TxOffset::kIsochronous, 0},
        {txBase + TxOffset::kNumberAudio, 0}, {txBase + TxOffset::kNumberMidi, 0},
        {txBase + TxOffset::kSpeed, 0},       {txBase + TxOffset::kNames, kStreamNamesBytes},
        {rxBase + RxOffset::kSize, 0},        {rxBase + RxOffset::kIsochronous, 0},
        {rxBase + RxOffset::kNumberMidi, 0},  {rxBase + RxOffset::kSeqStart, 0},
        {rxBase + RxOffset::kNumberAudio, 0}, {rxBase + RxOffset::kNames, kStreamNamesBytes},
    };
    for (const auto& read : reads) {
        if (!EnsureRouteCurrent()) {
            return Rollback(kIOReturnOffline);
        }
        if (read.blockBytes == 0) {
            if (const auto value = io_.ReadQuad(read.offset); !value) {
                return Rollback(value.error());
            }
        } else {
            const auto block = io_.ReadBlock(read.offset, read.blockBytes);
            if (!block || block->size() < read.blockBytes) {
                return Rollback(block ? kIOReturnUnderrun : block.error());
            }
        }
    }
    return FinishPrepare();
}

IOReturn DiceFamilyDriver::FinishPrepare() {
    if (refreshRuntimeCapsOnPrepare_) {
        const IOReturn status = RefreshRuntimeCaps();
        if (status != kIOReturnSuccess) {
            return Rollback(status);
        }
    }
    AdoptPreparedClock(diceClock_);
    session_.devicePrepared = true;
    session_.deviceTxArmed = false;
    session_.deviceRunning = false;
    session_.deviceRxProgrammed = false;
    session_.phase = DuplexRestartPhase::kPrepared;
    session_.appliedClock = session_.desiredClock;
    return kIOReturnSuccess;
}

IOReturn DiceFamilyDriver::CompleteClockApply() {
    const IOReturn refreshStatus = RefreshRuntimeCaps();
    if (refreshStatus != kIOReturnSuccess) {
        return Rollback(refreshStatus);
    }

    // RefreshRuntimeCaps records the achieved rate. CLOCK_ACCEPTED alone
    // cannot replace that observation with the requested clock.
    // The owner stays ours: we hold it while the device is present.
    AdoptPreparedClock(diceClock_);
    ClearRestartProgress(session_);
    flowMode_ = FlowMode::kNone;
    return kIOReturnSuccess;
}

IOReturn DiceFamilyDriver::RefreshRuntimeCaps() {
    const auto global = io_.ReadGlobalState(sections_);
    if (!global) {
        return global.error();
    }
    const auto tx = io_.ReadTxStreamConfig(sections_);
    if (!tx) {
        return tx.error();
    }
    const auto rx = io_.ReadRxStreamConfig(sections_);
    if (!rx) {
        return rx.error();
    }
    CacheRuntimeCaps(runtimeCaps_, *global, *tx, *rx);
    session_.runtimeCaps = runtimeCaps_;
    session_.appliedClock = AudioClockConfig{.sampleRateHz = runtimeCaps_.sampleRateHz};
    return kIOReturnSuccess;
}

IOReturn DiceFamilyDriver::ProgramRxStreams() {
    const AudioDuplexChannels& channels = session_.channels;
    if (!EnsureRouteCurrent()) {
        return Rollback(kIOReturnOffline);
    }
    session_.phase = DuplexRestartPhase::kProgrammingDeviceRx;

    // RX_SIZE is the per-stream register stride.
    const auto rxSize = io_.ReadQuad(sections_.rxStreamFormat.offset + RxOffset::kSize);
    ASFW_LOG(DICE, "DoProgramRx: RX_SIZE transport status=%u value=0x%08x streams=%u",
             rxSize ? 0U : static_cast<unsigned>(rxSize.error()),
             rxSize ? *rxSize : 0U,
             channels.playbackStreamCount);
    if (!rxSize) {
        return Rollback(rxSize.error());
    }
    const uint32_t stride = *rxSize * 4U;

    // Every advertised stream is armed before the single GLOBAL_ENABLE.
    for (uint32_t streamIndex = 0;; ++streamIndex) {
        if (!EnsureRouteCurrent()) {
            return Rollback(kIOReturnOffline);
        }
        session_.phase = DuplexRestartPhase::kProgrammingDeviceRx;
        if (streamIndex >= channels.playbackStreamCount) {
            session_.deviceRxProgrammed = true;
            session_.phase = DuplexRestartPhase::kDeviceRxProgrammed;
            return kIOReturnSuccess;
        }

        const uint8_t isoChannel = channels.PlaybackChannel(streamIndex);
        const uint32_t streamBase = sections_.rxStreamFormat.offset + streamIndex * stride;
        ASFW_LOG(DICE, "DoProgramRx: stream %u writing RX isoch channel %u (stride=%u)",
                 streamIndex, isoChannel, stride);
        if (const auto iso = io_.WriteQuad(streamBase + RxOffset::kIsochronous, isoChannel); !iso) {
            return Rollback(iso.error());
        }
        if (const auto seq = io_.WriteQuad(streamBase + RxOffset::kSeqStart, kRxSeqStartDefault); !seq) {
            return Rollback(seq.error());
        }
    }
}

IOReturn DiceFamilyDriver::ProgramTxStreamsAndEnable() {
    const AudioDuplexChannels& channels = session_.channels;
    if (!EnsureRouteCurrent()) {
        return Rollback(kIOReturnOffline);
    }
    session_.phase = DuplexRestartPhase::kProgrammingDeviceTx;

    // TX_SIZE is the per-stream register stride.
    const auto txSize = io_.ReadQuad(sections_.txStreamFormat.offset + TxOffset::kSize);
    if (!txSize) {
        return Rollback(txSize.error());
    }
    const uint32_t stride = *txSize * 4U;

    for (uint32_t streamIndex = 0;; ++streamIndex) {
        if (!EnsureRouteCurrent()) {
            return Rollback(kIOReturnOffline);
        }
        session_.phase = DuplexRestartPhase::kProgrammingDeviceTx;
        if (streamIndex >= channels.captureStreamCount) {
            break;
        }

        const uint8_t isoChannel = channels.CaptureChannel(streamIndex);
        const uint32_t streamBase = sections_.txStreamFormat.offset + streamIndex * stride;
        const uint32_t txSpeed = ResolvedTxSpeed();
        ASFW_LOG(DICE, "DoProgramTx: stream %u writing TX isoch channel %u speed %u (stride=%u)",
                 streamIndex, isoChannel, txSpeed, stride);
        if (const auto iso = io_.WriteQuad(streamBase + TxOffset::kIsochronous, isoChannel); !iso) {
            return Rollback(iso.error());
        }
        if (const auto speed = io_.WriteQuad(streamBase + TxOffset::kSpeed, txSpeed); !speed) {
            return Rollback(speed.error());
        }
    }

    // Every TX (and before it every RX) stream is armed: assert GLOBAL_ENABLE once.
    if (!EnsureRouteCurrent()) {
        return Rollback(kIOReturnOffline);
    }
    if (const auto enabled = io_.WriteQuad(sections_.global.offset + GlobalOffset::kEnable, 1U);
        !enabled) {
        return Rollback(enabled.error());
    }
    ASFW_LOG(DICE, "[DiceStart] GLOBAL_ENABLE=1 write completed rate=%u gen=%u",
             session_.desiredClock.sampleRateHz, session_.generation.value);
    session_.deviceTxArmed = true;
    session_.phase = DuplexRestartPhase::kDeviceTxArmed;
    return kIOReturnSuccess;
}

IOReturn DiceFamilyDriver::CompleteConfirm(uint32_t notification, uint32_t status, uint32_t extStatus) {
    confirmNotification_ = notification;
    confirmStatus_ = status;
    confirmExtStatus_ = extStatus;

    const IOReturn refreshStatus = RefreshRuntimeCaps();
    if (refreshStatus != kIOReturnSuccess) {
        (void)Stop();
        return refreshStatus;
    }

    const bool channelsMatch =
        runtimeCaps_.deviceToHostIsoChannel == session_.channels.deviceToHostIsoChannel &&
        runtimeCaps_.hostToDeviceIsoChannel == session_.channels.hostToDeviceIsoChannel;
    if (!channelsMatch) {
        ASFW_LOG_ERROR(DICE,
                       "ConfirmDuplex48kStart: stream channel readback mismatch expected d2h=%u h2d=%u actual d2h=%u h2d=%u",
                       session_.channels.deviceToHostIsoChannel,
                       session_.channels.hostToDeviceIsoChannel,
                       runtimeCaps_.deviceToHostIsoChannel,
                       runtimeCaps_.hostToDeviceIsoChannel);
        (void)Stop();
        return kIOReturnNotReady;
    }

    session_.deviceRunning = true;
    session_.phase = DuplexRestartPhase::kRunning;
    session_.appliedClock = session_.desiredClock;
    ASFW_LOG(DICE, "ConfirmDuplex48kStart: sourceLock=%u notify=0x%08x status=0x%08x (%{public}s) ext=0x%08x",
             IsSourceLocked(status) ? 1U : 0U, notification, status, ASFW::Logging::IOReturnName(status), extStatus);
    // A receiver that never locks, or loses lock, gets one hardware dump where
    // the firmware read path is verified (no-op elsewhere).
    lockWatch_.Start(io_.RegisterIo(), scheduler_, sections_, runtimeCaps_,
                     receiverDump_.VerdictTrigger(io_.RegisterIo(), scheduler_));
    return kIOReturnSuccess;
}

IOReturn DiceFamilyDriver::Rollback(IOReturn error) {
    // The terminal error is recorded by the caller when it enters Failed; the
    // rollback itself runs with the progress flags intact so Stop knows what to undo.
    session_.phase = DuplexRestartPhase::kFailed;

    if (!session_.ownerConfirmed || !EnsureRouteCurrent()) {
        ResetSession(session_);
        flowMode_ = FlowMode::kNone;
        return error;
    }

    const IOReturn stopStatus = StopSequence();
    if (stopStatus != kIOReturnSuccess) {
        ASFW_LOG(DICE, "DoRollback: cleanup reported 0x%x (%{public}s) after start failure 0x%x", stopStatus, ASFW::Logging::IOReturnName(stopStatus), error);
    }
    flowMode_ = FlowMode::kNone;
    return error;
}

// ---------------------------------------------------------------------------
// Stop sequence
// ---------------------------------------------------------------------------

IOReturn DiceFamilyDriver::StopSequence() {
    stopSequenceError_ = kIOReturnSuccess;
    session_.phase = DuplexRestartPhase::kStopping;
    if (AbortStopIfTeardown("SequenceEntry")) {
        return stopSequenceError_;
    }

    // GLOBAL_ENABLE off.
    if (AbortStopIfTeardown("DisableGlobal")) {
        return stopSequenceError_;
    }
    if (!EnsureRouteCurrent()) {
        RecordFirstError(stopSequenceError_, kIOReturnOffline);
        ResetSession(session_);
        return stopSequenceError_;
    }
    RecordFirstError(stopSequenceError_,
                     ErrorOr(io_.WriteQuad(sections_.global.offset + GlobalOffset::kEnable, 0U)));
    if (AbortStopIfTeardown("DisableGlobalComplete")) {
        return stopSequenceError_;
    }

    // Clear EVERY stream's ISOC register, not just stream[0]'s (FFADO
    // stopStreamByIndex writes 0xFFFFFFFF per stream): a stream[0]-only clear
    // leaves a stale channel that the next bring-up can adopt, and a start over a
    // stale duplicate channel wedges the device until a power cycle. The stride
    // comes from TX_SIZE/RX_SIZE; if that read fails only stream[0] is cleared.
    if (AbortStopIfTeardown("DisableTx")) {
        return stopSequenceError_;
    }
    if (!EnsureRouteCurrent()) {
        RecordFirstError(stopSequenceError_, kIOReturnOffline);
        return stopSequenceError_;
    }
    const auto txSize = io_.ReadQuad(sections_.txStreamFormat.offset + TxOffset::kSize);
    RecordFirstError(stopSequenceError_, ErrorOr(txSize));
    if (AbortStopIfTeardown("DisableTxReadComplete")) {
        return stopSequenceError_;
    }
    const uint32_t txStride = txSize ? *txSize * 4U : 0U;
    const uint32_t txCount = ClampedStreamCount(session_.channels.captureStreamCount, txStride);
    for (uint32_t i = 0; i < txCount; ++i) {
        const uint32_t streamBase = sections_.txStreamFormat.offset + i * txStride;
        RecordFirstError(stopSequenceError_,
                         ErrorOr(io_.WriteQuad(streamBase + TxOffset::kIsochronous, kDisabledIsoChannel)));
        if (AbortStopIfTeardown("DisableTxIsoComplete")) {
            return stopSequenceError_;
        }
        RecordFirstError(stopSequenceError_,
                         ErrorOr(io_.WriteQuad(streamBase + TxOffset::kSpeed, ResolvedTxSpeed())));
        if (AbortStopIfTeardown("DisableTxSpeedComplete")) {
            return stopSequenceError_;
        }
    }
    if (AbortStopIfTeardown("ReleaseTx")) {
        return stopSequenceError_;
    }

    if (AbortStopIfTeardown("DisableRx")) {
        return stopSequenceError_;
    }
    if (!EnsureRouteCurrent()) {
        RecordFirstError(stopSequenceError_, kIOReturnOffline);
        return stopSequenceError_;
    }
    const auto rxSize = io_.ReadQuad(sections_.rxStreamFormat.offset + RxOffset::kSize);
    RecordFirstError(stopSequenceError_, ErrorOr(rxSize));
    if (AbortStopIfTeardown("DisableRxReadComplete")) {
        return stopSequenceError_;
    }
    const uint32_t rxStride = rxSize ? *rxSize * 4U : 0U;
    const uint32_t rxCount = ClampedStreamCount(session_.channels.playbackStreamCount, rxStride);
    for (uint32_t i = 0; i < rxCount; ++i) {
        const uint32_t streamBase = sections_.rxStreamFormat.offset + i * rxStride;
        RecordFirstError(stopSequenceError_,
                         ErrorOr(io_.WriteQuad(streamBase + RxOffset::kIsochronous, kDisabledIsoChannel)));
        if (AbortStopIfTeardown("DisableRxIsoComplete")) {
            return stopSequenceError_;
        }
        RecordFirstError(stopSequenceError_,
                         ErrorOr(io_.WriteQuad(streamBase + RxOffset::kSeqStart, kRxSeqStartDefault)));
        if (AbortStopIfTeardown("DisableRxSeqComplete")) {
            return stopSequenceError_;
        }
    }
    if (AbortStopIfTeardown("ReleaseRx")) {
        return stopSequenceError_;
    }

    // The owner is not released: we hold it while the device is present, as
    // TCAT and Linux do, so its notifications keep reaching us.
    ResetSession(session_);
    flowMode_ = FlowMode::kNone;
    return stopSequenceError_;
}

bool DiceFamilyDriver::AbortStopIfTeardown(const char* stage) {
    if (!TeardownRequested()) {
        return false;
    }
    stopSequenceError_ = kIOReturnAborted;
    ResetSession(session_);
    flowMode_ = FlowMode::kNone;
    RecordStopTeardownAbort(stage);
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool DiceFamilyDriver::EnsureRouteCurrent() const noexcept {
    if (session_.phase == DuplexRestartPhase::kIdle) {
        return true;
    }
    return io_.RegisterIo().IsRouteCurrent();
}

bool DiceFamilyDriver::TeardownRequested() const noexcept {
    return teardownCancel_ != nullptr && teardownCancel_->load(std::memory_order_acquire);
}

uint64_t DiceFamilyDriver::OwnerValue() const noexcept {
    const uint64_t localNodeId =
        0xFFC0ULL | static_cast<uint64_t>(busInfo_.GetLocalNodeID().value & 0x3FU);
    return (localNodeId << kOwnerNodeShift) | kNotificationHandlerOffset;
}

uint32_t DiceFamilyDriver::ResolvedTxSpeed() const noexcept {
    // Device TX is host capture. Use the resolved operational link speed,
    // including slower verified links and S800, without a family-wide cap.
    // Cross-validated with Linux dice-stream.c:329, :359-369; its resource
    // reservation uses the same device max_speed at :192-194.
    return static_cast<uint32_t>(busInfo_.GetSpeed(io_.RegisterIo().NodeId()));
}

void DiceFamilyDriver::RecordStopTeardownAbort(const char* stage) const noexcept {
    ASFW_LOG(DICE, "DiceFamilyDriver: StopDuplex aborted by teardown stage=%{public}s kr=0x%x",
             stage ? stage : "unknown", kIOReturnAborted);
}

} // namespace ASFW::Audio::DICE
