#include "ASFWDriver/Async/AsyncSubsystem.hpp"

#include "ASFWDriver/Async/AsyncSubsystem.hpp"

// Stub definitions for unique_ptr types to allow destructor to compile
namespace ASFW::Async {
    class PacketBuilder {};
    class ResponseSender {};
    
    namespace Tx { class Submitter {}; }
}

namespace ASFW::Async {

AsyncSubsystem::AsyncSubsystem() {}
AsyncSubsystem::~AsyncSubsystem() {}

AsyncHandle AsyncSubsystem::Write(const WriteParams& params, CompletionCallback callback) {
    // Stub: immediately succeed
    if (callback) {
        callback(AsyncHandle{1}, AsyncStatus::kSuccess, {});
    }
    return AsyncHandle{1};
}

bool AsyncSubsystem::Cancel(AsyncHandle handle) {
    return true;
}

// Stub destructors for linked classes
PayloadRegistry::~PayloadRegistry() {}
TransactionManager::~TransactionManager() {}
namespace Engine {
    struct ContextManager::State {}; // Define incomplete type
    ContextManager::~ContextManager() {}
}

} // namespace ASFW::Async

namespace ASFW::Debug {
    BusResetPacketCapture::~BusResetPacketCapture() {}
}

namespace ASFW::Shared {
    PayloadHandle::~PayloadHandle() noexcept {}
    void PayloadHandle::Release() noexcept {}
}
