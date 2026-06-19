#pragma once

#include <cstdint>

namespace ASFW::Ports {

// RT-safe diagnostics sink (see README, Architecture / Ports).
//
// Instruments report through this seam instead of logging: counter ids are
// small stable integers owned by the reporting instrument, increments must be
// safe on a real-time thread (no allocation, no locks, no IO). Production
// binds this to whatever diagnostics plumbing exists there; the lab binds it
// to Lab::StickyCounterSink and dumps at StopIO or from test code.
class IDiagSink {
public:
    virtual ~IDiagSink() = default;

    virtual void Increment(uint32_t counterId, uint64_t delta) noexcept = 0;
};

} // namespace ASFW::Ports
