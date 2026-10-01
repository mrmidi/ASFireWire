// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// InterruptDrain.hpp - Service the controller until its interrupt deasserts.
//
// The controller interrupt is MSI (DriverContext::ConfigureInterrupts), and an
// MSI is one message per assertion of the controller's interrupt output, which
// is IntEvent & IntMask with masterIntEnable set (OHCI 1.1 §6.1). An event that
// lands while the output is still asserted, after the handler read IntEvent and
// before it acknowledged what it read, sends no message of its own. If the
// handler returned then, that event would keep the output asserted with no
// message coming, and the driver would never take another interrupt (seen
// 2026-10-01: a Duet joined the bus, the controller moved to the next
// generation, and the driver never heard it). So the handler re-reads IntEvent
// after each pass and runs again until no enabled event is pending; an event
// after that last read finds the output deasserted and raises a new message.

#pragma once

#include <cstdint>

namespace ASFW::Driver {

/// Passes before giving up. Each pass acknowledges what it read, so a second
/// pass only sees events that arrived during the first; needing more than a
/// few means an event that will not clear.
inline constexpr int kMaxInterruptPasses = 8;

/// Runs `handle` on `capture()` snapshots until `capture().intEvent & enabledMask()`
/// is zero. Returns the enabled events still pending when it gives up, or zero.
template <typename Capture, typename EnabledMask, typename Handle>
[[nodiscard]] uint32_t ServiceUntilDeasserted(Capture&& capture, EnabledMask&& enabledMask,
                                              Handle&& handle) {
    auto snapshot = capture();
    for (int pass = 0; pass < kMaxInterruptPasses; ++pass) {
        handle(snapshot);
        snapshot = capture();
        if ((snapshot.intEvent & enabledMask()) == 0U) {
            return 0U;
        }
    }
    return snapshot.intEvent & enabledMask();
}

} // namespace ASFW::Driver
