#pragma once

#include <cstdint>

namespace ASFW::Ports {

class ICycleTimeline {
public:
    virtual ~ICycleTimeline() = default;

    // Monotonically increasing, unwrapped bus time in 24.576 MHz ticks.
    virtual int64_t NowTicks() const noexcept = 0;
};

} // namespace ASFW::Ports
