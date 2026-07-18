#include "IsochReceiveContext.hpp"
#include "../Core/IsochEventGroup.hpp"
#include "../../Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../../Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"

#include "../../Common/TimingUtils.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Hardware/RegisterMap.hpp"
#include "../../Diagnostics/Signposts.hpp"

#include <utility>

namespace ASFW::Isoch {

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<IsochReceiveContext> IsochReceiveContext::Create(::ASFW::Driver::HardwareInterface* hw,
                                                            std::shared_ptr<::ASFW::Isoch::Memory::IIsochDMAMemory> dmaMemory) {
    auto ctx = std::unique_ptr<IsochReceiveContext>(new (std::nothrow) IsochReceiveContext());
    if (!ctx) return nullptr;

    ctx->hardware_ = hw;
    ctx->dmaMemory_ = std::move(dmaMemory);

    return ctx;
}

// ============================================================================
// Lifecycle
// ============================================================================

IsochReceiveContext::~IsochReceiveContext() {
    (void)Stop();
}

// ============================================================================
// Configuration
// ============================================================================

IsochReceiveContext::Registers IsochReceiveContext::GetRegisters(uint8_t index) const {
    return Registers{
        .CommandPtr          = static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvCommandPtr(index)),
        .ContextControlSet   = static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvContextControlSet(index)),
        .ContextControlClear = static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvContextControlClear(index)),
        .ContextMatch        = static_cast<::ASFW::Driver::Register32>(::DMAContextHelpers::IsoRcvContextMatch(index)),
    };
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t IsochReceiveContext::Configure(uint8_t channel,
                                            uint8_t contextIndex,
                                            Encoding::AudioWireFormat wireFormat,
                                            uint32_t am824Slots,
                                            uint32_t channelOffset,
                                            uint32_t streamChannels,
                                            bool isSecondary) {
    if (!hardware_ || !dmaMemory_) {
        return kIOReturnNotReady;
    }

    if (contextIndex >= 4) {
        return kIOReturnBadArgument;
    }

    contextIndex_ = contextIndex;
    channel_ = channel;
    registers_ = GetRegisters(contextIndex_);
    wireFormat_ = wireFormat;
    am824Slots_ = am824Slots;
    channelOffset_ = channelOffset;
    streamChannels_ = streamChannels;
    isSecondary_ = isSecondary;

    return rxRing_.SetupRings(*dmaMemory_, kNumDescriptors, kMaxPacketSize);
}

// ============================================================================
// Runtime
// ============================================================================

kern_return_t IsochReceiveContext::Start() {
    if (GetState() != IRPolicy::State::Stopped) {
        return kIOReturnInvalid;
    }

    if (!hardware_) {
        ASFW_LOG(Isoch, "❌ Start: hardware_ is null!");
        return kIOReturnNotReady;
    }

    const uint32_t contextMatch = 0xF0000000 | (channel_ & 0x3F);
    hardware_->Write(registers_.ContextMatch, contextMatch);

    const uint32_t cmdPtr = rxRing_.InitialCommandPtrWord();
    if (cmdPtr == 0) {
        ASFW_LOG(Isoch, "❌ Start: Invalid descriptor cmdPtr");
        return kIOReturnInternalError;
    }
    hardware_->Write(registers_.CommandPtr, cmdPtr);

    hardware_->Write(registers_.ContextControlClear, 0xFFFFFFFFu);
    const uint32_t ctlValue = Driver::ContextControl::kRun | Driver::ContextControl::kIsochHeader;
    hardware_->Write(registers_.ContextControlSet, ctlValue);

    const uint32_t contextMask = 1u << contextIndex_;
    hardware_->Write(ASFW::Driver::Register32::kIsoRecvIntMaskSet, contextMask);
    ASFW_LOG(Isoch, "Start: Enabled IR interrupt for context %u (mask=0x%08x)", contextIndex_, contextMask);

    while (rxLock_.test_and_set(std::memory_order_acquire)) {
    }

    Transition(IRPolicy::State::Running, 0, "Start");
    rxRing_.ResetForStart();
    absoluteFrameCursor_ = 0;
    cursorInitialized_ = false;
    secondaryAnchored_ = false;
    secondaryAnchorEpoch_ = 0;
    dbcInitialized_ = false;
    lastDbc_ = 0;
    rxZtsPublishCount_ = 0;
    rxTimestampValidCount_ = 0;
    rxTimestampInvalidCount_ = 0;
    rxCadenceEstablishedLogged_ = false;
    replayReadyNotified_ = false;
    replayResetForStart_ = false;
    replayCycleInitialized_ = false;
    lastReplayCycleOrdinal_ = 0;
    ztsTelemetry_.Reset();
    ztsTelemetryLogGate_.Reset();
    prevLoggedAnchorFrame_ = 0;
    prevLoggedAnchorHostTicks_ = 0;
    prevLoggedAnchorRate_ = 0;
    prevLoggedAnchorValid_ = false;
    (void)ASFW::Timing::initializeHostTimebase();

    rxLock_.clear(std::memory_order_release);
    return kIOReturnSuccess;
}

