// IsochTransmitContext.cpp
// ASFW - Isochronous Transmit Context (orchestrator)
//
// NOTE:
// OHCI IT programming details are in Tx::IsochTxDmaRing.
// Audio semantics (CIP/AM824/queue/zero-copy/external sync) are in IsochAudioTxPipeline.
//

#include "IsochTransmitContext.hpp"

#include "../Encoding/TimingUtils.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Logging/LogConfig.hpp"

#include <cstring>

namespace ASFW::Isoch {

using namespace ASFW::Driver;

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

void IsochTransmitContext::SetSharedTxQueue(void* base, uint64_t bytes) noexcept {
    audio_.SetSharedTxQueue(base, bytes);
}

void IsochTransmitContext::SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept {
    audio_.SetExternalSyncBridge(bridge);
}

uint32_t IsochTransmitContext::SharedTxFillLevelFrames() const noexcept {
    return audio_.SharedTxFillLevelFrames();
}

uint32_t IsochTransmitContext::SharedTxCapacityFrames() const noexcept {
    return audio_.SharedTxCapacityFrames();
}

void IsochTransmitContext::SetZeroCopyOutputBuffer(void* base, uint64_t bytes, uint32_t frameCapacity) noexcept {
    audio_.SetZeroCopyOutputBuffer(base, bytes, frameCapacity);
}

kern_return_t IsochTransmitContext::Configure(uint8_t channel,
                                              uint8_t sid,
                                              uint32_t streamModeRaw,
                                              uint32_t requestedChannels) noexcept {
    if (state_ != State::Unconfigured && state_ != State::Stopped) {
        return kIOReturnBusy;
    }

    verifier_.BindRecovery(&recovery_);

    channel_ = channel;
    ring_.SetChannel(channel_);

    const kern_return_t krAudio = audio_.Configure(sid, streamModeRaw, requestedChannels);
    if (krAudio != kIOReturnSuccess) {
        return krAudio;
    }

    if (dmaMemory_) {
        const kern_return_t kr = ring_.SetupRings(*dmaMemory_);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Isoch, "IT: SetupRings failed");
            return kr;
        }
    }

    state_ = State::Configured;
    ASFW_LOG(Isoch, "IT: Configured ch=%u sid=%u requestedChannels=%u queueChannels=%u",
             channel, sid, requestedChannels, audio_.ChannelCount());
    return kIOReturnSuccess;
}

