#include "TestHooks.hpp"

#include <chrono>
#include <utility>

namespace ASFW::Driver {

uint64_t SteadyTestClock::Now() const {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void InterruptTestHook::Install(Handler handler) {
    handler_ = std::move(handler);
}

void InterruptTestHook::Trigger() {
    if (handler_) {
        handler_();
    }
}

} // namespace ASFW::Driver

