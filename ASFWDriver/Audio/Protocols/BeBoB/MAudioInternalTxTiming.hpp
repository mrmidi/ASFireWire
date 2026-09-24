// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// M-Audio special-firmware 48 kHz TX cadence. Timing is anchored to the
// packet's own transmit cycle; it deliberately does not depend on RX SYT.

#pragma once

#include "../../Wire/AMDTP/AmdtpCadence.hpp"
#include "../../../Common/TimingUtils.hpp"

#include <cstdint>

namespace ASFW::Audio::BeBoB {

inline constexpr uint32_t kMAudioInternalTxSampleRateHz = 48'000;
inline constexpr uint8_t kMAudioInternalTxFramesPerCycle = 6;
inline constexpr uint8_t kMAudioInternalTxSytInterval = 8;

/// Blocking 48 kHz TX schedule used by M-Audio's special firmware.
/// Cadence starts at cycle zero: three 8-block DATA packets followed by one
/// NO-DATA packet, repeating. A DATA packet's SYT is composed later from its
/// sub-cycle offset plus the actual OHCI transmit-cycle anchor.
class MAudioInternalTxTiming final {
public:
    struct PacketPlan final {
        uint64_t sequence{0};
        uint64_t cadenceCycle{0};
        bool isData{false};
        uint8_t dataBlocks{0};
        uint16_t sytOffsetTicks{Protocols::Audio::AMDTP::kNoSytOffset};
    };

    [[nodiscard]] bool Arm() noexcept;
    void Disarm() noexcept;
    [[nodiscard]] bool IsArmed() const noexcept;
    [[nodiscard]] uint32_t TransferDelayTicks() const noexcept;
    [[nodiscard]] bool PreviewNextPacket(PacketPlan& outPlan) const noexcept;

    /// Commits exactly the current plan. DATA may be degraded to NO-DATA when
    /// the PCM producer is not ready; cadence still advances for that bus cycle.
    [[nodiscard]] bool CommitPacket(const PacketPlan& plan,
                                    bool emittedData) noexcept;

private:
    Protocols::Audio::AMDTP::RationalBlockingCadence cadence_{};
    uint64_t nextSequence_{0};
    bool armed_{false};
};

/// Place an M-Audio internal-clock SYT in the 16-bit CIP field, anchored to the
/// physical cycle in which this packet will be transmitted.
[[nodiscard]] constexpr uint16_t MAudioInternalTxSyt(
    uint16_t sytOffsetTicks,
    uint32_t transmitCycle,
    uint32_t transferDelayTicks) noexcept {
    const uint32_t total = static_cast<uint32_t>(sytOffsetTicks) +
                           transferDelayTicks;
    return static_cast<uint16_t>(
        (((transmitCycle + total / ASFW::Timing::kTicksPerCycle) & 0x0FU) << 12U) |
        (total % ASFW::Timing::kTicksPerCycle));
}

} // namespace ASFW::Audio::BeBoB
