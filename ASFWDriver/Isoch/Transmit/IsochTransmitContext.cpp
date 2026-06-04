// IsochTransmitContext.cpp
// ASFW - Isochronous Transmit Context (orchestrator)
//
// NOTE:
// OHCI IT programming details are in Tx::IsochTxDmaRing.
// Audio semantics (CIP/AM824 + direct ADK memory mapping) are in IsochAudioTxPipeline.
//

#include "IsochTransmitContext.hpp"
#include "../../Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"

#include "../../AudioWire/AMDTP/TimingUtils.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Logging/LogConfig.hpp"

#include <cstring>

namespace ASFW::Isoch {

using namespace ASFW::Driver;

namespace {

const char* TxStateName(ITState state) noexcept {
    switch (state) {
    case ITState::Unconfigured:
        return "unconfigured";
    case ITState::Configured:
        return "configured";
    case ITState::Running:
        return "running";
    case ITState::Stopped:
        return "stopped";
    }
    return "unknown";
}

} // namespace

std::unique_ptr<IsochTransmitContext> IsochTransmitContext::Create(
    Driver::HardwareInterface* hw,
    std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory) noexcept {

    auto ctx = std::make_unique<IsochTransmitContext>();
    if (!ctx) return nullptr;

    ctx->hardware_ = hw;
    ctx->dmaMemory_ = std::move(dmaMemory);

    ctx->verifier_.BindRecovery(&ctx->recovery_);

    return ctx;
}

IsochTransmitContext::~IsochTransmitContext() noexcept {
    verifier_.Shutdown();
}

void IsochTransmitContext::SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept {
    audio_.SetExternalSyncBridge(bridge);
}

void IsochTransmitContext::SetRecoveryCallback(RecoveryCallback callback) noexcept {
    recoveryCallback_ = std::move(callback);
}

void IsochTransmitContext::SetDirectTxRuntimeBinding(
    const IsochAudioTxPipeline::DirectTxRuntimeBinding& binding) noexcept {
    audio_.SetDirectTxRuntimeBinding(binding);
}

void IsochTransmitContext::SetDirectAudioBindingSource(ASFW::Audio::Runtime::IDirectAudioBindingSource* source) noexcept {
    directAudioBindingSource_ = source;
    lastDirectAudioGeneration_ = 0;
    ASFW_LOG(Isoch, "IT DBG BIND source=%p generation_reset=1", source);
}

kern_return_t IsochTransmitContext::Configure(uint8_t channel,
                                              uint8_t sid,
                                              uint32_t streamModeRaw,
                                              uint32_t requestedChannels,
                                              uint32_t requestedAm824Slots,
                                              Encoding::AudioWireFormat wireFormat) noexcept {
    if (state_ != State::Unconfigured && state_ != State::Stopped) {
        ASFW_LOG(Isoch, "IT: Configure rejected - state=%s", TxStateName(state_));
        return kIOReturnBusy;
    }

    verifier_.BindRecovery(&recovery_);

    channel_ = channel;
    ring_.SetChannel(channel_);

    const kern_return_t krAudio = audio_.Configure(
        sid, streamModeRaw, requestedChannels, requestedAm824Slots, wireFormat);
    if (krAudio != kIOReturnSuccess) {
        return krAudio;
    }

    if (dmaMemory_) {
        // Allocate-once policy: IsochService keeps the IT context (and its DMA slabs) alive
        // across start/stop. Re-allocating on every Configure() exhausts the bump allocator.
        if (!ring_.HasRings()) {
            const kern_return_t kr = ring_.SetupRings(*dmaMemory_);
            if (kr != kIOReturnSuccess) {
                ASFW_LOG(Isoch, "IT: SetupRings failed");
                return kr;
            }
        }
    }

    state_ = State::Configured;
    ASFW_LOG(Isoch, "IT: Configured ch=%u sid=%u requestedChannels=%u wireChannels=%u",
             channel, sid, requestedChannels, audio_.ChannelCount());
    return kIOReturnSuccess;
}

kern_return_t IsochTransmitContext::Start() noexcept {
    if (state_ != State::Configured && state_ != State::Stopped) {
        ASFW_LOG(Isoch, "IT: Start rejected - state=%s", TxStateName(state_));
        return kIOReturnNotReady;
    }

    if (!hardware_) {
        ASFW_LOG(Isoch, "IT: Cannot start - no hardware");
        return kIOReturnNotReady;
    }

    if (!ring_.HasRings()) {
        ASFW_LOG(Isoch, "IT: Cannot start - no DMA ring");
        return kIOReturnNoResources;
    }

    packetsAssembled_ = 0;
    dataPackets_ = 0;
    noDataPackets_ = 0;
    tickCount_ = 0;
    interruptCount_.store(0, std::memory_order_relaxed);
    lastInterruptCountSeen_ = 0;
    irqStallTicks_ = 0;
    refillInProgress_.clear(std::memory_order_release);

    latencyBucket0_.store(0, std::memory_order_relaxed);
    latencyBucket1_.store(0, std::memory_order_relaxed);
    latencyBucket2_.store(0, std::memory_order_relaxed);
    latencyBucket3_.store(0, std::memory_order_relaxed);
    maxRefillLatencyUs_.store(0, std::memory_order_relaxed);
    irqWatchdogKicks_.store(0, std::memory_order_relaxed);

    ring_.ResetForStart();
    audio_.ResetForStart();
    ASFW_LOG(Isoch, "IT DBG START bindingSource=%p lastGen=%llu", directAudioBindingSource_,
             lastDirectAudioGeneration_);

    verifier_.ResetForStart(static_cast<uint8_t>(audio_.FramesPerDataPacket()));

    ring_.SeedCycleTracking(*hardware_);
    audio_.SetCycleTrackingValid(true);

    if (!audio_.PrimeSyncFromExternalBridge()) {
        ASFW_LOG(Isoch, "IT: Cannot start - missing fresh RX SYT seed before prime");
        return kIOReturnNotReady;
    }

    ring_.DebugFillDescriptorSlab(0xDE);
    ASFW_LOG(Isoch, "IT: Pre-filled descriptor slab (%zu bytes) with 0xDE pattern", Tx::Layout::kDescriptorRingSize);

    const auto primeStats = ring_.Prime(audio_);
    packetsAssembled_ += primeStats.packetsAssembled;
    dataPackets_ += primeStats.dataPackets;
    noDataPackets_ += primeStats.noDataPackets;

    ASFW_LOG(Isoch, "IT: Ring primed with %llu packets (%llu DATA, %llu NO-DATA)",
             packetsAssembled_, dataPackets_, noDataPackets_);

    ring_.DumpDescriptorRing(0, 4);
    ring_.DumpDescriptorRing(7, 1);

    Register32 cmdPtrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex_));
    Register32 ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex_));
    Register32 ctrlSetReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlSet(contextIndex_));
    Register32 ctrlClrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));

    const uint64_t descIOVA = ring_.Slab().DescriptorRegion().deviceBase;
    if (descIOVA == 0 || descIOVA > 0xFFFFFFFFULL) {
        ASFW_LOG(Isoch, "IT: Invalid descriptor IOVA 0x%llx", descIOVA);
        return kIOReturnInternalError;
    }
    const uint32_t cmdPtr = static_cast<uint32_t>(descIOVA) | Tx::Layout::kBlocksPerPacket;

    ASFW_LOG(Isoch, "IT: Writing CommandPtr=0x%08x (Z=%u)", cmdPtr, Tx::Layout::kBlocksPerPacket);
    hardware_->Write(cmdPtrReg, cmdPtr);

    hardware_->Write(ctrlClrReg, Driver::ContextControl::kWritableBits);

    hardware_->Write(Register32::kIsoXmitIntEventClear, 0xFFFFFFFF);
    hardware_->Write(Register32::kIsoXmitIntMaskSet, (1u << contextIndex_));
    hardware_->Write(Register32::kIntMaskSet, IntEventBits::kIsochTx);
    ASFW_LOG(Isoch, "IT: Enabled IT interrupt for context %u", contextIndex_);

    hardware_->Write(ctrlSetReg, Driver::ContextControl::kRun);

    const uint32_t readCmd = hardware_->Read(cmdPtrReg);
    const uint32_t readCtl = hardware_->Read(ctrlReg);

    const bool runSet = (readCtl & Driver::ContextControl::kRun) != 0;
    const bool activeSet = (readCtl & Driver::ContextControl::kActive) != 0;
    const bool deadSet = (readCtl & Driver::ContextControl::kDead) != 0;
    const uint32_t eventCode = (readCtl & Driver::ContextControl::kEventCodeMask) >> Driver::ContextControl::kEventCodeShift;

    ASFW_LOG(Isoch, "IT: Readback Cmd=0x%08x Ctl=0x%08x (run=%d active=%d dead=%d evt=0x%02x)",
             readCmd, readCtl, runSet, activeSet, deadSet, eventCode);

    if (deadSet) {
        ASFW_LOG(Isoch, "❌ IT: Context is DEAD immediately! Check descriptor program.");
        return kIOReturnNotPermitted;
    }

    state_ = State::Running;
    ASFW_LOG(Isoch, "IT: Started successfully");
    return kIOReturnSuccess;
}

