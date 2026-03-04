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

bool AsyncSubsystem::Cancel(AsyncHandle) {
    return false;
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
