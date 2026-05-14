/**
 * PayloadContextStub.cpp
 *
 * Host-test stub for PayloadContext. The async command builder tests only
 * exercise BuildHeader() and never call PreparePayload() / Submit(), so
 * DMA allocation is not needed. This stub satisfies the linker without
 * pulling in real DriverKit IOMemoryMap / IODMACommand headers.
 */

#include "ASFWDriver/Async/Tx/PayloadContext.hpp"

namespace ASFW::Async {

struct PayloadContext::DMABufferImpl {};

// Factory — always returns nullptr in host-test context.
std::unique_ptr<PayloadContext> PayloadContext::Create(
    ASFW::Driver::HardwareInterface& /*hw*/,
    const void* /*data*/,
    std::size_t /*length*/,
    uint64_t /*direction*/)
{
    return nullptr;
}

PayloadContext::~PayloadContext() {}

bool PayloadContext::Initialize(ASFW::Driver::HardwareInterface& /*hw*/,
                                const void* /*data*/,
                                std::size_t /*length*/,
                                uint64_t /*direction*/)
{
    return false;
}

void PayloadContext::Cleanup() {}

uint64_t PayloadContext::DeviceAddress() const noexcept { return 0; }

std::shared_ptr<void> PayloadContext::IntoShared(std::unique_ptr<PayloadContext>&& up) {
    return std::shared_ptr<void>(up.release(), [](void* p) {
        delete static_cast<PayloadContext*>(p);
    });
}

} // namespace ASFW::Async