void IsochTransmitContext::Stop() noexcept {
    if (state_ == State::Running && hardware_) {
        Register32 ctrlClrReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlClear(contextIndex_));
        hardware_->Write(ctrlClrReg, Driver::ContextControl::kRun);

        hardware_->Write(Register32::kIsoXmitIntMaskClear, (1u << contextIndex_));

        state_ = State::Stopped;
        refillInProgress_.clear(std::memory_order_release);
        ASFW_LOG(Isoch, "IT: Stopped. Stats: %llu pkts (%lluD/%lluN) IRQs=%llu",
                 packetsAssembled_, dataPackets_, noDataPackets_,
                 interruptCount_.load(std::memory_order_relaxed));
    }

    if (state_ == State::Configured) {
        state_ = State::Stopped;
        refillInProgress_.clear(std::memory_order_release);
        ASFW_LOG(Isoch, "IT: Stopped from configured state before hardware run");
    }

    verifier_.Shutdown();
}

void IsochTransmitContext::DoRefillOnce() noexcept {
    if (!hardware_ || state_ != State::Running) {
        return;
    }

    Tx::IsochTxCaptureHook* capture = nullptr;
    if (ASFW::LogConfig::Shared().IsIsochTxVerifierEnabled()) {
        capture = &verifier_;
    }

    const auto outcome = ring_.Refill(*hardware_, contextIndex_, audio_, capture, &audio_);
    if (!outcome.ok) {
        return;
    }

    packetsAssembled_ += outcome.packetsFilled;
    dataPackets_ += outcome.dataPackets;
    noDataPackets_ += outcome.noDataPackets;
}