kern_return_t IsochTransmitContext::Start() noexcept {
    if (state_ != State::Configured && state_ != State::Stopped) {
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

    lastUnderrunCount_ = 0;

    ring_.ResetForStart();
    audio_.ResetForStart();

    verifier_.ResetForStart(static_cast<uint8_t>(audio_.FramesPerDataPacket()));

    ring_.SeedCycleTracking(*hardware_);
    audio_.SetCycleTrackingValid(true);

    if (audio_.SharedTxQueueValid() && !audio_.IsZeroCopyEnabled()) {
        audio_.PrePrimeFromSharedQueue();
    }

    ring_.DebugFillDescriptorSlab(0xDE);
    ASFW_LOG(Isoch, "IT: Pre-filled descriptor slab (%zu bytes) with 0xDE pattern", Tx::Layout::kDescriptorRingSize);

    const auto primeStats = ring_.Prime(audio_);
    packetsAssembled_ += primeStats.packetsAssembled;
    dataPackets_ += primeStats.dataPackets;
    noDataPackets_ += primeStats.noDataPackets;

    ASFW_LOG(Isoch, "IT: Ring primed with %llu packets (%llu DATA, %llu NO-DATA)",
             packetsAssembled_, dataPackets_, noDataPackets_);

    constexpr uint32_t kMinPrimeData = Config::kTxBufferProfile.minPrimeDataPackets;
    if (kMinPrimeData > 0 && dataPackets_ < kMinPrimeData) {
        ASFW_LOG(Isoch, "IT: WARNING: PrimeRing produced only %llu DATA packets (minimum=%u). "
                 "Audio may click at start.",
                 dataPackets_, kMinPrimeData);
    }

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

    hardware_->Write(ctrlClrReg, ContextControl::kWritableBits);

    hardware_->Write(Register32::kIsoXmitIntEventClear, 0xFFFFFFFF);
    hardware_->Write(Register32::kIsoXmitIntMaskSet, (1u << contextIndex_));
    hardware_->Write(Register32::kIntMaskSet, IntEventBits::kIsochTx);
    ASFW_LOG(Isoch, "IT: Enabled IT interrupt for context %u", contextIndex_);

    hardware_->Write(ctrlSetReg, ContextControl::kRun);

    const uint32_t readCmd = hardware_->Read(cmdPtrReg);
    const uint32_t readCtl = hardware_->Read(ctrlReg);

    const uint32_t isoXmitIntMask = hardware_->Read(Register32::kIsoXmitIntMaskSet);
    const uint32_t intMask = hardware_->Read(Register32::kIntMaskSet);

    const bool runSet = (readCtl & ContextControl::kRun) != 0;
    const bool activeSet = (readCtl & ContextControl::kActive) != 0;
    const bool deadSet = (readCtl & ContextControl::kDead) != 0;
    const uint32_t eventCode = (readCtl & ContextControl::kEventCodeMask) >> ContextControl::kEventCodeShift;

    ASFW_LOG(Isoch, "IT: Readback Cmd=0x%08x Ctl=0x%08x (run=%d active=%d dead=%d evt=0x%02x)",
             readCmd, readCtl, runSet, activeSet, deadSet, eventCode);
    ASFW_LOG(Isoch, "IT: IntMasks - IsoXmit=0x%08x Global=0x%08x (IsochTx bit=%d)",
             isoXmitIntMask, intMask, (intMask & IntEventBits::kIsochTx) != 0);

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
        hardware_->Write(ctrlClrReg, ContextControl::kRun);

        hardware_->Write(Register32::kIsoXmitIntMaskClear, (1u << contextIndex_));

        state_ = State::Stopped;
        refillInProgress_.clear(std::memory_order_release);
        ASFW_LOG(Isoch, "IT: Stopped. Stats: %llu pkts (%lluD/%lluN) IRQs=%llu",
                 packetsAssembled_, dataPackets_, noDataPackets_,
                 interruptCount_.load(std::memory_order_relaxed));
    }

    verifier_.Shutdown();
}

