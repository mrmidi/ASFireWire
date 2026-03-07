#include "ASFWDriver/Async/AsyncSubsystem.hpp"

// Stub definitions for unique_ptr-owned forward-declared types so AsyncSubsystem's
// destructor can compile in this host-only test target.
namespace ASFW::Async {
class PacketBuilder {};
class ResponseSender {};
namespace Tx {
class Submitter {};
} // namespace Tx
} // namespace ASFW::Async

namespace ASFW::Async {

AsyncSubsystem::AsyncSubsystem() = default;
AsyncSubsystem::~AsyncSubsystem() = default;

AsyncHandle AsyncSubsystem::Read(const ReadParams&, CompletionCallback callback) {
    const AsyncHandle handle{1};
    if (callback) {
        callback(handle, AsyncStatus::kHardwareError, 0xFF, {});
    }
    return handle;
}

AsyncHandle AsyncSubsystem::ReadWithRetry(const ReadParams&, const RetryPolicy&,
                                          CompletionCallback callback) {
    const AsyncHandle handle{1};
    if (callback) {
        callback(handle, AsyncStatus::kHardwareError, 0xFF, {});
    }
    return handle;
}

AsyncHandle AsyncSubsystem::Write(const WriteParams&, CompletionCallback callback) {
    const AsyncHandle handle{1};
    if (callback) {
        callback(handle, AsyncStatus::kHardwareError, 0xFF, {});
    }
    return handle;
}

AsyncHandle AsyncSubsystem::Lock(const LockParams&, uint16_t, CompletionCallback callback) {
    const AsyncHandle handle{1};
    if (callback) {
        callback(handle, AsyncStatus::kHardwareError, 0xFF, {});
    }
    return handle;
}

AsyncHandle AsyncSubsystem::CompareSwap(const CompareSwapParams&, CompareSwapCallback callback) {
    if (callback) {
        callback(AsyncStatus::kHardwareError, 0u, false);
    }
    return AsyncHandle{1};
}

AsyncHandle AsyncSubsystem::PhyRequest(const PhyParams&, CompletionCallback callback) {
    const AsyncHandle handle{1};
    if (callback) {
        callback(handle, AsyncStatus::kHardwareError, 0xFF, {});
    }
    return handle;
}

bool AsyncSubsystem::Cancel(AsyncHandle) { return false; }

void AsyncSubsystem::OnTxInterrupt() {}

void AsyncSubsystem::OnRxInterrupt(ARContextType) {}

kern_return_t AsyncSubsystem::ArmARContextsOnly() { return kIOReturnSuccess; }

void AsyncSubsystem::OnBusResetBegin(uint8_t) {}

void AsyncSubsystem::OnBusResetComplete(uint8_t) {}

void AsyncSubsystem::ConfirmBusGeneration(uint8_t) {}

void AsyncSubsystem::StopATContextsOnly() {}

void AsyncSubsystem::FlushATContexts() {}

void AsyncSubsystem::RearmATContexts() {}

void AsyncSubsystem::OnTimeoutTick() {}

AsyncWatchdogStats AsyncSubsystem::GetWatchdogStats() const { return {}; }

DMAMemoryManager* AsyncSubsystem::GetDMAManager() { return nullptr; }

std::optional<AsyncStatusSnapshot> AsyncSubsystem::GetStatusSnapshot() const {
    return std::nullopt;
}

} // namespace ASFW::Async

// Stub destructors for out-of-line definitions that this test target doesn't link.
namespace ASFW::Async::Engine {
struct ContextManager::State {};
ContextManager::~ContextManager() {}
} // namespace ASFW::Async::Engine

namespace ASFW::Debug {
BusResetPacketCapture::~BusResetPacketCapture() {}
} // namespace ASFW::Debug

namespace ASFW::Shared {
PayloadHandle::~PayloadHandle() noexcept {}
void PayloadHandle::Release() noexcept {}
} // namespace ASFW::Shared