kern_return_t IsochReceiveContext::Stop() {
    while (rxLock_.test_and_set(std::memory_order_acquire)) {
    }

    if (GetState() == IRPolicy::State::Stopped) {
        rxLock_.clear(std::memory_order_release);
        return kIOReturnSuccess;
    }

    const uint32_t contextMask = 1u << contextIndex_;
    hardware_->Write(ASFW::Driver::Register32::kIsoRecvIntMaskClear, contextMask);
    // Flush RUN-clear and wait for ACTIVE to fall before dropping the direct
    // audio binding.  See Linux firewire/ohci.c:1361-1378 for the same
    // teardown ordering; freeing this mapping while ACTIVE is set can fault
    // the host when OHCI completes a late DMA write.
    hardware_->WriteAndFlush(registers_.ContextControlClear, Driver::ContextControl::kRun);
    ASFW_LOG(Isoch, "Stop: Disabled IR interrupt for context %u", contextIndex_);

    if ((hardware_->Read(registers_.ContextControlSet) & Driver::ContextControl::kActive) != 0) {
        IODelay(5);
        constexpr uint32_t kMaxIterations = 250;
        constexpr uint32_t kBaseDelayMicros = 6;
        for (uint32_t iteration = 0; iteration < kMaxIterations; ++iteration) {
            if ((hardware_->Read(registers_.ContextControlSet) & Driver::ContextControl::kActive) == 0) {
                break;
            }
            IODelay(kBaseDelayMicros + iteration);
        }
    }

    const uint32_t control = hardware_->Read(registers_.ContextControlSet);
    if ((control & Driver::ContextControl::kActive) != 0) {
        const kern_return_t failure = (control & Driver::ContextControl::kDead) != 0
            ? kIOReturnDMAError
            : kIOReturnTimeout;
        ASFW_LOG_ERROR(Isoch,
                       "IR: stop did not quiesce context=%u control=0x%08x kr=0x%08x; retaining direct binding",
                       contextIndex_, control, failure);
        rxLock_.clear(std::memory_order_release);
        return failure;
    }

    Transition(IRPolicy::State::Stopped, 0, "Stop");

    rxLock_.clear(std::memory_order_release);
    return kIOReturnSuccess;
}

