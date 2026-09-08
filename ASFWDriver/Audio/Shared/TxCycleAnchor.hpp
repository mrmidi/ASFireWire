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

/// Half an eight-second window: the point at which the lift concludes it
/// picked the wrong one.
///
/// IsochTxDmaRing publishes clockPair at the top of a refill pass and pushes
/// that pass's completion stamps at the bottom, so a reader can pair a newer
/// pass's stamp with an older pass's correlation and see the completion lead by
/// the width of one pass -- microseconds. A harvested completion is only
/// milliseconds old either way, so the true separation is always far below half
/// a window and the nearest window is always the right one.
///
/// This must be applied in BOTH directions. OR-ing the stamp's low three bits
/// into the correlation's window is wrong whenever the two straddle an
/// eight-second boundary, and that happens in either order: a stamp behind the
/// boundary with a correlation past it lands one window HIGH, and a stamp past
/// the boundary with a correlation behind it -- the ordinary publish race, seen
/// on hardware as `[BackendTiming] noCycleAnchor` bursts -- lands one window
/// LOW. Correcting only the first left the second producing an anchor exactly
/// eight seconds early, which UnwrapBusTicks then refused against its
/// high-water mark, costing a burst of packets every time it happened.
inline constexpr int64_t kCompletionWindowHalfTicks =
    kCompletionStampDomainTicks / 2;

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
/// Lift an OHCI completion stamp -- which carries only cycleSeconds[2:0] --
/// into the full 128-second window implied by a correlated CYCLE_TIMER read,
/// choosing the NEAREST eight-second window. Both results are tick counts in
/// the same domain, and the completion may legitimately land slightly after the
/// correlation (see kCompletionWindowHalfTicks for why).
///
/// This is the single implementation of the lift. It previously existed twice,
/// with two different one-sided correction rules, which is how one of them
/// stayed wrong.
inline void LiftCompletionAgainstCorrelation(
    uint32_t completionCycleTimer,
    uint32_t correlationCycleTimer,
    int64_t& outCompletionTicks,
    int64_t& outCorrelationTicks) noexcept {
    const auto completion =
        ASFW::Timing::decodeCycleTimer(completionCycleTimer);
    const auto correlation =
        ASFW::Timing::decodeCycleTimer(correlationCycleTimer);

    outCorrelationTicks = ASFW::Timing::tstampToOffsets(
        correlation.seconds,
        correlation.cycle % ASFW::Timing::kCyclesPerSecond,
        correlation.offset);

    const uint32_t liftedSeconds =
        (correlation.seconds & ~0x7u) | (completion.seconds & 0x7u);
    int64_t completionTicks = ASFW::Timing::tstampToOffsets(
        liftedSeconds,
        completion.cycle % ASFW::Timing::kCyclesPerSecond,
        completion.offset);

    const int64_t lead = completionTicks - outCorrelationTicks;
    if (lead > kCompletionWindowHalfTicks) {
        completionTicks -= kCompletionStampDomainTicks;
    } else if (lead < -kCompletionWindowHalfTicks) {
        completionTicks += kCompletionStampDomainTicks;
    }
    outCompletionTicks = completionTicks;
}

/// Running reference for unwrapping the controller's correlation reads into a
/// monotone 64-bit bus timeline.
///
/// It holds CORRELATION times and nothing else. That is not incidental: the
/// unwrap has to decide which 128-second window a raw read belongs to, and it
/// can only do that against a previous sample of the same clock.
struct TxCorrelationUnwrapState final {
    bool valid{false};
    uint64_t lastCorrelationBusTicks{0};
};

/// Expand one transmit completion stamp and its correlated CYCLE_TIMER read
/// into monotone 64-bit bus times.
///
/// Ordering is deliberately NOT enforced here. Callers walk the completion
/// stamp queue in both directions -- the M-Audio path reads it newest-first to
/// find the freshest DATA stamp -- so completions legitimately arrive
/// descending, and a monotonic guard on them would reject almost everything.
/// HardwareSampleTimeline::Observe already rejects an out-of-order observation,
/// which is the layer that owns that question.
///
/// This used to keep one state slot and use it for both quantities: the guard
/// inside the unwrap compared this call's correlation against the previous
/// call's completion, because the caller overwrote the slot with the completion
/// afterwards. Two samples of two different clocks in one high-water mark. And
/// because the unwrap returns before updating on failure, any completion that
/// landed ahead of its correlation -- which the lift above explains is normal
/// -- left the mark in the future and wedged every later call until the bus
/// clock caught up. Measured on an M-Audio FireWire 1814: bursts of 1024, 4096
/// and finally 131,072 consecutive failures over 414 seconds, which starved the
/// zero-timestamp boundaries and left CoreAudio reanchoring on 14 of every 17
/// periods.
[[nodiscard]] inline bool ExpandCompletionAgainstCorrelation(
    TxCorrelationUnwrapState& state,
    uint32_t completionCycleTimer,
    uint32_t correlationCycleTimer,
    uint64_t& outCompletionBusTicks,
    uint64_t& outCorrelationBusTicks) noexcept {
    int64_t completionTicks = 0;
    int64_t correlationTicks = 0;
    LiftCompletionAgainstCorrelation(completionCycleTimer, correlationCycleTimer,
                                     completionTicks, correlationTicks);

    const uint64_t domain = static_cast<uint64_t>(kBusDomainTicks);
    uint64_t raw = static_cast<uint64_t>(correlationTicks) % domain;
    uint64_t unwrapped = raw;
    if (state.valid) {
        unwrapped = (state.lastCorrelationBusTicks / domain) * domain + raw;
        if (unwrapped + domain / 2 < state.lastCorrelationBusTicks) {
            unwrapped += domain;
        }
        // A correlation that still lands behind the mark is a controller read
        // out of order, which the caller counts. Leaving the mark untouched is
        // what keeps one such read from wedging the path: the next forward read
        // passes on its own.
        if (unwrapped < state.lastCorrelationBusTicks) return false;
    }

    // Signed on purpose: a completion may sit slightly after its correlation.
    const int64_t age = correlationTicks - completionTicks;
    const int64_t completionSigned = static_cast<int64_t>(unwrapped) - age;
    if (completionSigned < 0) return false;

    state.valid = true;
    state.lastCorrelationBusTicks = unwrapped;
    outCorrelationBusTicks = unwrapped;
    outCompletionBusTicks = static_cast<uint64_t>(completionSigned);
    return true;
}

[[nodiscard]] inline bool TransmitPacketBusTicks(
    uint32_t completionCycleTimer,
    uint32_t correlationCycleTimer,
    uint64_t packetDistance,
    int64_t& outTicks) noexcept {
    int64_t completionTicks = 0;
    int64_t correlationTicks = 0;
    LiftCompletionAgainstCorrelation(
        completionCycleTimer, correlationCycleTimer,
        completionTicks, correlationTicks);

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
