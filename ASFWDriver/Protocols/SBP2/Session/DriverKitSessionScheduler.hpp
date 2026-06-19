#pragma once

#include "ISessionScheduler.hpp"

#ifdef ASFW_HOST_TEST
#include "../../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include <functional>
#include <map>
#include <vector>

class ASFWDriver;

namespace ASFW::Protocols::SBP2 {

// Production implementation of ISessionScheduler. A single DriverKit timer
// source wakes the next due SBP-2 session callback; callbacks then run on the
// driver's Default queue.
class DriverKitSessionScheduler final : public ISessionScheduler {
public:
    DriverKitSessionScheduler();
    ~DriverKitSessionScheduler() override;

    DriverKitSessionScheduler(const DriverKitSessionScheduler&) = delete;
    DriverKitSessionScheduler& operator=(const DriverKitSessionScheduler&) = delete;

    [[nodiscard]] kern_return_t Prepare(::ASFWDriver& service,
                                        OSSharedPtr<IODispatchQueue> workQueue);
    void Reset() noexcept;

    [[nodiscard]] SchedulerToken ScheduleAfter(uint64_t delayNs,
                                               std::function<void()> fn) override;
    void Cancel(SchedulerToken token) override;

    void HandleTimerFired() noexcept;

private:
    struct PendingCallback {
        uint64_t deadlineTicks{0};
        std::function<void()> fn;
    };

    void ArmNextLocked() noexcept;
    [[nodiscard]] uint64_t DeadlineTicksFromNow(uint64_t delayNs) const noexcept;

    IOLock* lock_{nullptr};
    OSSharedPtr<IODispatchQueue> workQueue_{};
    OSSharedPtr<IOTimerDispatchSource> timer_{};
    OSSharedPtr<OSAction> action_{};
    std::map<SchedulerToken, PendingCallback> pending_;
    SchedulerToken nextToken_{1};
};

} // namespace ASFW::Protocols::SBP2
