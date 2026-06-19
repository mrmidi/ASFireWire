#pragma once

#include "AmdtpTypes.hpp"

#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

class IAmdtpCadence {
public:
    virtual ~IAmdtpCadence() = default;

    virtual void Reset() noexcept = 0;

    virtual bool CurrentCycleIsData() const noexcept = 0;
    virtual uint8_t CurrentCycleDataFrames() const noexcept = 0;
    virtual uint64_t TotalCycles() const noexcept = 0;

    virtual void AdvanceCycle() noexcept = 0;
};

class Blocking48kCadence final : public IAmdtpCadence {
public:
    void Reset() noexcept override;

    bool CurrentCycleIsData() const noexcept override;
    uint8_t CurrentCycleDataFrames() const noexcept override;
    uint64_t TotalCycles() const noexcept override;

    void AdvanceCycle() noexcept override;

private:
    uint8_t phase_{0};
    uint64_t totalCycles_{0};
};

class NonBlocking48kCadence final : public IAmdtpCadence {
public:
    void Reset() noexcept override;

    bool CurrentCycleIsData() const noexcept override;
    uint8_t CurrentCycleDataFrames() const noexcept override;
    uint64_t TotalCycles() const noexcept override;

    void AdvanceCycle() noexcept override;

private:
    uint64_t totalCycles_{0};
};

} // namespace ASFW::Protocols::Audio::AMDTP
