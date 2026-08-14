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

#include "../../../../Common/TimingUtils.hpp"

#include <cstdint>
#include <variant>

namespace ASFW::Audio::Families::BeBoB::MAudio {

inline constexpr uint32_t kInternalTxTimingRateHz = 48'000;
inline constexpr uint8_t kInternalTxSytInterval = 8;
inline constexpr uint32_t kInternalTxSytIncrementTicks = 4'096;

/// IEC 61883-6 transfer delay, derived exactly as Linux does
/// (amdtp-stream.c:303-307): the device's default input buffering
/// (TRANSFER_DELAY_TICKS - TICKS_PER_CYCLE) plus, for CIP_BLOCKING, the extra
/// buffering that absorbs the cadence NO-DATA packets
/// (TICKS_PER_SECOND * syt_interval / rate).  12800 ticks at 48 kHz, so a SYT
/// leads its own packet's transmit cycle by 4.17 to 5.17 cycles.
///
/// Anchoring SYT to the transmit cycle is the point of this constant.  A
/// previous revision ran a free-running SYT accumulator and reconciled it
/// against `completion + 683 + 4` cycles, where 683 is the *vendor's* DCL ring
/// depth (4096 frames at 48 kHz, SetupCIP @ 0x27102) and not ours.  Measured on
/// a bus capture, our transmit pipeline is ~676 cycles deep, so the borrowed
/// constant overshot by ~7 cycles and every SYT went out 12.00 to 12.67 cycles
/// ahead -- stable, no drift, so not a clock error but a fixed offset.  The SYT
/// cycle field is 4 bits: that left ~3 cycles of headroom in a 16-cycle window
/// before the timestamp aliases to "due now", and the re-anchor itself supplied
/// the jitter to cross it.  For reference the vendor measures ~8.5 cycles and
/// Linux 4.17 to 5.17.
inline constexpr uint32_t kInternalTxTransferDelayTicks =
    ASFW::Timing::kTransferDelayTicks - ASFW::Timing::kTicksPerCycle +
    kInternalTxSytIncrementTicks;

static_assert(kInternalTxTransferDelayTicks == 12'800,
              "48 kHz blocking transfer delay must match Linux's derivation");

/// Phase the cadence NO-DATA packet holds, and so the value Arm seeds in order
/// that a group's first DATA packet emits phase 0.
///
/// The phase advances 1024 modulo 3072 per DATA packet while the transmit cycle
/// advances 1, so the three DATA packets of a group must emit 0, 1024, 2048 and
/// the 2048 -> 0 wrap must fall across the NO-DATA.  Only that two-cycle gap
/// makes the wrap worth 4096 ticks like every other step; between two
/// consecutive DATA packets the same wrap is worth 1024, which would understate
/// the stream's rate by a factor of four for one packet in every group.
///
/// This invariant is specific to anchoring SYT on the physical transmit cycle.
/// The superseded free-accumulator model could seed any phase, because its
/// cycle field came from the same accumulator as its offset and so absorbed the
/// wrap automatically.
inline constexpr uint32_t kInternalTxSytPhaseSeedTicks = 2'048;

static_assert((kInternalTxSytPhaseSeedTicks + kInternalTxSytIncrementTicks) %
                      ASFW::Timing::kTicksPerCycle ==
                  0,
              "seed must make the first DATA packet of a group emit phase 0");

/// Build a packet's 16-bit SYT from its own transmit cycle, mirroring Linux
/// compute_syt (amdtp-stream.c:1019-1028).  `sytPhaseTicks` is the sub-cycle
/// phase carried by InternalTxTiming, always in [0, 3072).
[[nodiscard]] constexpr uint16_t ComputeInternalTxSyt(
    const uint32_t sytPhaseTicks, const uint32_t transmitCycle) noexcept {
    const uint32_t total = sytPhaseTicks + kInternalTxTransferDelayTicks;
    return static_cast<uint16_t>(
        (((transmitCycle + total / ASFW::Timing::kTicksPerCycle) & 0x0FU)
         << 12U) |
        (total % ASFW::Timing::kTicksPerCycle));
}

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
    /// Sub-cycle phase before this packet, in [0, 3072).  CommitPacket rejects
    /// a plan whose base no longer matches, so a stale preview cannot advance
    /// the phase twice.
    uint32_t basePhaseTicks{0};
    /// Phase this packet's SYT carries, in [0, 3072).  Equal to basePhaseTicks
    /// for a cadence NO-DATA packet, which emits no timestamp at all.
    uint32_t sytPhaseTicks{0};
    uint8_t noDataAccumulatorAfter{0};
};

/// Owns the M-Audio output callback's 48 kHz cadence and SYT phase:
///
///   * Packet decisions repeat DATA, DATA, DATA, NO-DATA.
///   * A DATA decision advances the phase by 4096 ticks modulo 3072, so
///     successive DATA packets carry 0, 1024, 2048 and the wrap back to 0
///     falls across the cadence NO-DATA -- the two-cycle gap that keeps the
///     inter-packet SYT delta at exactly 4096 ticks, i.e. real time.
///     Linux generates the same sequence in calculate_syt_offset
///     (amdtp-stream.c:425-461) from a 1024-tick state.
///
/// It deliberately does *not* know where in bus time a packet will transmit.
/// The caller pairs the phase with that packet's own transmit cycle through
/// ComputeInternalTxSyt, exactly as Linux pairs seq->syt_offset with
/// desc->cycle (amdtp-stream.c:1049).  That keeps the SYT lead pinned to the
/// transfer delay instead of to a pipeline depth this driver would have to
/// guess.
class InternalTxTiming final {
public:
    InternalTxTiming() noexcept = default;

    [[nodiscard]] bool Arm(StartEpoch epoch,
                           uint32_t sampleRateHz,
                           uint8_t sytInterval) noexcept;
    void Disarm() noexcept;

    [[nodiscard]] bool IsArmed() const noexcept;
    [[nodiscard]] const InternalTxTimingState& State() const noexcept;
    [[nodiscard]] uint32_t SytPhaseTicks() const noexcept;

    [[nodiscard]] bool PreviewNextPacket(
        InternalTxPacketPlan& outPlan) const noexcept;
    [[nodiscard]] bool CommitPacket(const InternalTxPacketPlan& plan,
                                    bool emittedData) noexcept;

private:
    [[nodiscard]] static StartEpoch StateEpoch(
        const InternalTxTimingState& state) noexcept;

    InternalTxTimingState state_{InternalTxTimingStopped{}};
    uint32_t sytPhaseTicks_{0};
    uint8_t noDataAccumulator_{1};
    uint64_t nextSequence_{0};
};

} // namespace ASFW::Audio::Families::BeBoB::MAudio
