#include "IsochReceiveContext.hpp"
#include "../../Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"

#include "../../AudioWire/AMDTP/TimingUtils.hpp"
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
    (void)ASFW::Timing::initializeHostTimebase();
    rxCycleHostTicks_ = ASFW::Timing::nanosToHostTicks(ASFW::Timing::kNanosPerCycle);

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
            // Re-evaluate on a generation bump OR when the IOUserAudioDevice pointer
            // itself transitions (e.g. null -> non-null when the provider finally
            // publishes the device). The latter guards against latching a device-less
            // binding and never re-arming the clock leg.
            const bool bindingChanged =
                snapshot.generation != lastDirectAudioGeneration_ ||
                snapshot.audioDevice != directInputView_.audioDevice;
            if (bindingChanged) {
                if (snapshot.valid && snapshot.HasInput()) {
                    ASFW_LOG(Isoch,
                             "IR: direct audio binding changed (gen %llu -> %llu). Arming direct Rx inBase=%p inFrames=%u inCh=%u outBase=%p outFrames=%u outCh=%u control=%p audioDevice=%p rate=%u",
                             lastDirectAudioGeneration_,
                             snapshot.generation,
                             static_cast<void*>(snapshot.inputBase),
                             snapshot.inputFrames,
                             snapshot.inputChannels,
                             static_cast<const void*>(snapshot.outputBase),
                             snapshot.outputFrames,
                             snapshot.outputChannels,
                             static_cast<void*>(snapshot.control),
                             static_cast<void*>(snapshot.audioDevice),
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
                    directInputView_.audioDevice = snapshot.audioDevice;

                    // Data plane (RX -> input buffer) and the controller-side clock
                    // publisher both arm. When AudioDriverKit owns the IOUserAudioDevice
                    // in another process, the clock publisher writes the shared timeline;
                    // the ADK side mirrors that timeline to UpdateCurrentZeroTimestamp.
                    directInputWriter_.Bind(&directInputView_);
                    clockPublisher_.Bind(&directInputView_);
                    if (snapshot.audioDevice == nullptr) {
                        ASFW_LOG(Isoch,
                                 "IR: direct audio binding has NULL audioDevice (gen %llu); publishing clock to shared control block for ADK-side HAL mirror. control=%p inBase=%p rate=%u",
                                 snapshot.generation,
                                 static_cast<void*>(snapshot.control),
                                 static_cast<void*>(snapshot.inputBase),
                                 snapshot.sampleRateHz);
                    }
                } else {
                    ASFW_LOG(Isoch,
                             "IR: direct audio binding invalid or has no input (gen %llu -> %llu). Disarming valid=%d hasIn=%d control=%p audioDevice=%p rate=%u",
                             lastDirectAudioGeneration_,
                             snapshot.generation,
                             snapshot.valid,
                             snapshot.HasInput(),
                             static_cast<void*>(snapshot.control),
                             static_cast<void*>(snapshot.audioDevice),
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

    const uint64_t drainHostTicks = mach_absolute_time();
    uint64_t cycleHostTicks = rxCycleHostTicks_;
    if (cycleHostTicks == 0 && ASFW::Timing::initializeHostTimebase()) {
        cycleHostTicks = ASFW::Timing::nanosToHostTicks(ASFW::Timing::kNanosPerCycle);
        rxCycleHostTicks_ = cycleHostTicks;
    }

    const uint32_t processed = rxRing_.DrainCompleted(*dmaMemory_, [this, drainHostTicks, cycleHostTicks](const Rx::IsochRxDmaRing::CompletedPacket& pkt) {
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

                constexpr uint32_t kRxTimingGroupPackets = 12;
                const uint8_t packetInGroup =
                    static_cast<uint8_t>(pkt.descriptorIndex % kRxTimingGroupPackets);
                const uint64_t packetBackTicks =
                    cycleHostTicks * static_cast<uint64_t>((kRxTimingGroupPackets - 1u) - packetInGroup);
                const uint64_t packetHostTicks =
                    (drainHostTicks > packetBackTicks) ? (drainHostTicks - packetBackTicks) : drainHostTicks;
                bool rxClockEstablished = false;
                if (externalSyncBridge_ && result.hasValidCip) {
                    uint32_t updateSeq = 0;
                    const bool establishTransition = externalSyncClockState_.ObserveSample(
                        *externalSyncBridge_,
                        packetHostTicks,
                        result.syt,
                        result.fdf,
                        result.dbs,
                        &updateSeq
                    );
                    if (establishTransition) {
                        ASFW_LOG(Isoch, "IR SYT CLOCK ESTABLISHED syt=0x%04x fdf=0x%02x dbs=%u seq=%u",
                                 result.syt, result.fdf, result.dbs, updateSeq);
                        externalSyncBridge_->clockEstablished.store(true, std::memory_order_release);
                        externalSyncBridge_->startupQualified.store(true, std::memory_order_release);
                    }
                    rxClockEstablished =
                        externalSyncBridge_->clockEstablished.load(std::memory_order_acquire);
                }

                const uint64_t nextFrameCursor = absoluteFrameCursor_ + result.framesDecoded;
                if (result.hasValidCip &&
                    result.syt != Core::ExternalSyncBridge::kNoInfoSyt &&
                    result.framesDecoded > 0 &&
                    directInputView_.sampleRateHz != 0 &&
                    clockPublisher_.IsBound() &&
                    rxClockEstablished) {
                    const uint32_t hostNanosPerSampleQ8 =
                        static_cast<uint32_t>((1000000000ULL << 8) / directInputView_.sampleRateHz);
                    clockPublisher_.Publish(nextFrameCursor, packetHostTicks, hostNanosPerSampleQ8);
                    if (externalSyncBridge_) {
                        externalSyncBridge_->PublishTransportTiming(nextFrameCursor,
                                                                    packetHostTicks,
                                                                    hostNanosPerSampleQ8);
                    }

                    ++rxZtsPublishCount_;
                    if (rxZtsPublishCount_ <= 8 || (rxZtsPublishCount_ % 1024) == 0) {
                        ASFW_LOG(Isoch,
                                 "ZTS publish source=rx count=%llu frame=%llu host=%llu syt=0x%04x desc=%u packetInGroup=%u clockEstablished=%d bound=%d",
                                 rxZtsPublishCount_,
                                 nextFrameCursor,
                                 packetHostTicks,
                                 result.syt,
                                 pkt.descriptorIndex,
                                 packetInGroup,
                                 rxClockEstablished,
                                 clockPublisher_.IsBound());
                    }
                }

                absoluteFrameCursor_ = nextFrameCursor;
            }
        }

        if (callback_) {
            const auto span = std::span<const uint8_t>(pkt.payload, pkt.actualLength);
            callback_(span, static_cast<uint32_t>(pkt.xferStatus), 0);
        }
    });

    rxLock_.clear(std::memory_order_release);
    return processed;
}

void IsochReceiveContext::SetDirectAudioBindingSource(ASFW::Audio::Runtime::IDirectAudioBindingSource* source) noexcept {
    directAudioBindingSource_ = source;
    lastDirectAudioGeneration_ = 0;
}

void IsochReceiveContext::SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept {
    externalSyncBridge_ = bridge;
}

void IsochReceiveContext::SetTimingLossCallback(TimingLossCallback callback) noexcept {
    timingLossCallback_ = std::move(callback);
}

void IsochReceiveContext::SetCallback(IsochReceiveCallback callback) {
    callback_ = callback;
}

void IsochReceiveContext::LogHardwareState() {
}

} // namespace ASFW::Isoch