void IsochTransmitContext::Poll() noexcept {
    if (state_ != State::Running) return;
    ++tickCount_;

    if (directAudioBindingSource_) {
        ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
        if (directAudioBindingSource_->CopyDirectAudioBinding(snapshot)) {
            if (tickCount_ <= 32 || (tickCount_ % 1000) == 0 ||
                snapshot.generation != lastDirectAudioGeneration_) {
                ASFW_LOG(Isoch,
                         "IT DBG BIND poll tick=%llu source=%p ready=1 gen=%llu lastGen=%llu valid=%d hasOut=%d outBase=%p outFrames=%u outCh=%u control=%p rate=%u hasIn=%d",
                         tickCount_,
                         directAudioBindingSource_,
                         snapshot.generation,
                         lastDirectAudioGeneration_,
                         snapshot.valid,
                         snapshot.HasOutput(),
                         static_cast<const void*>(snapshot.outputBase),
                         snapshot.outputFrames,
                         snapshot.outputChannels,
                         static_cast<void*>(snapshot.control),
                         snapshot.sampleRateHz,
                         snapshot.HasInput());
            }
            if (snapshot.generation != lastDirectAudioGeneration_) {
                if (snapshot.valid && snapshot.HasOutput()) {
                    ASFW_LOG(Isoch, "IT: direct audio binding changed (gen %llu -> %llu). Arming direct Tx.",
                             lastDirectAudioGeneration_, snapshot.generation);
                    IsochAudioTxPipeline::DirectTxRuntimeBinding binding{};
                    binding.outputBase = snapshot.outputBase;
                    binding.outputBytes = snapshot.outputBytes;
                    binding.outputFrames = snapshot.outputFrames;
                    binding.control = snapshot.control;
                    binding.enabled = true;
                    binding.sampleRateHz = snapshot.sampleRateHz;
                    binding.streamModeRaw = std::to_underlying(audio_.EffectiveStreamMode());
                    binding.outputChannels = snapshot.outputChannels;
                    binding.am824Slots = audio_.Am824SlotCount();
                    SetDirectTxRuntimeBinding(binding);
                } else {
                    ASFW_LOG(Isoch, "IT: direct audio binding invalid or has no output (gen %llu -> %llu). Disarming.",
                             lastDirectAudioGeneration_, snapshot.generation);
                    IsochAudioTxPipeline::DirectTxRuntimeBinding binding{};
                    SetDirectTxRuntimeBinding(binding);
                }
                lastDirectAudioGeneration_ = snapshot.generation;
            }
        } else {
            if (tickCount_ <= 32 || (tickCount_ % 1000) == 0) {
                ASFW_LOG(Isoch,
                         "IT DBG BIND poll tick=%llu source=%p ready=0 lastGen=%llu",
                         tickCount_, directAudioBindingSource_, lastDirectAudioGeneration_);
            }
            if (lastDirectAudioGeneration_ != 0) {
                ASFW_LOG(Isoch, "IT: direct audio binding cleared/unavailable. Disarming.");
                IsochAudioTxPipeline::DirectTxRuntimeBinding binding{};
                SetDirectTxRuntimeBinding(binding);
                lastDirectAudioGeneration_ = 0;
            }
        }
    } else if (tickCount_ <= 32 || (tickCount_ % 1000) == 0) {
        ASFW_LOG(Isoch, "IT DBG BIND poll tick=%llu source=null", tickCount_);
    }

    // IRQ-stall watchdog
    const uint64_t irqNow = interruptCount_.load(std::memory_order_relaxed);
    if (irqNow != lastInterruptCountSeen_) {
        lastInterruptCountSeen_ = irqNow;
        irqStallTicks_ = 0;
    } else {
        ++irqStallTicks_;
    }

    constexpr uint32_t kIrqStallThresholdTicks = 2;
    if (irqStallTicks_ >= kIrqStallThresholdTicks) {
        if (!refillInProgress_.test_and_set(std::memory_order_acq_rel)) {
            const uint64_t wdStart = mach_absolute_time();
            DoRefillOnce();
            const uint64_t wdEnd = mach_absolute_time();
            refillInProgress_.clear(std::memory_order_release);

            const uint64_t wdNs = ASFW::Timing::hostTicksToNanos(wdEnd - wdStart);
            const uint32_t wdUs = static_cast<uint32_t>(wdNs / 1000);
            if (wdUs < 50) {
                latencyBucket0_.fetch_add(1, std::memory_order_relaxed);
            } else if (wdUs < 200) {
                latencyBucket1_.fetch_add(1, std::memory_order_relaxed);
            } else if (wdUs < 500) {
                latencyBucket2_.fetch_add(1, std::memory_order_relaxed);
            } else {
                latencyBucket3_.fetch_add(1, std::memory_order_relaxed);
            }
            uint32_t wdMax = maxRefillLatencyUs_.load(std::memory_order_relaxed);
            while (wdUs > wdMax && !maxRefillLatencyUs_.compare_exchange_weak(
                       wdMax, wdUs, std::memory_order_relaxed, std::memory_order_relaxed)) {}
        }

        WakeHardware();
        irqWatchdogKicks_.fetch_add(1, std::memory_order_relaxed);
        irqStallTicks_ = 0;
    }

    // Periodic non-RT diagnostics.
    if (tickCount_ == 1 || (tickCount_ % 1000) == 0) {
        if (::ASFW::LogConfig::Shared().GetIsochVerbosity() >= 3) {
            const auto& ringC = ring_.RTCounters();
            const auto& audioC = audio_.RTCounters();
            ASFW_LOG(Isoch, "IT: Poll tick=%llu | ring(refills=%llu pkts=%llu) audio(directPackets=%llu underrunSilenced=%llu invalid=%llu)",
                     tickCount_,
                     ringC.refills.load(std::memory_order_relaxed),
                     ringC.packetsRefilled.load(std::memory_order_relaxed),
                     audioC.directTxPackets.load(std::memory_order_relaxed),
                     audioC.directTxUnderrunSilencedPackets.load(std::memory_order_relaxed),
                     audioC.directTxInvalidPackets.load(std::memory_order_relaxed));
        }
    }
}

