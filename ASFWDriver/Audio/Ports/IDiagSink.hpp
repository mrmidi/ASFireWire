#pragma once

#include <cstdint>

namespace ASFW::Ports {

class IDiagSink {
public:
    virtual ~IDiagSink() = default;

    virtual void Increment(uint32_t counterId, uint64_t delta) noexcept = 0;
};

} // namespace ASFW::Ports
