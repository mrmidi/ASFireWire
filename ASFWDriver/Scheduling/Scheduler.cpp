#include "Scheduler.hpp"

#ifndef ASFW_HOST_TEST
#include <Block.h>
#include <DriverKit/IOLib.h>
#endif

namespace ASFW::Driver {

Scheduler::Scheduler() = default;
Scheduler::~Scheduler() = default;

void Scheduler::Bind(OSSharedPtr<IODispatchQueue> queue) {
    queue_ = std::move(queue);
}

void Scheduler::DispatchAsync(const std::function<void()>& work) {
    if (!work) {
        return;
    }
#ifdef ASFW_HOST_TEST
    work();
#else
    if (!queue_) {
        work();
        return;
    }

    auto task = work;
    queue_->DispatchAsync(^{ task(); });
#endif
}

void Scheduler::DispatchAsyncAfter(uint64_t delayNs, const std::function<void()>& work) {
    if (!work) {
        return;
    }
#ifdef ASFW_HOST_TEST
    if (!queue_) {
        work();
        return;
    }
    queue_->DispatchAsyncAfter(delayNs, work);
#else
    if (!queue_) {
        work();
        return;
    }

    const uint64_t delayMs = delayNs / 1'000'000ULL;
    const uint64_t trailingNs = delayNs % 1'000'000ULL;
    auto task = work;
    queue_->DispatchAsync(^{
      if (delayMs > 0) {
          IOSleep(delayMs);
      }
      if (trailingNs > 0) {
          IODelay((trailingNs + 999ULL) / 1000ULL);
      }
      task();
    });
#endif
}

void Scheduler::DispatchSync(const std::function<void()>& work) {
    if (!work) {
        return;
    }
#ifdef ASFW_HOST_TEST
    work();
#else
    if (!queue_) {
        work();
        return;
    }

    auto task = work;
    queue_->DispatchSync(^{ task(); });
#endif
}

} // namespace ASFW::Driver
