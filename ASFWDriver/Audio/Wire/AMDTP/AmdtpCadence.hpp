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

/// Exact rational blocking-mode cadence, kept in FireWire ticks x sample rate.
///
/// The single blocking-cadence arithmetic engine: one integer deadline DDA
/// parameterized by (sampleRate, sytInterval), validated against the golden
/// vectors from tools/amdtp_blocking_cadence_sim.py. Production reaches it
/// through the BlockingCadence adapter below; `initialDeadlineSubticks` is
/// relative to the start of the first cycle and is a wire-visible seed policy
/// owned by the adapter (or a future capture-derived policy), not by this
/// arithmetic.
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

// Blocking-mode AMDTP cadence for any 1x/2x/4x rate: the IAmdtpCadence
// adapter over RationalBlockingCadence used by the production packetizer.
// Defaults to 48 kHz so default-constructed callers behave like the old
// Blocking48kCadence; Configure() switches the rate.
//
// SEED POLICY (wire-visible): the engine is seeded one subtick-cycle short of
// one full data block,
//     seed = sytInterval * kTicksPerSecond - kTicksPerCycle,
// which makes the deadline DDA emit a block exactly when a full block of
// frames is pending -- provably identical, cycle for cycle at every rate, to
// the FFADO-style rational accumulator this class previously implemented
// (cross-validated with libffado-2.4.9 src/libstreaming/util/cip.c:130-160:
// data iff ready_samples + samples_per_cycle >= syt_interval). That is the
// hardware-verified startup phase (48/44.1k duplex), so the adapter preserves
// it. Apple's captured AppleFWAudio seed differs (opens N,N,N,D --
// documentation/44100.md section 7); adopting a different wire-visible seed
// is capture-gated (SAMPLE_RATE_EXPANSION.md section 8). In live duplex the
// free-running cadence is superseded by RX sequence replay after RX locks,
// so the seed governs only the pre-replay window.
class BlockingCadence final : public IAmdtpCadence {
public:
    BlockingCadence() noexcept { (void)Configure(48000, 8); }

    // sytIntervalFrames is the blocking frames-per-data-packet: 8 @1x
    // (32/44.1/48 k), 16 @2x, 32 @4x (FFADO getSytInterval()). Returns false
    // (and goes inert: every cycle no-data) for an invalid rate/interval
    // combination, e.g. more than sytInterval frames per cycle.
    [[nodiscard]] bool Configure(uint32_t sampleRateHz,
                                 uint8_t sytIntervalFrames) noexcept;

    void Reset() noexcept override;

    bool CurrentCycleIsData() const noexcept override;
    uint8_t CurrentCycleDataFrames() const noexcept override;
    uint64_t TotalCycles() const noexcept override;

    void AdvanceCycle() noexcept override;

private:
    RationalBlockingCadence engine_{};
};

// Transitional alias: existing callers default-construct this for 48 kHz.
using Blocking48kCadence = BlockingCadence;

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