uint32_t IsochReceiveContext::Poll() {
    if (rxLock_.test_and_set(std::memory_order_acquire)) {
        return 0;
    }

    if (GetState() != IRPolicy::State::Running) {
        rxLock_.clear(std::memory_order_release);
        return 0;
    }

    if (directAudioBindingSource_) {
        ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
        if (directAudioBindingSource_->CopyDirectAudioBinding(snapshot)) {
            const bool bindingChanged = snapshot.generation != lastDirectAudioGeneration_;
            if (bindingChanged) {
                // The producer's cumulative counters are scoped to its control
                // block. Rebase the drain-side comparison before observing a
                // newly published binding.
                payloadWriterTelemetryAggregator_.Reset();
                if (snapshot.valid && snapshot.HasInput()) {
                    ASFW_LOG(Isoch,
                             "IR: direct audio binding changed (gen %llu -> %llu). Arming direct Rx inBase=%p inFrames=%u inCh=%u outBase=%p outFrames=%u outCh=%u control=%p rate=%u",
                             lastDirectAudioGeneration_,
                             snapshot.generation,
                             static_cast<void*>(snapshot.inputBase),
                             snapshot.inputFrames,
                             snapshot.inputChannels,
                             static_cast<const void*>(snapshot.outputBase),
                             snapshot.outputFrames,
                             snapshot.outputChannels,
                             static_cast<void*>(snapshot.control),
                             snapshot.sampleRateHz);

                    directInputView_.guid = 0;
                    directInputView_.sampleRateHz = snapshot.sampleRateHz;
                    directInputView_.memory.inputBase = snapshot.inputBase;
                    directInputView_.memory.inputFrameCapacity = snapshot.inputFrames;
                    directInputView_.memory.inputChannels = snapshot.inputChannels;
                    directInputView_.memory.storage = ASFW::Audio::Runtime::AudioSampleStorage::kFloat32Native;
                    directInputView_.control = snapshot.control;
                    directInputView_.deviceToHostAm824Slots = am824Slots_ > 0 ? am824Slots_ : snapshot.inputChannels;
                    directInputView_.hostToDeviceAm824Slots = snapshot.outputChannels;
                    directInputView_.streamMode = ASFW::Audio::Runtime::AudioStreamMode::kUnknown;
                    directInputView_.hostToDeviceWireFormat = ASFW::Audio::Runtime::AudioWireFormat::kAM824;

                    // Master-only: reset the shared clock/replay timeline on every
                    // (re)bind. A secondary slice must never touch the shared control
                    // block's cadence/replay — the master owns it. This block is
                    // generation-gated (runs once per NEW binding generation), so a
                    // rate change — which bumps the binding generation while the IR
                    // context keeps running (no Start(), so replayResetForStart_ stays
                    // set) — MUST reset here too. Gating on replayResetForStart_ skipped
                    // it, leaving the master with stale 48k replay/cadence at 44.1k:
                    // ch1-16 (master) went dead while the secondary (ch17-32) survived.
                    if (!isSecondary_ && directInputView_.control) {
                        directInputView_.control->rxSytCadence.Reset();
                        directInputView_.control->rxSequenceReplay.Reset();
                        directInputView_.control->rxReplayEpochResets.fetch_add(
                            1, std::memory_order_relaxed);
                        replayResetForStart_ = true;
                        ASFW_LOG(Isoch,
                                 "IR: master replay/cadence reset on rebind (gen %llu rate=%u)",
                                 snapshot.generation, snapshot.sampleRateHz);
                    }

                    // Data plane (RX -> input buffer) arms for every stream. The
                    // controller-side clock publisher is master-only: it writes
                    // the shared timeline the ADK side mirrors to HAL.
                    directInputWriter_.Bind(&directInputView_);
                    if (!isSecondary_) {
                        clockPublisher_.Bind(&directInputView_);
                    }
                } else {
                    ASFW_LOG(Isoch,
                             "IR: direct audio binding invalid or has no input (gen %llu -> %llu). Disarming valid=%d hasIn=%d control=%p rate=%u",
                             lastDirectAudioGeneration_,
                             snapshot.generation,
                             snapshot.valid,
                             snapshot.HasInput(),
                             static_cast<void*>(snapshot.control),
                             snapshot.sampleRateHz);
                    directInputWriter_.Unbind();
                    clockPublisher_.Unbind();
                    // FW-62: drop the stale view so the non-null guard in
                    // IsReplayEstablished() is meaningful. Unbind() only detaches the
                    // writer/publisher; .control would otherwise dangle into freed
                    // audio-owned memory. Re-arm the one-shot replay reset on rebind.
                    directInputView_ = {};
                    replayResetForStart_ = false;
                }
                lastDirectAudioGeneration_ = snapshot.generation;
            }
        } else {
            if (lastDirectAudioGeneration_ != 0) {
                ASFW_LOG(Isoch, "IR: direct audio binding cleared/unavailable. Disarming.");
                directInputWriter_.Unbind();
                clockPublisher_.Unbind();
                // FW-62: see above — null the view so .control can't dangle.
                directInputView_ = {};
                replayResetForStart_ = false;
                lastDirectAudioGeneration_ = 0;
            }
        }
    }

    const auto cycleHostPair =
        hardware_
            ? hardware_->ReadCycleTimeAndUpTime()
            : std::pair<uint32_t, uint64_t>{0, mach_absolute_time()};
    const uint32_t drainCycleTimer = cycleHostPair.first;
    const uint64_t drainHostTicks = cycleHostPair.second;

    const uint32_t processed = rxRing_.DrainCompleted(
        *dmaMemory_,
        [this, drainHostTicks, drainCycleTimer](
            const Rx::IsochRxDmaRing::CompletedPacket& pkt) {
        uint64_t callbackTimestamp = 0;
        if (pkt.payload) {
            // Anchor a secondary slice to the master's published ring position
            // before writing. The master owns inputProducedEndFrame; this stream
            // is frame-locked to it but its OHCI context armed/started at a
            // different time, so its private cursor must be re-based to the
            // master's leading edge once per replay epoch. Until the master has
            // produced (masterEnd != 0) we drop this stream's packets rather than
            // write a mis-anchored slice.
            if (isSecondary_ && directInputView_.control) {
                const uint64_t epoch = directInputView_.control->rxReplayEpochResets.load(
                    std::memory_order_acquire);
                if (!secondaryAnchored_ || epoch != secondaryAnchorEpoch_) {
                    const uint64_t masterEnd =
                        directInputView_.control->inputProducedEndFrame.load(
                            std::memory_order_acquire);
                    if (masterEnd == 0) {
                        return;
                    }
                    absoluteFrameCursor_ = masterEnd;
                    secondaryAnchored_ = true;
                    secondaryAnchorEpoch_ = epoch;
                }
            }

            const uint64_t packetFirstAudioFrame =
                absoluteFrameCursor_;
            // This stream decodes its own slice width (streamChannels_), or the
            // binding's full width for single-stream devices, and writes it at
            // channelOffset_. Only the master publishes the producer timeline.
            const uint32_t channels = streamChannels_ > 0
                ? streamChannels_
                : directInputView_.memory.inputChannels;
            const uint32_t slots = directInputView_.deviceToHostAm824Slots;
            const auto result = directProcessor_.ProcessPacket(pkt.payload,
                                                               pkt.actualLength,
                                                               packetFirstAudioFrame,
                                                               channels,
                                                               slots,
                                                               wireFormat_,
                                                               channelOffset_,
                                                               /*publishTimeline=*/!isSecondary_);
            const bool packetAccepted =
                result.status ==
                    AudioEngine::Direct::Rx::
                        DirectRxWriteStatus::kAvailable ||
                result.status ==
                    AudioEngine::Direct::Rx::
                        DirectRxWriteStatus::kInvalidBinding;

            // Secondary slice: PCM is already written at its channel offset.
            // Skip all master-only bookkeeping (clock/replay/ZTS/DBC/callback);
            // just advance this stream's frame cursor in lockstep with the
            // master (both are frame-locked by the device clock). Under packet
            // loss the two halves can transiently skew until a discontinuity
            // re-anchors — the input safety offset absorbs the sub-cycle case.
            if (isSecondary_) {
                if (packetAccepted) {
                    absoluteFrameCursor_ =
                        packetFirstAudioFrame + result.framesDecoded;
                }
                return;
            }
            if (!packetAccepted) {
                ResetReplayEpochForDiscontinuity();
            } else {
                
                if (!cursorInitialized_ && result.hasValidCip) {
                    // First valid CIP packet anchors the RX device cursor (starts at 0).
                    // We do not load or reset to inputProducedEndFrame on late binding to keep the timeline monotonic.
                    cursorInitialized_ = true;
                }

                // Track DBC delta for device-domain frame count.
                // DBC is the device's own frame counter in the CIP header —
                // authoritative regardless of sample rate, blocking mode, or
                // CIP geometry. The 8-bit DBC wraps at 256; handle wrap.
                if (result.hasValidCip) {
                    if (dbcInitialized_) {
                        const uint8_t dbcDelta =
                            static_cast<uint8_t>(result.dbc - lastDbc_);
                        if (directInputView_.control) {
                            directInputView_.control->rxDbcFrameCount.fetch_add(
                                dbcDelta, std::memory_order_relaxed);
                        }
                    }
                    lastDbc_ = result.dbc;
                    dbcInitialized_ = true;
                }

                Rx::ExpandedReceiveTimestamp rxTimestamp{};
                const bool hasHardwareTimestamp =
                    result.hasReceiveCycleTimestamp &&
                    Rx::ExpandReceiveTimestamp(
                        result.receiveCycleTimestamp,
                        drainCycleTimer,
                        rxTimestamp);
                uint64_t packetReceiveHostTicks = 0;
                if (hasHardwareTimestamp) {
                    ++rxTimestampValidCount_;
                    if (rxTimestamp.ageTicks >= 0) {
                        const uint64_t ageHostTicks =
                            ASFW::Timing::nanosToHostTicks(
                                Rx::FireWireTicksToNanos(
                                    static_cast<uint64_t>(
                                        rxTimestamp.ageTicks)));
                        packetReceiveHostTicks =
                            drainHostTicks > ageHostTicks
                                ? drainHostTicks - ageHostTicks
                                : drainHostTicks;
                    } else {
                        // Packet cycle stamp is ahead of the master drain read:
                        // advance (not rewind) the receive host time. Small,
                        // occasional negatives are fine; track magnitude so a
                        // frequent/large pattern (bad expansion/pairing) shows.
                        ++rxNegativeAgeCount_;
                        const bool largeNegativeAge =
                            -rxTimestamp.ageTicks >=
                            static_cast<int64_t>(ASFW::Timing::kTicksPerCycle);
                        if (largeNegativeAge) {
                            ++rxLargeNegativeAgeCount_;
                            if (rxLargeNegativeAgeCount_ <= 8 ||
                                (rxLargeNegativeAgeCount_ % 64) == 0) {
                                ASFW_LOG(
                                    Isoch,
                                    "IR negative age >= 1 cycle: age=%lld "
                                    "desc=%u drainCycle=0x%08x rxCycle=0x%08x "
                                    "largeNeg=%llu totalNeg=%llu valid=%llu",
                                    rxTimestamp.ageTicks,
                                    pkt.descriptorIndex,
                                    drainCycleTimer,
                                    rxTimestamp.cycleTimer,
                                    rxLargeNegativeAgeCount_,
                                    rxNegativeAgeCount_,
                                    rxTimestampValidCount_);
                            }
                        }
                        const uint64_t advanceHostTicks =
                            ASFW::Timing::nanosToHostTicks(
                                Rx::FireWireTicksToNanos(
                                    static_cast<uint64_t>(
                                        -rxTimestamp.ageTicks)));
                        packetReceiveHostTicks =
                            drainHostTicks + advanceHostTicks;
                    }
                    callbackTimestamp = rxTimestamp.cycleTimer;
                } else {
                    ++rxTimestampInvalidCount_;
                    ResetReplayEpochForDiscontinuity();
                    if (rxTimestampInvalidCount_ <= 8 ||
                        (rxTimestampInvalidCount_ % 128) == 0) {
                        ASFW_LOG(
                            Isoch,
                            "IR RX timestamp invalid count=%llu desc=%u hasRaw=%d raw=0x%04x drainCycle=0x%08x len=%u",
                            rxTimestampInvalidCount_,
                            pkt.descriptorIndex,
                            result.hasReceiveCycleTimestamp,
                            result.receiveCycleTimestamp,
                            drainCycleTimer,
                            pkt.actualLength);
                    }
                }

                // Two clocks, never conflated (Saffire ReadFirewireBuffers):
                // the SYT delta history recovers the DEVICE clock and feeds
                // TX phase only; the HAL zero timestamp below is pure HOST
                // receive time (IR drain uptime back-corrected per packet).
                // SYT upkeep happens on SYT-bearing packets; anchor
                // publication is gated only on the history being established,
                // not on the current packet carrying a SYT.
                bool rxClockEstablished = false;
                int64_t sytLeadTicks = 0;
                uint32_t rxRollingCadenceTicks = 0;
                int64_t rxRecoveredPhaseTicks = 0;
                if (hasHardwareTimestamp && directInputView_.control) {
                    const auto cycleFields =
                        ASFW::Timing::decodeCycleTimer(
                            rxTimestamp.cycleTimer);
                    const uint32_t cycleOrdinal =
                        cycleFields.seconds *
                            ASFW::Timing::kCyclesPerSecond +
                        cycleFields.cycle;
                    constexpr uint32_t kCycleDomain =
                        ASFW::Timing::kFWTimeWrapSeconds *
                        ASFW::Timing::kCyclesPerSecond;
                    if (replayCycleInitialized_ &&
                        cycleOrdinal !=
                            (lastReplayCycleOrdinal_ + 1) %
                                kCycleDomain) {
                        ResetReplayEpochForDiscontinuity();
                    }
                    lastReplayCycleOrdinal_ = cycleOrdinal;
                    replayCycleInitialized_ = true;

                    ASFW::Audio::Runtime::RxSequenceEntry replayEntry{};
                    replayEntry.firstAudioFrame =
                        packetFirstAudioFrame;
                    replayEntry.sourceCycleTimer = rxTimestamp.cycleTimer;
                    replayEntry.dataBlocks =
                        static_cast<uint16_t>(result.framesDecoded);
                    replayEntry.dbc = result.dbc;
                    if (result.hasValidCip) {
                        replayEntry.flags |=
                            ASFW::Audio::Runtime::RxSequenceFlags::
                                kValidCip;
                    }
                    if (result.hasValidCip && result.syt != 0xFFFF) {
                        const bool cadenceAccepted =
                            directInputView_.control
                                ->rxSytCadence.Observe(
                                    result.syt,
                                    rxTimestamp.cycleTimer);
                        if (!cadenceAccepted &&
                            directInputView_.control
                                ->rxSequenceReplay.IsEstablished()) {
                            ResetReplayEpochForDiscontinuity();
                        }
                        replayEntry.sytOffset =
                            ASFW::Audio::Runtime::
                                ComputeReplaySytOffset(
                                    result.syt,
                                    rxTimestamp.cycleTimer,
                                    directInputView_.control
                                        ->rxTransferDelayTicks.load(
                                            std::memory_order_relaxed));
                        replayEntry.flags |=
                            ASFW::Audio::Runtime::RxSequenceFlags::
                                kValidSyt;
                        const int64_t receiveTicks =
                            ASFW::Timing::encodedTstampToOffsets(
                                rxTimestamp.cycleTimer);
                        const int64_t presentationTicks =
                            ASFW::Timing::extendTstampFromCycleTimer(
                                rxTimestamp.cycleTimer, result.syt);
                        sytLeadTicks = ASFW::Timing::extOffsetDiff(
                            presentationTicks, receiveTicks);
                    }
                    directInputView_.control->rxSequenceReplay.Publish(
                        replayEntry);
                    directInputView_.control->rxReplayEntries.fetch_add(
                        1, std::memory_order_relaxed);

                    ASFW::Driver::RxSytCadence::Snapshot cadence{};
                    if (directInputView_.control->rxSytCadence.TrySnapshot(
                            cadence) &&
                        cadence.established) {
                        rxClockEstablished = true;
                        rxRollingCadenceTicks = cadence.rollingCadenceTicks;
                        rxRecoveredPhaseTicks = cadence.recoveredPhaseTicks;
                        if (!directInputView_.control
                                 ->rxSequenceReplay.IsEstablished()) {
                            (void)directInputView_.control
                                ->rxSequenceReplay.MarkEstablished();
                        }
                        if (directInputView_.control
                                ->rxSequenceReplay.IsEstablished() &&
                            !replayReadyNotified_ &&
                            replayReadyCallback_) {
                            replayReadyNotified_ = true;
                            replayReadyCallback_();
                        }
                        if (!rxCadenceEstablishedLogged_) {
                            const uint16_t delayedReadIndex =
                                static_cast<uint16_t>(
                                    (cadence.writeIndex +
                                     ASFW::Driver::RxSytCadence::kReadDelay) &
                                    (ASFW::Driver::RxSytCadence::kEntryCount -
                                     1));
                            ASFW_LOG(
                                Isoch,
                                "IR RX CADENCE ESTABLISHED updates=%u rollingTicks=%u readIndex=%u",
                                cadence.validUpdates,
                                cadence.rollingCadenceTicks,
                                delayedReadIndex);
                            ASFW_LOG(
                                Isoch,
                                "IR SYT ZTS QUALIFIED syt=0x%04x fdf=0x%02x dbs=%u rawRxTs=0x%04x",
                                result.syt,
                                result.fdf,
                                result.dbs,
                                result.receiveCycleTimestamp);
                            // The stream FDF is the device's ground-truth
                            // nominal rate. A mismatch against the session
                            // rate means host and device stream at different
                            // rates — audible as pitch shift/skew but
                            // otherwise silent (observed: Apogee Duet
                            // claiming 44.1 kHz over AV/C while streaming
                            // FDF 0x02 = 48 kHz; BUGLIST.md Bug 5).
                            const auto sessionGeometry =
                                ASFW::Encoding::AmdtpRateGeometryForSampleRate(
                                    directInputView_.sampleRateHz);
                            if (sessionGeometry &&
                                sessionGeometry->fdf != result.fdf) {
                                const auto deviceGeometry =
                                    ASFW::Encoding::AmdtpRateGeometryForFdf(
                                        result.fdf);
                                ASFW_LOG_ERROR(
                                    Isoch,
                                    "IR RATE MISMATCH sessionRate=%u (fdf=0x%02x) "
                                    "deviceFdf=0x%02x (rate=%u) — device streams "
                                    "at a different nominal rate than the session",
                                    directInputView_.sampleRateHz,
                                    sessionGeometry->fdf,
                                    result.fdf,
                                    deviceGeometry ? deviceGeometry->sampleRateHz
                                                   : 0);
                            }
                            rxCadenceEstablishedLogged_ = true;
                        }
                    }
                }

                const uint32_t hostNanosPerSampleQ8 =
                    directInputView_.sampleRateHz == 0
                        ? 0
                        : static_cast<uint32_t>(
                              (1000000000ULL << 8) /
                              directInputView_.sampleRateHz);
                constexpr uint64_t kZtsPeriodFrames =
                    ASFW::IsochTransport::AudioTimingGeometry::
                        kHalZeroTimestampPeriodFrames;
                const bool observedZtsBoundary =
                    result.framesDecoded != 0 &&
                    kZtsPeriodFrames != 0 &&
                    (packetFirstAudioFrame % kZtsPeriodFrames) == 0;
                if (observedZtsBoundary &&
                    rxClockEstablished &&
                    packetReceiveHostTicks != 0 &&
                    clockPublisher_.IsBound() &&
                    hostNanosPerSampleQ8 != 0) {
                    const auto publishResult =
                        clockPublisher_.Publish(
                            packetFirstAudioFrame,
                            packetReceiveHostTicks,
                            hostNanosPerSampleQ8);
                    if (!publishResult.accepted) {
                        ResetReplayEpochForDiscontinuity();
                    } else {
                        ++rxZtsPublishCount_;
                        if (publishResult.notifyConsumer &&
                            ztsAnchorReadyCallback_) {
                            ztsAnchorReadyCallback_(
                                publishResult
                                    .notificationGeneration);
                        }

                        Rx::ZtsTelemetryRecord rec{};
                        rec.publishCount = rxZtsPublishCount_;
                        rec.sampleFrame = packetFirstAudioFrame;
                        rec.hostTicks = packetReceiveHostTicks;
                        rec.rawHostTicks = packetReceiveHostTicks;
                        rec.drainHostTicks = drainHostTicks;
                        rec.ageTicks = rxTimestamp.ageTicks;
                        rec.sytLeadTicks = sytLeadTicks;
                        rec.recoveredPhaseTicks =
                            rxRecoveredPhaseTicks;
                        rec.rollingCadenceTicks =
                            rxRollingCadenceTicks;
                        rec.drainCycleTimer = drainCycleTimer;
                        rec.rxCycleTimer = rxTimestamp.cycleTimer;
                        rec.descriptorIndex = pkt.descriptorIndex;
                        rec.framesDecoded = result.framesDecoded;
                        rec.hostNanosPerSampleQ8 =
                            hostNanosPerSampleQ8;
                        rec.rawRxTs =
                            result.receiveCycleTimestamp;
                        rec.syt = result.syt;
                        rec.kind = static_cast<uint8_t>(
                            rxZtsPublishCount_ == 1
                                ? Rx::ZtsEventKind::kSeed
                                : Rx::ZtsEventKind::kUpdate);
                        ztsTelemetry_.Record(rec);
                    }
                }
                absoluteFrameCursor_ =
                    packetFirstAudioFrame + result.framesDecoded;
            }
        }

        if (callback_) {
            const auto span = std::span<const uint8_t>(pkt.payload, pkt.actualLength);
            callback_(span,
                      static_cast<uint32_t>(pkt.xferStatus),
                      callbackTimestamp);
        }
    });

    rxLock_.clear(std::memory_order_release);
    return processed;
}

