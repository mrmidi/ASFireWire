#include "IsochReceiveContext.hpp"
#include "../Core/IsochEventGroup.hpp"
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
kern_return_t IsochReceiveContext::Configure(uint8_t channel, uint8_t contextIndex) {
    if (!hardware_ || !dmaMemory_) {
        return kIOReturnNotReady;
    }

    if (contextIndex >= 4) {
        return kIOReturnBadArgument;
    }

    contextIndex_ = contextIndex;
    channel_ = channel;
    registers_ = GetRegisters(contextIndex_);
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

    const uint32_t cmdPtr = rxRing_.InitialCommandPtrWord();
    if (cmdPtr == 0) {
        ASFW_LOG(Isoch, "❌ Start: Invalid descriptor cmdPtr");
        return kIOReturnInternalError;
    }
    const uint32_t contextMatch = 0xF0000000 | (channel_ & 0x3F);
    // kRun (bit 15) | kWake (bit 12) | kIsochHeader (bit 30).
    //   - kWake is what makes the context actually fetch from CommandPtr — without it the
    //     context arms (RUN set, readback shows active) but the IR DMA never runs and the
    //     receive drain never completes (-> ZTS timeout 0xe00002d6). Confirmed on a
    //     MOTU 828 Mk3.
    //   - kIsochHeader (OHCI 1.1 section 10.6.2.1) unchanged.
    // The register writes themselves now go through the revocable access scope below;
    // this only names the value.
    const uint32_t ctlValue = Driver::ContextControl::kRun |
                              Driver::ContextControl::kWake |
                              Driver::ContextControl::kIsochHeader;
    const uint32_t contextMask = 1u << contextIndex_;
    uint32_t ctrlBefore = 0;
    uint32_t pendingEvents = 0;
    {
        auto access = hardware_->TryBeginAccess();
        if (!access) return kIOReturnNotReady;
        // What the context held before this start (one line per start). The
        // Set address returns the unmasked events.
        ctrlBefore = access.Read(registers_.ContextControlSet);
        pendingEvents = access.Read(ASFW::Driver::Register32::kIsoRecvIntEventSet);
        access.Write(registers_.ContextMatch, contextMatch);
        access.Write(registers_.CommandPtr, cmdPtr);
        access.Write(registers_.ContextControlClear, 0xFFFFFFFFu);
        // Stopping a context sets its event bit (OHCI 1.2 draft §3.1.1.3).
        // Clear this context's stale bit and unmask it before RUN, as Linux
        // ohci_start_iso does (ohci.c:3209), so the clear cannot take a
        // completion of this run.
        access.Write(ASFW::Driver::Register32::kIsoRecvIntEventClear, contextMask);
        access.Write(ASFW::Driver::Register32::kIsoRecvIntMaskSet, contextMask);
        access.Write(registers_.ContextControlSet, ctlValue);
    }
    ASFW_LOG(Isoch, "IR: start ctx=%u ctrlBefore=0x%08x pendingEvents=0x%08x",
             contextIndex_, ctrlBefore, pendingEvents);

    // Readback proves kWake actually landed (bit 10 = ACTIVE) instead of just arming RUN.
    // Reads go through a revocable access scope; a revoked
    // scope leaves this 0, which reads as "not active" and is the honest answer.
    uint32_t readCtl = 0;
    if (auto access = hardware_->TryBeginAccess()) {
        readCtl = access.Read(registers_.ContextControlSet);
    }
    ASFW_LOG(Isoch, "Start: IR readback Ctl=0x%08x (run=%u wake=%u active=%u)",
             readCtl,
             (readCtl & Driver::ContextControl::kRun) != 0,
             (readCtl & Driver::ContextControl::kWake) != 0,
             (readCtl & Driver::ContextControl::kActive) != 0);

    while (rxLock_.test_and_set(std::memory_order_acquire)) {
    }

    Transition(IRPolicy::State::Running, 0, "Start");
    rxRing_.ResetForStart();

    if (receiveConsumer_) {
        receiveConsumer_->OnReceiveActivated();
    }
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
    if (auto access = hardware_->TryBeginAccess()) {
        access.Write(ASFW::Driver::Register32::kIsoRecvIntMaskClear, contextMask);
        access.WriteAndFlush(registers_.ContextControlClear, Driver::ContextControl::kRun);
    } else {
        if (hardware_->HardwareGone()) {
            // Provider revocation is proof that this controller cannot issue a
            // late DMA write. Retaining the direct binding in that case turns
            // a safe surprise-removal into a leak/UAF hazard.
            Transition(IRPolicy::State::Stopped, 0, "Stop/provider-gone");
            if (receiveConsumer_) {
                receiveConsumer_->OnReceiveQuiesced();
            }
            ASFW_LOG(Isoch,
                     "[Lifecycle] IR stop context=%u hardware-gone action=release-direct-binding",
                     contextIndex_);
            rxLock_.clear(std::memory_order_release);
            return kIOReturnSuccess;
        }
        rxLock_.clear(std::memory_order_release);
        return kIOReturnNotReady;
    }
    // Flush RUN-clear and wait for ACTIVE to fall before dropping the direct
    // audio binding.  See Linux firewire/ohci.c:1361-1378 for the same
    // teardown ordering; freeing this mapping while ACTIVE is set can fault
    // the host when OHCI completes a late DMA write.
    ASFW_LOG(Isoch, "Stop: Disabled IR interrupt for context %u", contextIndex_);

    // Two complementary guards, both required: the revocable access scope keeps
    // us from issuing MMIO after Detach, and the all-ones sentinel catches a
    // physically removed device, which still reads 0xFFFFFFFF through a live scope.
    const auto readControl = [this] {
        auto access = hardware_->TryBeginAccess();
        return access ? access.Read(registers_.ContextControlSet) : 0U;
    };
    const uint32_t initialControl = readControl();
    if (initialControl != 0xFFFFFFFFu &&
        (initialControl & Driver::ContextControl::kActive) != 0) {
        IODelay(5);
        constexpr uint32_t kMaxIterations = 250;
        constexpr uint32_t kBaseDelayMicros = 6;
        for (uint32_t iteration = 0; iteration < kMaxIterations; ++iteration) {
            const uint32_t polledControl = readControl();
            if (polledControl == 0xFFFFFFFFu ||
                (polledControl & Driver::ContextControl::kActive) == 0) {
                break;
            }
            IODelay(kBaseDelayMicros + iteration);
        }
    }

    const uint32_t control = readControl();
    if (control != 0xFFFFFFFFu &&
        (control & Driver::ContextControl::kActive) != 0) {
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

    if (receiveConsumer_) {
        receiveConsumer_->OnReceiveQuiesced();
    }

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

    const auto cycleHostPair =
        hardware_
            ? hardware_->ReadCycleTimeAndUpTime()
            : std::pair<uint32_t, uint64_t>{0, mach_absolute_time()};
    const uint32_t drainCycleTimer = cycleHostPair.first;
    const uint64_t drainHostTicks = cycleHostPair.second;
    const IsochReceiveBatch receiveBatch{
        .drainCycleTimer = drainCycleTimer,
        .drainHostTicks = drainHostTicks,
    };
    if (receiveConsumer_) {
        receiveConsumer_->BeginReceiveBatch(receiveBatch);
    }

    const uint32_t processed = rxRing_.DrainCompleted(
        *dmaMemory_,
        [this, drainHostTicks, drainCycleTimer, receiveBatch](
            const Rx::IsochRxDmaRing::CompletedPacket& pkt) {
        uint64_t callbackTimestamp = 0;
        if (receiveConsumer_) {
            receiveConsumer_->ConsumePacket(
                receiveBatch,
                IsochReceivePacket{
                    .descriptorIndex = pkt.descriptorIndex,
                    .transferStatus = pkt.xferStatus,
                    .residualCount = pkt.resCount,
                    .payload = pkt.payload
                        ? std::span<const uint8_t>(pkt.payload, pkt.actualLength)
                        : std::span<const uint8_t>{},
                });
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

void IsochReceiveContext::SetCallback(IsochReceiveCallback callback) {
    callback_ = callback;
}

void IsochReceiveContext::SetReceiveConsumer(
    IIsochReceiveConsumer* consumer) noexcept {
    receiveConsumer_ = consumer;
}

void IsochReceiveContext::DrainZtsTelemetry(uint32_t maxRecords) {
    if (receiveConsumer_) receiveConsumer_->DrainReceiveTelemetry(maxRecords);
}

void IsochReceiveContext::ServiceConsumerDiagnostics() {
    if (receiveConsumer_) receiveConsumer_->ServiceConsumerDiagnostics();
}

} // namespace ASFW::Isoch
