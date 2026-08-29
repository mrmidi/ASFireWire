// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MAudioInternalTxTiming.hpp -- M-Audio TX-cycle anchoring for generic AMDTP
// blocking cadence.

#pragma once

#include "MAudioDuplexChoreography.hpp"

#include "../../../Wire/AMDTP/AmdtpCadence.hpp"
#include "../../../Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../../../../Common/TimingUtils.hpp"

#include <cstdint>
#include <variant>

namespace ASFW::Audio::Families::BeBoB::MAudio {

inline constexpr uint8_t kInternalTxSytInterval = 8;

/// IEC 61883-6 blocking transfer delay. Linux derives the same integer-tick
/// value in firewire/amdtp-stream.c:303-307: default device buffering plus one
/// SYT interval, which absorbs the cadence NO-DATA packets.
[[nodiscard]] constexpr uint32_t InternalTxTransferDelayTicks(
    const uint32_t sampleRateHz,
    const uint8_t sytInterval) noexcept {
    return sampleRateHz == 0
        ? 0
        : ASFW::Timing::kTransferDelayTicks - ASFW::Timing::kTicksPerCycle +
              static_cast<uint32_t>((uint64_t(ASFW::Timing::kTicksPerSecond) *
                                     sytInterval) /
                                    sampleRateHz);
}

static_assert(InternalTxTransferDelayTicks(48'000, 8) == 12'800,
              "48 kHz blocking transfer delay must match Linux's derivation");
static_assert(InternalTxTransferDelayTicks(96'000, 16) == 12'800);
static_assert(InternalTxTransferDelayTicks(192'000, 32) == 12'800);

/// Compose a 16-bit SYT from the generic cadence offset and the actual cycle
/// in which this OHCI packet will transmit. The M-Audio-specific part is the
/// latter: completion-derived cycle anchoring, not packet cadence arithmetic.
[[nodiscard]] constexpr uint16_t ComputeInternalTxSyt(
    const uint16_t sytOffsetTicks,
    const uint32_t transmitCycle,
    const uint32_t transferDelayTicks) noexcept {
    const uint32_t total = static_cast<uint32_t>(sytOffsetTicks) +
        transferDelayTicks;
    return static_cast<uint16_t>(
        (((transmitCycle + total / ASFW::Timing::kTicksPerCycle) & 0x0FU)
         << 12U) |
        (total % ASFW::Timing::kTicksPerCycle));
}

struct InternalTxTimingStopped final { StartEpoch epoch{}; };
struct InternalTxTimingRunning final { StartEpoch epoch{}; };
struct InternalTxTimingFailed final { StartEpoch epoch{}; };

using InternalTxTimingState = std::variant<InternalTxTimingStopped,
                                           InternalTxTimingRunning,
                                           InternalTxTimingFailed>;

/// A preview is immutable until CommitPacket() succeeds. A planned data packet
/// may be degraded to NO-DATA while CoreAudio has no complete PCM range, but
/// the generic cadence still advances once for that physical packet.
struct InternalTxPacketPlan final {
    uint64_t sequence{0};
    uint64_t cadenceCycle{0};
    bool isData{false};
    uint8_t dataBlocks{0};
    /// Offset within the packet's own bus cycle, supplied by the generic
    /// rational AMDTP cadence engine. NO-DATA uses kNoSytOffset.
    uint16_t sytOffsetTicks{
        ::ASFW::Protocols::Audio::AMDTP::kNoSytOffset};
};

/// Retains only the special firmware's completion-cycle anchoring. Packet
/// cadence and NO-DATA placement are owned by RationalBlockingCadence, the
/// shared IEC 61883-6 AMDTP implementation. V3 accepts only 48/96/192 kHz.
class InternalTxTiming final {
public:
    InternalTxTiming() noexcept = default;

    [[nodiscard]] bool Arm(StartEpoch epoch, uint32_t sampleRateHz,
                           uint8_t sytInterval) noexcept;
    void Disarm() noexcept;

    [[nodiscard]] bool IsArmed() const noexcept;
    [[nodiscard]] const InternalTxTimingState& State() const noexcept;
    [[nodiscard]] uint32_t TransferDelayTicks() const noexcept;
    [[nodiscard]] bool PreviewNextPacket(InternalTxPacketPlan& outPlan) const noexcept;
    [[nodiscard]] bool CommitPacket(const InternalTxPacketPlan& plan,
                                    bool emittedData) noexcept;

private:
    [[nodiscard]] static StartEpoch StateEpoch(const InternalTxTimingState& state) noexcept;

    InternalTxTimingState state_{InternalTxTimingStopped{}};
    ::ASFW::Protocols::Audio::AMDTP::RationalBlockingCadence cadence_{};
    uint32_t transferDelayTicks_{0};
    uint64_t nextSequence_{0};
};

} // namespace ASFW::Audio::Families::BeBoB::MAudio
