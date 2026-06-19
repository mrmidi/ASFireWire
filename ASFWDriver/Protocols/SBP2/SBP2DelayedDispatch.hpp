#pragma once

#include <cstdint>
#include <functional>

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#endif

namespace ASFW::Protocols::SBP2 {

inline void DispatchAfterCompat(IODispatchQueue* queue,
                                uint64_t delayNs,
                                std::function<void()> callback) noexcept {
    if (queue == nullptr || !callback) {
        return;
    }

#ifdef ASFW_HOST_TEST
    queue->DispatchAsyncAfter(delayNs, std::move(callback));
#else
    const uint64_t delayMs = delayNs / 1'000'000ULL;
    const uint64_t trailingNs = delayNs % 1'000'000ULL;
    auto work = std::move(callback);
    queue->DispatchAsync(^{
        if (delayMs > 0) {
            IOSleep(delayMs);
        }
        if (trailingNs > 0) {
            IODelay((trailingNs + 999ULL) / 1000ULL);
        }
        work();
    });
#endif
}

} // namespace ASFW::Protocols::SBP2
