// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MAudioInternalTxTiming.hpp -- pure 48 kHz TX timestamp policy for the
// M-Audio FireWire 1814 / ProjectMix special firmware.
//
// This is intentionally content- and transport-agnostic.  The caller feeds
// it an already-published OUTPUT_LAST completion timestamp and executes the
// resulting packet decision.  It owns no MMIO, DMA descriptor, IPC object, or
// PCM buffer.  The state alternatives are a std::variant so an unarmed or
// rejected geometry cannot accidentally emit a timestamped M-Audio packet.

#pragma once

#include "MAudioDuplexChoreography.hpp"

#include <cstdint>
#include <variant>

namespace ASFW::Audio::Families::BeBoB::MAudio {

inline constexpr uint32_t kInternalTxTimingRateHz = 48'000;
inline constexpr uint8_t kInternalTxSytInterval = 8;
inline constexpr uint32_t kInternalTxSytIncrementTicks = 4'096;
inline constexpr uint32_t kInternalTxLeadCycles = 683;
inline constexpr uint32_t kInternalTxReanchorExtraCycles = 4;

/// 683 is the vendor's own quantity -- SetupCIP @ 0x27102 computes it as the
/// number of cycles spanned by 4096 audio frames at the current rate, i.e. the
/// depth of its 4096-frame DCL ring (683 at 48 kHz, 743 at 44.1 kHz).
///
/// Do not "correct" the resulting SYT lead.  Measured on vendor captures, the
/// lead of the SYT over the packet's own transmit time is:
///
///   vendor host->device, 44.1 kHz   ~6.0 cycles
///   vendor host->device, 48 kHz     ~8.5 cycles
///   device->host,        48 kHz    ~12.3 cycles
///   ASFW   host->device, 48 kHz    ~11.8 cycles
///
/// It is rate-dependent, and the device itself transmits at ~12.3, so ASFW's
/// ~11.8 is an ordinary lead on this link rather than an error.  In particular
/// a lead past 8 is not ambiguous in practice: the vendor's own host already
/// exceeds the 16-cycle window's midpoint, so receivers here round up the way
/// Linux does (amdtp-stream.c:492) rather than treating the field as signed.

/// The vendor callback treats phase deltas 3, 4, and 5 as already aligned.
inline constexpr int32_t kInternalTxPhaseWindowStartCycles = 3;
inline constexpr int32_t kInternalTxPhaseWindowEndCycles = 6;

struct InternalTxTimingStopped final {
    StartEpoch epoch{};
};

struct InternalTxTimingRunning final {
    StartEpoch epoch{};
};

struct InternalTxTimingFailed final {
    StartEpoch epoch{};
};

using InternalTxTimingState = std::variant<InternalTxTimingStopped,
                                           InternalTxTimingRunning,
                                           InternalTxTimingFailed>;

/// A preview is immutable until CommitPacket() succeeds.  A caller may turn a
/// planned DATA packet into NO-DATA when the PCM snapshot is unavailable, which
/// prevents ASFW from fabricating DATA from placeholder PCM during the M-Audio
/// bootstrap.  The running SYT still advances in that case: it is a clock, so
/// it tracks elapsed time rather than bytes emitted.
struct InternalTxPacketPlan final {
    uint64_t sequence{0};
    bool isData{false};
    uint8_t dataBlocks{0};
    uint16_t syt{0xFFFF};
    uint32_t baseCycleTimer{0};
    uint32_t dataCycleTimer{0};
    uint8_t noDataAccumulatorAfter{0};
};

struct InternalTxPhaseObservation final {
    bool accepted{false};
    bool reanchored{false};
    uint64_t stampCount{0};
    uint32_t completionCycleTimer{0};
    int32_t phaseCycles{0};
    uint32_t runningCycleTimer{0};
};

/// Reproduces the M-Audio output callback's 48 kHz timing decisions:
///
///   * ResetPort(0) seeds the full running SYT at zero.
///   * Packet decisions repeat DATA, DATA, DATA, NO-DATA.
///   * A DATA decision advances the full SYT by 4096 ticks before it is
///     emitted, yielding the first zero-seeded SYT 0x1400.  4096 is not a
///     whole cycle: the offset carries at the 3072-tick modulus, so three DATA
///     packets advance the cycle field by 1, 1, then 2 -- exactly the four
///     cycles the DATA,DATA,DATA,NO-DATA group occupies.
///   * An actual TX completion re-anchors only when the phase is outside
///     [3, 6), to completion + 683 + 4 cycles while preserving the old
///     sub-cycle offset.
///
/// The cycle timestamp must originate from OUTPUT_LAST completion status, not
/// an RX DMA/raw receive timestamp and not a newly read cycle register.
class InternalTxTiming final {
public:
    InternalTxTiming() noexcept = default;

    [[nodiscard]] bool Arm(StartEpoch epoch,
                           uint32_t sampleRateHz,
                           uint8_t sytInterval) noexcept;
    void Disarm() noexcept;

    [[nodiscard]] bool IsArmed() const noexcept;
    [[nodiscard]] const InternalTxTimingState& State() const noexcept;
    [[nodiscard]] uint32_t RunningCycleTimer() const noexcept;

    [[nodiscard]] bool PreviewNextPacket(
        InternalTxPacketPlan& outPlan) const noexcept;
    [[nodiscard]] bool CommitPacket(const InternalTxPacketPlan& plan,
                                    bool emittedData) noexcept;

    [[nodiscard]] InternalTxPhaseObservation ObserveTxCompletion(
        uint64_t stampCount,
        uint32_t completionCycleTimer) noexcept;

private:
    [[nodiscard]] static StartEpoch StateEpoch(
        const InternalTxTimingState& state) noexcept;

    InternalTxTimingState state_{InternalTxTimingStopped{}};
    uint32_t runningCycleTimer_{0};
    uint8_t noDataAccumulator_{1};
    uint64_t nextSequence_{0};
    uint64_t lastCompletionStampCount_{0};
};

} // namespace ASFW::Audio::Families::BeBoB::MAudio
