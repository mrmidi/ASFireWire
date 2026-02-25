#include "IsochReceiveContext.hpp"

#include "../../Common/DriverKitUtils.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Hardware/RegisterMap.hpp"
#include "../../Diagnostics/Signposts.hpp"

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

    audio_.ConfigureFor48k();

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
    const uint32_t ctlValue = ContextControl::kRun | ContextControl::kIsochHeader;
    hardware_->Write(registers_.ContextControlSet, ctlValue);

    const uint32_t contextMask = 1u << contextIndex_;
    hardware_->Write(ASFW::Driver::Register32::kIsoRecvIntMaskSet, contextMask);
    ASFW_LOG(Isoch, "Start: Enabled IR interrupt for context %u (mask=0x%08x)", contextIndex_, contextMask);

    const uint32_t readMatch = hardware_->Read(registers_.ContextMatch);
    const uint32_t readCmd = hardware_->Read(registers_.CommandPtr);
    const uint32_t readCtl = hardware_->Read(registers_.ContextControlSet);

    ASFW_LOG(Isoch, "Start: Wrote Match=0x%08x Cmd=0x%08x Ctl=0x%08x", contextMatch, cmdPtr, ctlValue);
    ASFW_LOG(Isoch, "Start: Readback Match=0x%08x Cmd=0x%08x Ctl=0x%08x", readMatch, readCmd, readCtl);

    const bool deadSet = (readCtl & ContextControl::kDead) != 0;
    if (deadSet) {
        ASFW_LOG(Isoch, "❌ Start: Context is DEAD! Check descriptor program.");
        return kIOReturnNotPermitted;
    }

    while (rxLock_.test_and_set(std::memory_order_acquire)) {
    }

    Transition(IRPolicy::State::Running, 0, "Start");

    rxRing_.ResetForStart();
    audio_.OnStart();

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

    hardware_->Write(registers_.ContextControlClear, ContextControl::kRun);

    const uint32_t contextMask = 1u << contextIndex_;
    hardware_->Write(ASFW::Driver::Register32::kIsoRecvIntMaskClear, contextMask);
    ASFW_LOG(Isoch, "Stop: Disabled IR interrupt for context %u", contextIndex_);

    Transition(IRPolicy::State::Stopped, 0, "Stop");

    audio_.OnStop();

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

    const uint64_t start = mach_absolute_time();

    const uint32_t processed = rxRing_.DrainCompleted(*dmaMemory_, [&](const Rx::IsochRxDmaRing::CompletedPacket& pkt) {
        if (pkt.payload) {
            audio_.OnPacket(pkt.payload, pkt.actualLength);
        }

        if (callback_) {
            const auto span = std::span<const uint8_t>(pkt.payload, pkt.actualLength);
            callback_(span, static_cast<uint32_t>(pkt.xferStatus), 0);
        }
    });

    audio_.OnPollEnd(*hardware_, processed, start);

    rxLock_.clear(std::memory_order_release);
    return processed;
}

void IsochReceiveContext::SetSharedRxQueue(void* base, uint64_t bytes) {
    audio_.SetSharedRxQueue(base, bytes);
}

void IsochReceiveContext::SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept {
    audio_.SetExternalSyncBridge(bridge);
}

void IsochReceiveContext::SetCallback(IsochReceiveCallback callback) {
    callback_ = callback;
}

void IsochReceiveContext::LogHardwareState() {
#if 0
    // Keep disabled unless troubleshooting; should be reimplemented using rxRing_ accessors.
#endif
}

} // namespace ASFW::Isoch

