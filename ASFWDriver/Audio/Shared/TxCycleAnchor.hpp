// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../Common/TimingUtils.hpp"

#include <cstdint>

namespace ASFW::Audio::Shared {

/// The FireWire cycle timer's full 128-second period, in 24.576 MHz ticks.
/// Every bus time that crosses the audio timeline -- RX presentation
/// observations and TX presentation plans alike -- lives in this domain.
inline constexpr int64_t kBusDomainTicks =
    static_cast<int64_t>(ASFW::Timing::kFWTimeWrapSeconds) *
    static_cast<int64_t>(ASFW::Timing::kTicksPerSecond);

/// The eight-second period an OHCI transmit completion stamp is ambiguous in.
inline constexpr int64_t kCompletionStampDomainTicks =
    8 * static_cast<int64_t>(ASFW::Timing::kTicksPerSecond);

/// Lift an OHCI transmit completion stamp into the controller's full
/// 128-second cycle-timer domain, then project it forward by `packetDistance`
/// isochronous cycles to give the transmit time of a later packet.
///
/// An OUTPUT_LAST status timestamp carries only cycleSeconds[2:0] and
/// cycleCount[12:0] (`IsochTxDmaRing` reconstructs it at offset zero), so on
/// its own it is ambiguous modulo eight seconds. `correlationCycleTimer` is a
/// full CYCLE_TIMER read taken by the same refill pass that harvested the
/// stamp, so the completion is never later than the correlation -- which is
/// what resolves the window. Cross-validated with the same lift Linux applies
/// to OUTPUT_LAST timestamps in firewire-ohci.
///
/// The result must stay in the 128-second domain: collapsing it to eight
/// seconds makes a TX plan incomparable with the hardware timeline's RX
/// observations and breaks any unwrapper keyed to the 128-second wrap.
[[nodiscard]] inline bool TransmitPacketBusTicks(
    uint32_t completionCycleTimer,
    uint32_t correlationCycleTimer,
    uint64_t packetDistance,
    int64_t& outTicks) noexcept {
    const auto completion =
        ASFW::Timing::decodeCycleTimer(completionCycleTimer);
    const auto correlation =
        ASFW::Timing::decodeCycleTimer(correlationCycleTimer);

    const int64_t correlationTicks = ASFW::Timing::tstampToOffsets(
        correlation.seconds,
        correlation.cycle % ASFW::Timing::kCyclesPerSecond,
        correlation.offset);

    const uint32_t liftedSeconds =
        (correlation.seconds & ~0x7u) | (completion.seconds & 0x7u);
    int64_t completionTicks = ASFW::Timing::tstampToOffsets(
        liftedSeconds,
        completion.cycle % ASFW::Timing::kCyclesPerSecond,
        completion.offset);
    if (completionTicks > correlationTicks) {
        completionTicks -= kCompletionStampDomainTicks;
    }

    int64_t ticks = completionTicks +
        static_cast<int64_t>(packetDistance) *
            static_cast<int64_t>(ASFW::Timing::kTicksPerCycle);
    ticks %= kBusDomainTicks;
    if (ticks < 0) {
        ticks += kBusDomainTicks;
    }
    outTicks = ticks;
    return true;
}

} // namespace ASFW::Audio::Shared