void IsochTransmitContext::HandleInterrupt() noexcept {
    if (state_ != State::Running) return;
    interruptCount_.fetch_add(1, std::memory_order_relaxed);

    if (refillInProgress_.test_and_set(std::memory_order_acq_rel)) {
        return;
    }

    const uint64_t refillStart = mach_absolute_time();
    DoRefillOnce();
    const uint64_t refillEnd = mach_absolute_time();
    refillInProgress_.clear(std::memory_order_release);

    const uint64_t deltaNs = ASFW::Timing::hostTicksToNanos(refillEnd - refillStart);
    const uint32_t deltaUs = static_cast<uint32_t>(deltaNs / 1000);
    if (deltaUs < 50) {
        latencyBucket0_.fetch_add(1, std::memory_order_relaxed);
    } else if (deltaUs < 200) {
        latencyBucket1_.fetch_add(1, std::memory_order_relaxed);
    } else if (deltaUs < 500) {
        latencyBucket2_.fetch_add(1, std::memory_order_relaxed);
    } else {
        latencyBucket3_.fetch_add(1, std::memory_order_relaxed);
    }

    uint32_t prevMax = maxRefillLatencyUs_.load(std::memory_order_relaxed);
    while (deltaUs > prevMax && !maxRefillLatencyUs_.compare_exchange_weak(
               prevMax, deltaUs, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

void IsochTransmitContext::WakeHardware() noexcept {
    if (!hardware_) return;
    ring_.WakeHardwareIfIdle(*hardware_, contextIndex_);
}

void IsochTransmitContext::KickTxVerifier() noexcept {
    if (state_ != State::Running) {
        return;
    }

    IsochTxVerifier::Inputs in{};
    in.framesPerPacket = audio_.FramesPerDataPacket();
    in.pcmChannels = audio_.ChannelCount();
    in.am824Slots = audio_.Am824SlotCount();
    in.audioWireFormat = audio_.WireFormat();
    in.zeroCopyEnabled = true;
    in.sharedTxQueueValid = false;
    in.sharedTxQueueFillFrames = 0;

    const auto& audioC = audio_.RTCounters();
    const auto& ringC = ring_.RTCounters();
    in.audioInjectCursorResets = audioC.audioInjectCursorResets.load(std::memory_order_relaxed);
    in.audioInjectMissedPackets = audioC.audioInjectMissedPackets.load(std::memory_order_relaxed);
    in.underrunSilencedPackets = audioC.directTxUnderrunSilencedPackets.load(std::memory_order_relaxed);
    in.criticalGapEvents = ringC.criticalGapEvents.load(std::memory_order_relaxed);
    in.dbcDiscontinuities = 0; // DBC continuity check is producer-side only now

    verifier_.Kick(in);
}

void IsochTransmitContext::ServiceTxRecovery() noexcept {
    if (state_ != State::Running) {
        return;
    }

    const uint64_t nowNs = ASFW::LogDetail::NowNs();
    uint32_t reasons = 0;
    if (!recovery_.TryBegin(nowNs, reasons)) {
        return;
    }

    const uint64_t restartIndex = recovery_.RestartCount() + 1;
    ASFW_LOG_V0(Isoch,
                "IT TX RECOVER: restarting IT (idx=%llu reasons=0x%08x invalid_label=%d cip=%d dbc=%d uncomplete=%d inject_miss=%d)",
                restartIndex, reasons,
                (reasons & IsochTxRecoveryController::kReasonInvalidLabel) != 0,
                (reasons & IsochTxRecoveryController::kReasonCipAnomaly) != 0,
                (reasons & IsochTxRecoveryController::kReasonDbcDiscontinuity) != 0,
                (reasons & IsochTxRecoveryController::kReasonUncompletedOverwrite) != 0,
                (reasons & IsochTxRecoveryController::kReasonInjectMiss) != 0);

    if (recoveryCallback_) {
        if (recoveryCallback_(reasons)) {
            ASFW_LOG_V0(Isoch, "IT TX RECOVER: delegated to upper-layer recovery coordinator");
            recovery_.Complete(nowNs, reasons, true);
            return;
        }

        ASFW_LOG_V1(Isoch,
                    "IT TX RECOVER: upper-layer recovery delegate rejected request, will retry later");
        recovery_.Complete(nowNs, reasons, false);
        return;
    }

    Stop();
    const kern_return_t kr = Start();
    const bool ok = (kr == kIOReturnSuccess);
    if (!ok) {
        ASFW_LOG_V0(Isoch, "IT TX RECOVER: restart failed (kr=0x%08x), will retry", kr);
    }

    recovery_.Complete(nowNs, reasons, ok);
}

void IsochTransmitContext::LogStatistics() const noexcept {
    // No-op for now, simplified architecture.
}

void IsochTransmitContext::DumpPayloadBuffers(uint32_t numPackets) const noexcept {
    ring_.DumpPayloadBuffers(numPackets);
}

void IsochTransmitContext::DumpDescriptorRing(uint32_t startPacket, uint32_t numPackets) const noexcept {
    ring_.DumpDescriptorRing(startPacket, numPackets);
}

} // namespace ASFW::Isoch
