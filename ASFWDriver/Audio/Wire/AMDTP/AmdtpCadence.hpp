#pragma once

#include "AmdtpTypes.hpp"

#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

inline constexpr uint16_t kNoSytOffset = 0xFFFF;

/// One blocking-mode packet decision. A no-data packet has zero data blocks
/// and no SYT offset.
struct RationalBlockingDecision final {
    bool isData{false};
    uint8_t dataBlocks{0};
    uint16_t sytOffsetTicks{kNoSytOffset};
};

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

/// Exact rational blocking-mode cadence, kept in FireWire ticks x sample rate.
///
/// This is deliberately a standalone Phase 2 primitive. The production
/// packetizer remains 48 kHz-only until its rate-selection and presentation
/// timing paths are converted together. `initialDeadlineSubticks` is relative
/// to the start of the first cycle; wire-visible seed/lead policy belongs to
/// that later integration, not to this arithmetic engine.
class RationalBlockingCadence final {
public:
    /// Configures a schedule with at most one data packet per bus cycle.
    /// Returns false for an invalid rate/interval or an event rate over 8 kHz.
    [[nodiscard]] bool Configure(uint32_t sampleRateHz,
                                 uint8_t sytInterval,
                                 uint64_t initialDeadlineSubticks = 0) noexcept;

    void Reset() noexcept;

    [[nodiscard]] bool IsConfigured() const noexcept;
    [[nodiscard]] RationalBlockingDecision CurrentDecision() const noexcept;
    [[nodiscard]] uint64_t TotalCycles() const noexcept;

    void AdvanceCycle() noexcept;

private:
    uint32_t denominator_{0};
    uint8_t sytInterval_{0};
    uint64_t cycleSpanSubticks_{0};
    uint64_t stepSubticks_{0};
    uint64_t initialDeadlineSubticks_{0};
    uint64_t deadlineSubticks_{0};
    uint64_t totalCycles_{0};
};

} // namespace ASFW::Protocols::Audio::AMDTP
