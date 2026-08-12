// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBBootloaderPreparation.hpp — the cue state machine, as a pure decision
// function over a std::variant.
//
// The machine owns no transport. It consumes events and returns the next action
// for a caller to perform, so every rule below is host-testable without a bus
// and without hardware that a mistake would cost a power cycle.
//
// Two safety rules are expressed as type invariants rather than checks:
//
//   * A cue's protocol version cannot be stale or defaulted. SendCue is
//     reachable only from EvaluatingInfo, the sole state that owns a
//     BootRomInfo, and BeBoBBootloaderCue can only be built from one.
//   * "Cue already sent" is not a bool. AwaitingResponse *is* the post-cue
//     state, so a timeout handler cannot re-send by misreading a flag —
//     re-sending would require constructing EvaluatingInfo again, which
//     requires a fresh info-block read. That is the intended recovery.

#pragma once

#include "BeBoBBootloaderCue.hpp"

#include <cstdint>
#include <variant>

namespace ASFW::Audio::Families::BeBoB::Bootloader {

/// Reads may be retried; see RetryBudget below.
inline constexpr uint8_t kMaxInfoReadAttempts = 3;
inline constexpr uint8_t kMaxResponseReadAttempts = 3;

/// The vendor driver waits 2 s between response reads. The bootloader answers on
/// its own schedule and this path is cold, so the generous interval is kept.
inline constexpr uint32_t kResponseRetryDelayMs = 2000;

// --- States -----------------------------------------------------------------

/// Reading the BootROM info block. Carries only its own retry budget.
struct ReadingInfo final {
    uint8_t attempt{0};
};

/// The info block is in hand. This is the only state that owns one, and
/// therefore the only state a cue can be built from.
struct EvaluatingInfo final {
    BootRomInfo info{};
};

/// The cue has been written. Reaching this state *is* the record that it was
/// sent; there is no separate flag.
struct AwaitingResponse final {
    uint32_t cuedProtocolVersion{0};
    uint8_t attempt{0};
};

enum class RetireReason : uint8_t {
    /// Bootloader was not active — the device is already running firmware.
    NoCueNeeded,
    /// Cue written and acknowledged; the device is expected to reset.
    Cued,
    /// Info block could not be read within its retry budget.
    InfoUnavailable,
    /// The write itself failed outright, so the cue did not land.
    CueWriteFailed,
    /// Cue was written but no response arrived within the retry budget. The
    /// device may still be booting; this is not proof the cue failed.
    ResponseTimeout,
    /// Bus generation moved under us. Nothing is inferred; the machine restarts
    /// from discovery on the next generation.
    GenerationChanged,
};

struct Retired final {
    RetireReason reason{RetireReason::NoCueNeeded};
};

using PreparationState =
    std::variant<ReadingInfo, EvaluatingInfo, AwaitingResponse, Retired>;

// --- Events -----------------------------------------------------------------

struct InfoReadSucceeded final {
    BootRomInfo info{};
};
struct InfoReadFailed final {};
struct CueWriteSucceeded final {};
struct CueWriteFailed final {};
struct ResponseReadSucceeded final {};
struct ResponseReadFailed final {};
struct GenerationInvalidated final {};

using PreparationEvent =
    std::variant<InfoReadSucceeded, InfoReadFailed, CueWriteSucceeded,
                 CueWriteFailed, ResponseReadSucceeded, ResponseReadFailed,
                 GenerationInvalidated>;

// --- Actions ----------------------------------------------------------------

struct ReadInfoBlock final {};

/// Carries the fully formed cue. The caller writes exactly these bytes and
/// nothing else; IsPermittedBootloaderWrite gates the write regardless.
struct WriteCue final {
    BeBoBBootloaderCue cue;
};

struct ReadResponse final {
    uint32_t delayMs{0};
};

struct Done final {};

using PreparationAction =
    std::variant<ReadInfoBlock, WriteCue, ReadResponse, Done>;

struct PreparationStep final {
    PreparationState state;
    PreparationAction action;
};

/// Entry point: a device whose catalog plan carries a cue policy starts here.
[[nodiscard]] PreparationStep BeginPreparation() noexcept;

/// The whole machine. Total over (state, event); an event that cannot occur in
/// a state leaves that state unchanged rather than asserting, because events
/// can race a generation change.
[[nodiscard]] PreparationStep AdvancePreparation(
    const PreparationState& state, const PreparationEvent& event) noexcept;

[[nodiscard]] const char* RetireReasonName(RetireReason reason) noexcept;

/// True once the machine has stopped. Preparation is deliberately terminal: it
/// never becomes an audio endpoint. A cued device returns as a different
/// identity in a later generation and is resolved from scratch.
[[nodiscard]] inline bool IsRetired(const PreparationState& state) noexcept {
    return std::holds_alternative<Retired>(state);
}

} // namespace ASFW::Audio::Families::BeBoB::Bootloader
