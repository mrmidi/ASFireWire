#pragma once

#include <functional>
#include <new> // libc++ placement-new declaration must precede DriverKit.

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/OSAction.h>
#endif
#include "../Logging/Logging.hpp"

namespace ASFW::Common {

// Consumes one owned reference. Call after the dispatch source has quiesced.
// OSAction.iig: the target is retained until the ACTION is cancelled or freed;
// releasing our reference alone relies on all remote references disappearing.
// label must have static lifetime (the callers pass string literals).
inline void CancelAndReleaseOwnedAction(OSAction* action, const char* label, std::function<void()> done = {}) {
    if (!action) {
        if (done) done();
        return;
    }
    ASFW_LOG(Controller, "[Teardown] action cancel requested owner=%{public}s action=%p", label, action);
    const auto kr = action->Cancel(^{
        ASFW_LOG(Controller, "[Teardown] action cancel completed owner=%{public}s action=%p", label, action);
        action->release();
        if (done) done();
    });
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Controller, "[Teardown] action cancel failed owner=%{public}s action=%p kr=0x%x", label, action, kr);
        action->release();
        if (done) done();
    }
}

} // namespace ASFW::Common
