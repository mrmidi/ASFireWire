#include "IsochReceiveContext.hpp"
#include "../Core/IsochEventGroup.hpp"
#include "../../Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"

#include "../../Common/TimingUtils.hpp"
#include "../../Common/DriverKitUtils.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Hardware/RegisterMap.hpp"
#include "../../Diagnostics/Signposts.hpp"

#include <utility>

namespace ASFW::Isoch {

// ============================================================================
// Factory
// ============================================================================

OSSharedPtr<IsochReceiveContext> IsochReceiveContext::Create(::ASFW::Driver::HardwareInterface* hw,
                                                            std::shared_ptr<::ASFW::Isoch::Memory::IIsochDMAMemory> dmaMemory) {
    auto ctx = ASFW::Common::MakeOSObject<IsochReceiveContext>();
    if (!ctx) return nullptr;

    ctx->hardware_ = hw;
    ctx->dmaMemory_ = std::move(dmaMemory);

    if (!ctx->init()) return nullptr;  // OSSharedPtr destructor calls release()

    return ctx;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool IsochReceiveContext::init() {
    if (!OSObject::init()) {
        return false;
    }
    return true;
}

void IsochReceiveContext::free() {
    Stop();
    OSObject::free();
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
                                            uint32_t am824Slots) {
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
    ztsProjector_.Reset(0);
    cursorInitialized_ = false;
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
    (void)ASFW::Timing::initializeHostTimebase();

    rxLock_.clear(std::memory_order_release);
    return kIOReturnSuccess;
}

void IsochReceiveContext::Stop() {
    while (rxLock_.test_and_set(std::memory_order_acquire)) {
    }

    if (GetState() == IRPolicy::State::Stopped) {
        rxLock_.clear(std::memory_order_release);
        return;
    }

    hardware_->Write(registers_.ContextControlClear, Driver::ContextControl::kRun);

    const uint32_t contextMask = 1u << contextIndex_;
    hardware_->Write(ASFW::Driver::Register32::kIsoRecvIntMaskClear, contextMask);
    ASFW_LOG(Isoch, "Stop: Disabled IR interrupt for context %u", contextIndex_);

    Transition(IRPolicy::State::Stopped, 0, "Stop");

    rxLock_.clear(std::memory_order_release);
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
                    directInputView_.memory.storage = ASFW::Audio::Runtime::AudioSampleStorage::kInt32Native;
                    directInputView_.control = snapshot.control;
                    directInputView_.deviceToHostAm824Slots = am824Slots_ > 0 ? am824Slots_ : snapshot.inputChannels;
                    directInputView_.hostToDeviceAm824Slots = snapshot.outputChannels;
                    directInputView_.streamMode = ASFW::Audio::Runtime::AudioStreamMode::kUnknown;
                    directInputView_.hostToDeviceWireFormat = ASFW::Audio::Runtime::AudioWireFormat::kAM824;

                    if (!replayResetForStart_ &&
                        directInputView_.control) {
                        directInputView_.control->rxSytCadence.Reset();
                        directInputView_.control->rxSequenceReplay.Reset();
                        directInputView_.control->rxReplayEpochResets.fetch_add(
                            1, std::memory_order_relaxed);
                        replayResetForStart_ = true;
                    }

                    // Data plane (RX -> input buffer) and the controller-side clock
                    // publisher both arm. The clock publisher writes the shared
                    // timeline; the ADK side mirrors that timeline to HAL.
                    directInputWriter_.Bind(&directInputView_);
                    clockPublisher_.Bind(&directInputView_);
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
                }
                lastDirectAudioGeneration_ = snapshot.generation;
            }
        } else {
            if (lastDirectAudioGeneration_ != 0) {
                ASFW_LOG(Isoch, "IR: direct audio binding cleared/unavailable. Disarming.");
                directInputWriter_.Unbind();
                clockPublisher_.Unbind();
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
            const uint32_t channels = directInputView_.memory.inputChannels;
            const uint32_t slots = directInputView_.deviceToHostAm824Slots;
            const auto result = directProcessor_.ProcessPacket(pkt.payload,
                                                               pkt.actualLength,
                                                               absoluteFrameCursor_,
                                                               channels,
                                                               slots,
                                                               wireFormat_);
            const bool packetAccepted =
                result.status ==
                    AudioEngine::Direct::Rx::
                        DirectRxWriteStatus::kAvailable ||
                result.status ==
                    AudioEngine::Direct::Rx::
                        DirectRxWriteStatus::kInvalidBinding;
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
                        absoluteFrameCursor_;
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
                ztsProjector_.Advance(
                    result.framesDecoded,
                    [&](uint64_t gridFrame,
                        uint32_t framesFromPacketStart) {
                        if (!rxClockEstablished ||
                            packetReceiveHostTicks == 0 ||
                            !clockPublisher_.IsBound() ||
                            hostNanosPerSampleQ8 == 0) {
                            return;
                        }

                        const uint64_t gridDeltaNanos =
                            (static_cast<uint64_t>(
                                 framesFromPacketStart) *
                             hostNanosPerSampleQ8) >>
                            8;
                        const uint64_t gridHostTicks =
                            packetReceiveHostTicks +
                            ASFW::Timing::nanosToHostTicks(
                                gridDeltaNanos);

                        const auto publishResult =
                            clockPublisher_.Publish(
                                gridFrame,
                                gridHostTicks,
                                hostNanosPerSampleQ8);
                        if (!publishResult.accepted) {
                            ResetReplayEpochForDiscontinuity();
                            return;
                        }

                        ++rxZtsPublishCount_;
                        if (publishResult.notifyConsumer &&
                            ztsAnchorReadyCallback_) {
                            ztsAnchorReadyCallback_(
                                publishResult
                                    .notificationGeneration);
                        }

                        Rx::ZtsTelemetryRecord rec{};
                        rec.publishCount = rxZtsPublishCount_;
                        rec.sampleFrame = gridFrame;
                        rec.hostTicks = gridHostTicks;
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
                    });
                absoluteFrameCursor_ =
                    ztsProjector_.AbsoluteFrame();
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
    return directInputView_.control &&
           directInputView_.control->rxSequenceReplay.IsEstablished();
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

            // Consecutive-anchor clock comparison: measure actual device frames
            // per host second and compare against nominal 48 kHz. This reveals
            // whether the ZTS anchors are host-clock-disciplined or device-derived.
            if (prevAnchorValid_ && rate != 0) {
                const int64_t frameDelta =
                    static_cast<int64_t>(rec.sampleFrame) -
                    static_cast<int64_t>(prevAnchorFrame_);
                if (frameDelta > 0) {
                    // Host time delta: convert mach ticks to nanoseconds.
                    const uint64_t hostDeltaTicks =
                        rec.hostTicks - prevAnchorHostTicks_;
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
                        static_cast<int64_t>(prevAnchorFrame_) +
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
            prevAnchorFrame_ = rec.sampleFrame;
            prevAnchorHostTicks_ = rec.hostTicks;
            prevAnchorValid_ = true;
        });

    if (dropped != 0) {
        ASFW_LOG(Zts,
                 "drain overflow: dropped=%llu (capacity=%u — raise watchdog "
                 "drain cadence or maxRecords)",
                 dropped, Rx::ZtsTelemetryRing::kCapacity);
    }
}

void IsochReceiveContext::DrainTxSytTelemetry(uint32_t maxRecords) {
    auto* control = directInputView_.control;
    if (control == nullptr) {
        return;
    }

    const uint64_t dropped = control->txSytTelemetry.Drain(
        maxRecords, [](const ASFW::Audio::Runtime::TxSytTelemetryRecord& r) {
            // Full servo operand set per data-packet decision. flags bits:
            // 0x1 seeded, 0x2 forceAdjust, 0x4 reseeded, 0x8 committed(data).
            ASFW_LOG(
                TxSyt,
                "pkt=%llu flags=0x%02x health=%u anchor=%lld phasePre=%lld "
                "phasePost=%lld recPhase=%lld rxFree=%lld pErr=%lld fErr=%lld "
                "corr=%lld lead=%lld wire=%lld rollCad=%u pend=%u ridx=%u syt=0x%04x "
                "tgtZts=%lld tgtDev=%lld tgtDiff=%lld",
                r.packetIndex,
                static_cast<unsigned>(r.flags),
                static_cast<unsigned>(r.health),
                r.packetAnchorTicks,
                r.phaseTicksPre,
                r.phaseTicksPost,
                r.recoveredPhaseTicks,
                r.rxPhaseDelayFree,
                r.phaseError,
                r.frameError,
                r.correctionTicks,
                r.leadTicks,
                r.wireLeadTicks,
                r.rollingCadenceTicks,
                static_cast<unsigned>(r.pendingCadenceTicks),
                static_cast<unsigned>(r.cadenceReadIndex),
                static_cast<unsigned>(r.syt),
                r.targetFromZts,
                r.targetFromDevice,
                r.targetFrameDiff);
        });

    if (dropped != 0) {
        ASFW_LOG(TxSyt,
                 "drain overflow: dropped=%llu (capacity=%u)",
                 dropped, ASFW::Audio::Runtime::TxSytTelemetryRing::kCapacity);
    }
}

void IsochReceiveContext::DrainPayloadWriterTelemetry(uint32_t maxRecords) {
    auto* control = directInputView_.control;
    if (control == nullptr) {
        return;
    }

    const uint64_t dropped = control->payloadWriterTelemetry.Drain(
        maxRecords, [](const ASFW::Audio::Runtime::PayloadWriterTelemetryRecord& r) {
            const uint32_t cipQ0 = (static_cast<uint32_t>(r.lastReadPacketBytes[0]) << 24) |
                                   (static_cast<uint32_t>(r.lastReadPacketBytes[1]) << 16) |
                                   (static_cast<uint32_t>(r.lastReadPacketBytes[2]) << 8) |
                                   static_cast<uint32_t>(r.lastReadPacketBytes[3]);
            const uint32_t cipQ1 = (static_cast<uint32_t>(r.lastReadPacketBytes[4]) << 24) |
                                   (static_cast<uint32_t>(r.lastReadPacketBytes[5]) << 16) |
                                   (static_cast<uint32_t>(r.lastReadPacketBytes[6]) << 8) |
                                   static_cast<uint32_t>(r.lastReadPacketBytes[7]);
            const uint32_t payQ0 = (static_cast<uint32_t>(r.lastReadPacketBytes[8]) << 24) |
                                   (static_cast<uint32_t>(r.lastReadPacketBytes[9]) << 16) |
                                   (static_cast<uint32_t>(r.lastReadPacketBytes[10]) << 8) |
                                   static_cast<uint32_t>(r.lastReadPacketBytes[11]);
            const uint32_t payQ1 = (static_cast<uint32_t>(r.lastReadPacketBytes[12]) << 24) |
                                   (static_cast<uint32_t>(r.lastReadPacketBytes[13]) << 16) |
                                   (static_cast<uint32_t>(r.lastReadPacketBytes[14]) << 8) |
                                   static_cast<uint32_t>(r.lastReadPacketBytes[15]);

            ASFW_LOG(
                DirectAudio,
                "[PayloadWriter] sampleTime=%llu frameCount=%u frameCapacity=%u completion=%llu exposedEnd=%llu "
                "visited=%llu written=%llu withoutPkt=%llu outsidePkt=%llu raced=%llu legacyTrans=%llu nonZero=%llu maxAbs=%f "
                "pbRead=%llu pbWrite=%llu outBase=0x%llx capRead=%llu capWrite=%llu inBase=0x%llx "
                "lastReadPkt=%llu cip=%08x %08x pay=%08x %08x",
                r.sampleTime,
                r.frameCount,
                r.frameCapacity,
                r.completionCursor,
                r.exposedFrameEnd,
                r.visited,
                r.written,
                r.withoutPacket,
                r.outsidePacket,
                r.racedReuse,
                r.wroteIntoTransmitted,
                r.nonZeroFrames,
                r.maxAbsSample,
                r.playbackRingReadFrame,
                r.playbackRingWriteFrame,
                r.outputBaseAddr,
                r.captureRingReadFrame,
                r.captureRingWriteFrame,
                r.inputBaseAddr,
                r.lastReadPacketIndex,
                cipQ0,
                cipQ1,
                payQ0,
                payQ1);
        });

    if (dropped != 0) {
        ASFW_LOG(DirectAudio,
                 "[PayloadWriter] drain overflow: dropped=%llu (capacity=%u)",
                 dropped, ASFW::Audio::Runtime::PayloadWriterTelemetryRing::kCapacity);
    }
}

} // namespace ASFW::Isoch