void IsochReceiveContext::ResetReplayEpochForDiscontinuity() noexcept {
    auto* control = directInputView_.control;
    if (!control || !replayResetForStart_) {
        replayCycleInitialized_ = false;
        return;
    }

    const bool wasEstablished =
        control->rxSequenceReplay.IsEstablished();
    control->rxSytCadence.Reset();
    control->rxSequenceReplay.Reset();
    control->rxReplayEpochResets.fetch_add(
        1, std::memory_order_relaxed);
    replayReadyNotified_ = false;
    rxCadenceEstablishedLogged_ = false;
    replayCycleInitialized_ = false;
    dbcInitialized_ = false;

    if (wasEstablished && timingLossCallback_) {
        timingLossCallback_();
    }
}

void IsochReceiveContext::SetDirectAudioBindingSource(ASFW::Audio::Runtime::IDirectAudioBindingSource* source) noexcept {
    directAudioBindingSource_ = source;
    lastDirectAudioGeneration_ = 0;
    payloadWriterTelemetryAggregator_.Reset();
}



void IsochReceiveContext::SetTimingLossCallback(TimingLossCallback callback) noexcept {
    timingLossCallback_ = std::move(callback);
}

void IsochReceiveContext::SetZtsAnchorReadyCallback(
    ZtsAnchorReadyCallback callback) noexcept {
    ztsAnchorReadyCallback_ = std::move(callback);
}

