#pragma once

#include <functional>

#ifdef ASFW_HOST_TEST
#include "HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>
#endif

namespace ASFW::Driver {

class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    void Bind(OSSharedPtr<IODispatchQueue> queue);

    void DispatchAsync(const std::function<void()>& work);
    void DispatchSync(const std::function<void()>& work);

    OSSharedPtr<IODispatchQueue> Queue() const { return queue_; }

private:
    OSSharedPtr<IODispatchQueue> queue_;
};

} // namespace ASFW::Driver
