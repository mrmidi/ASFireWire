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
    cursorInitialized_ = false;
    rxZtsPublishCount_ = 0;
    rxTimestampValidCount_ = 0;
    rxTimestampInvalidCount_ = 0;
    rxCadenceEstablishedLogged_ = false;
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
            if (result.status == AudioEngine::Direct::Rx::DirectRxWriteStatus::kAvailable ||
                result.status == AudioEngine::Direct::Rx::DirectRxWriteStatus::kInvalidBinding) {
                
                if (!cursorInitialized_ && result.hasValidCip) {
                    // First valid CIP packet anchors the RX device cursor (starts at 0).
                    // We do not load or reset to inputProducedEndFrame on late binding to keep the timeline monotonic.
                    cursorInitialized_ = true;
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

                bool rxClockEstablished = false;
                int64_t sytLeadTicks = 0;
                uint64_t packetPresentationHostTicks = 0;
                if (result.hasValidCip &&
                    result.syt != 0xFFFF &&
                    hasHardwareTimestamp &&
                    directInputView_.control) {
                    const bool cadenceAccepted =
                        directInputView_.control->rxSytCadence.Observe(
                            result.syt, rxTimestamp.cycleTimer);
                    ASFW::Driver::RxSytCadence::Snapshot cadence{};
                    if (cadenceAccepted &&
                        directInputView_.control->rxSytCadence.TrySnapshot(
                            cadence)) {
                        rxClockEstablished = cadence.established;
                        if (cadence.established &&
                            !rxCadenceEstablishedLogged_) {
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

                        const int64_t receiveTicks =
                            ASFW::Timing::encodedTstampToOffsets(
                                rxTimestamp.cycleTimer);
                        const int64_t presentationTicks =
                            ASFW::Timing::extendTstampFromCycleTimer(
                                rxTimestamp.cycleTimer, result.syt);
                        sytLeadTicks = ASFW::Timing::extOffsetDiff(
                            presentationTicks, receiveTicks);
                        if (sytLeadTicks >= 0) {
                            packetPresentationHostTicks =
                                packetReceiveHostTicks +
                                ASFW::Timing::nanosToHostTicks(
                                    Rx::FireWireTicksToNanos(
                                        static_cast<uint64_t>(
                                            sytLeadTicks)));
                        } else {
                            rxClockEstablished = false;
                        }
                    }
                }

                const uint64_t packetFirstFrame = absoluteFrameCursor_;
                const uint64_t nextFrameCursor =
                    packetFirstFrame + result.framesDecoded;

                constexpr uint64_t kZtsPeriodFrames =
                    ASFW::IsochTransport::AudioTimingGeometry::
                        kHalZeroTimestampPeriodFrames;
                if (rxClockEstablished &&
                    packetPresentationHostTicks != 0 &&
                    result.framesDecoded != 0 &&
                    clockPublisher_.IsBound() &&
                    directInputView_.sampleRateHz != 0) {
                    const uint32_t hostNanosPerSampleQ8 =
                        static_cast<uint32_t>(
                            (1000000000ULL << 8) /
                            directInputView_.sampleRateHz);
                    uint64_t gridFrame =
                        ((packetFirstFrame / kZtsPeriodFrames) + 1u) *
                        kZtsPeriodFrames;
                    while (gridFrame <= nextFrameCursor) {
                        const uint64_t framesFromPacketStart =
                            gridFrame - packetFirstFrame;
                        const uint64_t gridDeltaNanos =
                            (framesFromPacketStart *
                             static_cast<uint64_t>(
                                 hostNanosPerSampleQ8)) >>
                            8;
                        const uint64_t gridHostTicks =
                            packetPresentationHostTicks +
                            ASFW::Timing::nanosToHostTicks(
                                gridDeltaNanos);

                        const auto publishResult =
                            clockPublisher_.Publish(
                                gridFrame,
                                gridHostTicks,
                                hostNanosPerSampleQ8);
                        if (publishResult.accepted) {
                            ++rxZtsPublishCount_;
                            if (publishResult.notifyConsumer &&
                                ztsAnchorReadyCallback_) {
                                ztsAnchorReadyCallback_(
                                    publishResult
                                        .notificationGeneration);
                            }
                            if (rxZtsPublishCount_ <= 8 ||
                                (rxZtsPublishCount_ % 128) == 0) {
                                ASFW_LOG(
                                    Isoch,
                                    "ZTS publish grid count=%llu frame=%llu host=%llu period=%llu rate=%u",
                                    rxZtsPublishCount_,
                                    gridFrame,
                                    gridHostTicks,
                                    kZtsPeriodFrames,
                                    directInputView_.sampleRateHz);
                                ASFW_LOG(
                                    Isoch,
                                    "ZTS source desc=%u rawRxTs=0x%04x rxCycle=0x%08x drainCycle=0x%08x ageTicks=%lld syt=0x%04x sytLeadTicks=%lld",
                                    pkt.descriptorIndex,
                                    result.receiveCycleTimestamp,
                                    rxTimestamp.cycleTimer,
                                    drainCycleTimer,
                                    rxTimestamp.ageTicks,
                                    result.syt,
                                    sytLeadTicks);
                            }
                        }
                        gridFrame += kZtsPeriodFrames;
                    }
                }
                absoluteFrameCursor_ = nextFrameCursor;
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

void IsochReceiveContext::SetCallback(IsochReceiveCallback callback) {
    callback_ = callback;
}

void IsochReceiveContext::LogHardwareState() {
}

} // namespace ASFW::Isoch
