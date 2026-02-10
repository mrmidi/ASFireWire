#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSObject.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include "../Controller/ControllerTypes.hpp"

namespace ASFW {
namespace Async {
class AsyncSubsystem;
}
}

class ASFWDriverUserClient;

namespace ASFW::Driver {

class ControllerCore;

class StatusPublisher {
public:
    StatusPublisher() = default;
    ~StatusPublisher() = default;

    kern_return_t Prepare();
    void Reset();

    void Publish(ControllerCore* controller,
                 const ASFW::Async::AsyncSubsystem* asyncSubsystem,
                 SharedStatusReason reason,
                 uint32_t detailMask = 0);

    void BindListener(::ASFWDriverUserClient* client);
    void UnbindListener(::ASFWDriverUserClient* client);

    kern_return_t CopySharedMemory(uint64_t* options, IOMemoryDescriptor** memory) const;

    void SetLastAsyncCompletion(uint64_t machTime);

    void UpdateAsyncWatchdog(uint32_t asyncTimeoutCount,
                             uint64_t watchdogTickCount,
                             uint64_t watchdogLastTickUsec);

    const SharedStatusBlock* StatusBlock() const { return statusBlock_; }

private:
    OSSharedPtr<IOBufferMemoryDescriptor> statusMemory_;
    OSSharedPtr<IOMemoryMap> statusMap_;
    SharedStatusBlock* statusBlock_{nullptr};
    std::atomic<uint64_t> statusSequence_{0};
    OSSharedPtr<OSObject> statusListener_;
    std::atomic<uint64_t> lastAsyncCompletionMach_{0};
    std::atomic<uint32_t> asyncTimeoutCount_{0};
    std::atomic<uint64_t> watchdogTickCount_{0};
    std::atomic<uint64_t> watchdogLastTickUsec_{0};
};

} // namespace ASFW::Driver
