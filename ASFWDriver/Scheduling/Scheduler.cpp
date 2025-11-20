#include "Scheduler.hpp"

#ifndef ASFW_HOST_TEST
#include <Block.h>
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