void IsochTransmitContext::DoRefillOnce() noexcept {
    if (!hardware_ || state_ != State::Running) {
        return;
    }

    audio_.OnRefillTickPreHW();

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

    audio_.OnPollTick1ms();

    // Periodic non-RT diagnostics.
    if (tickCount_ == 1 || (tickCount_ % 1000) == 0) {
        const uint32_t rbFill = audio_.BufferFillLevel();
        const uint32_t txFill = audio_.SharedTxFillLevelFrames();

        const uint64_t underrunNow = audio_.UnderrunCount();
        const uint64_t underrunDelta = underrunNow - lastUnderrunCount_;
        lastUnderrunCount_ = underrunNow;
        if (underrunDelta > 0) {
            ASFW_LOG(Isoch, "IT: UNDERRUN %llu frames (total=%llu) rbFill=%u txFill=%u",
                     underrunDelta, underrunNow, rbFill, txFill);
        }

        if (::ASFW::LogConfig::Shared().GetIsochVerbosity() >= 3) {
            const auto& ringC = ring_.RTCounters();
            const auto& audioC = audio_.RTCounters();
            ASFW_LOG(Isoch, "IT: Poll tick=%llu zeroCopy=%{public}s rbFill=%u txFill=%u | ring(calls=%llu refills=%llu pkts=%llu dead=%llu dec=%llu oob=%llu gapCrit=%llu) audio(resync=%llu drop=%llu injectReset=%llu injectMiss=%llu zeroExit=%llu silenced=%llu)",
                     tickCount_,
                     audio_.IsZeroCopyEnabled() ? "YES" : "NO",
                     rbFill,
                     txFill,
                     ringC.calls.load(std::memory_order_relaxed),
                     ringC.refills.load(std::memory_order_relaxed),
                     ringC.packetsRefilled.load(std::memory_order_relaxed),
                     ringC.exitDead.load(std::memory_order_relaxed),
                     ringC.exitDecodeFail.load(std::memory_order_relaxed),
                     ringC.exitHwOOB.load(std::memory_order_relaxed),
                     ringC.criticalGapEvents.load(std::memory_order_relaxed),
                     audioC.resyncApplied.load(std::memory_order_relaxed),
                     audioC.staleFramesDropped.load(std::memory_order_relaxed),
                     audioC.audioInjectCursorResets.load(std::memory_order_relaxed),
                     audioC.audioInjectMissedPackets.load(std::memory_order_relaxed),
                     audioC.exitZeroRefill.load(std::memory_order_relaxed),
                     audioC.underrunSilencedPackets.load(std::memory_order_relaxed));
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
    in.channels = audio_.ChannelCount();
    in.zeroCopyEnabled = audio_.IsZeroCopyEnabled();
    in.sharedTxQueueValid = audio_.SharedTxQueueValid();
    in.sharedTxQueueFillFrames = audio_.SharedTxFillLevelFrames();

    const auto& audioC = audio_.RTCounters();
    const auto& ringC = ring_.RTCounters();
    in.audioInjectCursorResets = audioC.audioInjectCursorResets.load(std::memory_order_relaxed);
    in.audioInjectMissedPackets = audioC.audioInjectMissedPackets.load(std::memory_order_relaxed);
    in.underrunSilencedPackets = audioC.underrunSilencedPackets.load(std::memory_order_relaxed);
    in.criticalGapEvents = ringC.criticalGapEvents.load(std::memory_order_relaxed);
    in.dbcDiscontinuities = audio_.DbcDiscontinuityCount();

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

    Stop();
    const kern_return_t kr = Start();
    const bool ok = (kr == kIOReturnSuccess);
    if (!ok) {
        ASFW_LOG_V0(Isoch, "IT TX RECOVER: restart failed (kr=0x%08x), will retry", kr);
    }

    recovery_.Complete(nowNs, reasons, ok);
}

void IsochTransmitContext::LogStatistics() const noexcept {
    if (!hardware_ || state_ != State::Running) {
        return;
    }

    const uint32_t cmdPtr = hardware_->Read(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(contextIndex_)));
    const uint32_t ctrl = hardware_->Read(static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(contextIndex_)));

    const bool run = (ctrl & ContextControl::kRun) != 0;
    const bool active = (ctrl & ContextControl::kActive) != 0;
    const bool dead = (ctrl & ContextControl::kDead) != 0;
    const uint32_t eventCode = (ctrl & ContextControl::kEventCodeMask) >> ContextControl::kEventCodeShift;

    ASFW_LOG(Isoch, "IT: run=%d active=%d dead=%d evt=0x%02x pkts=%llu IRQ=%llu | CmdPtr=0x%08x Ctrl=0x%08x",
             run, active, dead, eventCode,
             packetsAssembled_,
             interruptCount_.load(std::memory_order_relaxed),
             cmdPtr, ctrl);
}

void IsochTransmitContext::DumpPayloadBuffers(uint32_t numPackets) const noexcept {
    ring_.DumpPayloadBuffers(numPackets);
}

void IsochTransmitContext::PrimeOnly() noexcept {
    if (!ring_.HasRings()) return;
    ring_.Prime(audio_);
}

void IsochTransmitContext::DumpDescriptorRing(uint32_t startPacket, uint32_t numPackets) const noexcept {
    ring_.DumpDescriptorRing(startPacket, numPackets);
}

} // namespace ASFW::Isoch