void IsochReceiveContext::SetReplayReadyCallback(
    ReplayReadyCallback callback) noexcept {
    replayReadyCallback_ = std::move(callback);
}

bool IsochReceiveContext::IsReplayEstablished() const noexcept {
    // FW-62: this runs on the audio queue while .control is written/nulled on the RX
    // queue (Poll, under rxLock_). Snapshot the pointer once — reading it twice risks a
    // null-TOCTOU (check passes, deref nulls). NOTE: this does not fix use-after-free of
    // a non-null-but-freed control block; that lifetime ownership is FW-63.
    const auto* control = directInputView_.control;
    return control != nullptr && control->rxSequenceReplay.IsEstablished();
}

void IsochReceiveContext::SetCallback(IsochReceiveCallback callback) {
    callback_ = callback;
}

void IsochReceiveContext::LogHardwareState() {
}

void IsochReceiveContext::DrainZtsTelemetry(uint32_t maxRecords) {
    constexpr uint64_t kZtsPeriodFrames =
        ASFW::IsochTransport::AudioTimingGeometry::kHalZeroTimestampPeriodFrames;
    const uint32_t rate = directInputView_.sampleRateHz;

    const uint64_t dropped = ztsTelemetry_.Drain(
        maxRecords, [&](const Rx::ZtsTelemetryRecord& rec) {
            if (!ztsTelemetryLogGate_.ShouldEmit(rec, rate)) {
                return;
            }

            // Three clocks side by side so drift between them is visible:
            //   host monotonic (host = published grid value, rawHost =
            //     back-corrected packet receive, drainHost = batch-drain read),
            //   FireWire bus cycle timer (drainCycle = host OHCI read,
            //     rxCycle = RX DMA descriptor), and the sample-frame grid;
            //   plus the recovered device-SYT cadence (sytLead/rollCad/recPhase).
            ASFW_LOG(
                Zts,
                "%{public}s count=%llu frame=%llu host=%llu rawHost=%llu "
                "drainHost=%llu drainCycle=0x%08x rxCycle=0x%08x age=%lld "
                "rawRxTs=0x%04x syt=0x%04x sytLead=%lld rollCad=%u recPhase=%lld "
                "desc=%u dec=%u period=%llu rate=%u rateQ8=%u",
                rec.kind == static_cast<uint8_t>(Rx::ZtsEventKind::kSeed)
                    ? "SEED"
                    : "UPD",
                rec.publishCount,
                rec.sampleFrame,
                rec.hostTicks,
                rec.rawHostTicks,
                rec.drainHostTicks,
                rec.drainCycleTimer,
                rec.rxCycleTimer,
                rec.ageTicks,
                rec.rawRxTs,
                rec.syt,
                rec.sytLeadTicks,
                rec.rollingCadenceTicks,
                rec.recoveredPhaseTicks,
                rec.descriptorIndex,
                rec.framesDecoded,
                kZtsPeriodFrames,
                rate,
                rec.hostNanosPerSampleQ8);

            // Sampled-anchor clock comparison: measure actual device frames
            // across the multi-second log interval and compare against nominal
            // rate. This smooths 32 ms quantization jitter while preserving the
            // long-term device-vs-host drift signal.
            if (prevLoggedAnchorValid_ && rate != 0 &&
                rate == prevLoggedAnchorRate_) {
                const int64_t frameDelta =
                    static_cast<int64_t>(rec.sampleFrame) -
                    static_cast<int64_t>(prevLoggedAnchorFrame_);
                if (frameDelta > 0) {
                    // Host time delta: convert mach ticks to nanoseconds.
                    const uint64_t hostDeltaTicks =
                        rec.hostTicks - prevLoggedAnchorHostTicks_;
                    const uint64_t hostDeltaNs =
                        ASFW::Timing::hostTicksToNanos(hostDeltaTicks);

                    // Measured nanoseconds per frame (Q32 fixed-point).
                    const int64_t measuredNsPerFrameQ32 =
                        static_cast<int64_t>(
                            (static_cast<__uint128_t>(hostDeltaNs) << 32) /
                            static_cast<__uint128_t>(frameDelta));

                    // Nominal nanoseconds per frame (Q32 fixed-point).
                    const int64_t nominalNsPerFrameQ32 =
                        static_cast<int64_t>(
                            (static_cast<__uint128_t>(1'000'000'000ULL) << 32) /
                            static_cast<__uint128_t>(rate));

                    // PPM deviation (Q16 fixed-point). The measured-minus-
                    // nominal difference is SIGNED: a slow device makes it
                    // negative. The whole expression must therefore stay in
                    // signed 128-bit arithmetic — the previous __uint128_t cast
                    // underflowed a negative drift to ~7.9e18.
                    const int64_t ppmQ16 =
                        (nominalNsPerFrameQ32 != 0)
                            ? static_cast<int64_t>(
                                  (static_cast<__int128_t>(
                                       measuredNsPerFrameQ32 -
                                       nominalNsPerFrameQ32) *
                                   static_cast<__int128_t>(1'000'000) *
                                   (__int128_t{1} << 16)) /
                                  static_cast<__int128_t>(nominalNsPerFrameQ32))
                            : 0;
                    // Human-readable integer ppm (the log used to print the raw
                    // Q16 value under a "ppm" label, ~65536x too large).
                    const int64_t ppm = ppmQ16 / (1 << 16);

                    // Frame residual: how far nominal interpolation misses.
                    const int64_t nominalExpectedFrame =
                        static_cast<int64_t>(prevLoggedAnchorFrame_) +
                        static_cast<int64_t>(
                            (hostDeltaNs * static_cast<uint64_t>(rate)) /
                            1'000'000'000ULL);
                    const int64_t frameResidual =
                        static_cast<int64_t>(rec.sampleFrame) -
                        nominalExpectedFrame;

                    ASFW_LOG(
                        Zts,
                        "CLKDELTA hostDeltaNs=%llu frameDelta=%lld "
                        "measuredNsQ32=%lld nominalNsQ32=%lld "
                        "ppm=%lld ppmQ16=%lld residual=%lld",
                        hostDeltaNs,
                        frameDelta,
                        measuredNsPerFrameQ32,
                        nominalNsPerFrameQ32,
                        ppm,
                        ppmQ16,
                        frameResidual);
                }
            }
            prevLoggedAnchorFrame_ = rec.sampleFrame;
            prevLoggedAnchorHostTicks_ = rec.hostTicks;
            prevLoggedAnchorRate_ = rate;
            prevLoggedAnchorValid_ = true;
        });

    if (dropped != 0) {
        ASFW_LOG(Zts,
                 "drain overflow: dropped=%llu (capacity=%u — raise watchdog "
                 "drain cadence or maxRecords)",
                 dropped, Rx::ZtsTelemetryRing::kCapacity);
    }
}

void IsochReceiveContext::DrainPayloadWriterTelemetry() {
    auto* control = directInputView_.control;
    if (control == nullptr) {
        return;
    }

    const uint64_t errorGeneration =
        control->ioCallbackErrorGeneration.load(std::memory_order_acquire);
    const uint64_t reportedGeneration =
        control->ioCallbackErrorReportedGeneration.load(
            std::memory_order_relaxed);
    if (errorGeneration != reportedGeneration) {
        ASFW_LOG(
            DirectAudio,
            "[AudioIO] callback returned kr=0x%08x operation=%u objectId=%u "
            "frameCount=%u sampleTime=%llu hostTime=%llu errors=%llu",
            control->ioLastError.load(std::memory_order_relaxed),
            control->ioLastErrorOperation.load(std::memory_order_relaxed),
            control->ioLastErrorObjectId.load(std::memory_order_relaxed),
            control->ioLastErrorFrameCount.load(std::memory_order_relaxed),
            control->ioLastErrorSampleTime.load(std::memory_order_relaxed),
            control->ioLastErrorHostTime.load(std::memory_order_relaxed),
            errorGeneration);
        control->ioCallbackErrorReportedGeneration.store(
            errorGeneration, std::memory_order_release);
    }

    // Basic TX flow is confirmed (under-exposure / Defect B closed, see tag
    // tx-frame-exposure-lead). Keep draining the ring every watchdog pass so
    // it never overflows, but only emit an aggregate when a counter advances
    // anomalously. The callback path itself only records into the ring.
    payloadWriterTelemetryAggregator_.BeginDrain();
    ASFW::Audio::Runtime::PayloadWriterTelemetryRecord lastRecord{};
    bool haveLastRecord = false;
    const uint64_t dropped = control->payloadWriterTelemetry.Drain(
        [this, &lastRecord, &haveLastRecord](
            const ASFW::Audio::Runtime::PayloadWriterTelemetryRecord& r) {
            payloadWriterTelemetryAggregator_.Observe(r);
            lastRecord = r;
            haveLastRecord = true;
        });

    const auto& summary = payloadWriterTelemetryAggregator_.Summary();
    if (haveLastRecord && summary.HasAnomaly()) {
        ASFW_LOG(
            DirectAudio,
            "[PayloadWriter] anomaly lastSample=%llu completion=%llu deficitMax=%llu "
            "visitedDelta=%llu writtenDelta=%llu withoutPktDelta=%llu outsidePktDelta=%llu "
            "racedDelta=%llu transmittedDelta=%llu underExpCallsDelta=%llu underExpFramesDelta=%llu",
            lastRecord.sampleTime,
            lastRecord.completionCursor,
            summary.maxExposureDeficitFrames,
            summary.visitedDelta,
            summary.writtenDelta,
            summary.withoutPacketDelta,
            summary.outsidePacketDelta,
            summary.racedReuseDelta,
            summary.wroteIntoTransmittedDelta,
            summary.underExposureCallsDelta,
            summary.underExposureFramesDelta);
    }

    if (dropped != 0) {
        ASFW_LOG(DirectAudio,
                 "[PayloadWriter] drain overflow: dropped=%llu (capacity=%u)",
                 dropped, ASFW::Audio::Runtime::PayloadWriterTelemetryRing::kCapacity);
    }
}

void IsochReceiveContext::LogTxSytTrace() {
    auto* control = directInputView_.control;
    if (control == nullptr) {
        return;
    }

    ASFW::Audio::Runtime::TxSytTraceSample sample{};
    uint64_t decisions = 0;
    if (!control->txSytTrace.ReadLatest(sample, decisions)) {
        return;
    }

    // tx = rx + delay, re-anchored to our transmit cycle: the txSyt offset
    // within its cycle equals (sytOffDelayFree + txDelay) % 3072, and its cycle
    // nibble is outCyc + carry. Surfaced so the replay phase is observed, not
    // inferred.
    ASFW_LOG(
        TxSyt,
        "obsCyc=%u rxSyt=0x%04x sytOffDelayFree=%u +txDelay=%u outCyc=%u "
        "=> txSyt=0x%04x (cyc=%u off=0x%03x) pkt=%llu decisions=%llu",
        sample.sourceCycle,
        sample.observedRxSyt,
        sample.sytOffsetDelayFree,
        sample.txDelayTicks,
        sample.outCycle,
        sample.txSyt,
        (static_cast<uint32_t>(sample.txSyt) >> 12) & 0x0Fu,
        static_cast<uint32_t>(sample.txSyt) & 0x0FFFu,
        sample.packetIndex,
        decisions);
}

} // namespace ASFW::Isoch
