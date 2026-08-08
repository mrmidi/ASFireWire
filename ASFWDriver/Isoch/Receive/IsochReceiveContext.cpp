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
    const uint32_t ctlValue = Driver::ContextControl::kRun | Driver::ContextControl::kIsochHeader;
    const uint32_t contextMask = 1u << contextIndex_;
    {
        auto access = hardware_->TryBeginAccess();
        if (!access) return kIOReturnNotReady;
        access.Write(registers_.ContextMatch, contextMatch);
        access.Write(registers_.CommandPtr, cmdPtr);
        access.Write(registers_.ContextControlClear, 0xFFFFFFFFu);
        access.Write(registers_.ContextControlSet, ctlValue);
        access.Write(ASFW::Driver::Register32::kIsoRecvIntMaskSet, contextMask);
    }
    ASFW_LOG(Isoch, "Start: Enabled IR interrupt for context %u (mask=0x%08x)", contextIndex_, contextMask);

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

void IsochReceiveContext::LogHardwareState() {
}

void IsochReceiveContext::DrainZtsTelemetry(uint32_t maxRecords) {
    if (receiveConsumer_) receiveConsumer_->DrainReceiveTelemetry(maxRecords);
}

void IsochReceiveContext::DrainPayloadWriterTelemetry() {
    if (receiveConsumer_) receiveConsumer_->DrainPayloadTelemetry();
}

void IsochReceiveContext::LogTxSytTrace() {
    if (receiveConsumer_) receiveConsumer_->LogTransmitTimingTrace();
}

} // namespace ASFW::Isoch
