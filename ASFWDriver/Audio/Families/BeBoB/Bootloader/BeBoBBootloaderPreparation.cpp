// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "BeBoBBootloaderPreparation.hpp"

namespace ASFW::Audio::Families::BeBoB::Bootloader {

namespace {

[[nodiscard]] PreparationStep Retire(RetireReason reason) noexcept {
    return PreparationStep{Retired{reason}, Done{}};
}

[[nodiscard]] PreparationStep Stay(const PreparationState& state) noexcept {
    // An event that cannot occur in this state. Racing a generation change can
    // produce one, so this is not a programming error; hold and wait.
    return PreparationStep{state, Done{}};
}

// --- Per-state handlers -----------------------------------------------------

[[nodiscard]] PreparationStep OnReadingInfo(const ReadingInfo& state,
                                            const PreparationEvent& event) noexcept {
    if (const auto* ok = std::get_if<InfoReadSucceeded>(&event)) {
        // Nothing is decided here. The next step reads the flag from the block
        // we actually received.
        return PreparationStep{EvaluatingInfo{ok->info}, ReadInfoBlock{}};
    }
    if (std::holds_alternative<InfoReadFailed>(event)) {
        const uint8_t next = static_cast<uint8_t>(state.attempt + 1);
        if (next >= kMaxInfoReadAttempts) {
            return Retire(RetireReason::InfoUnavailable);
        }
        return PreparationStep{ReadingInfo{next}, ReadInfoBlock{}};
    }
    return Stay(state);
}

[[nodiscard]] PreparationStep OnEvaluatingInfo(const EvaluatingInfo& state) noexcept {
    if (!state.info.BootloaderActive()) {
        // The application firmware is already running. This is the common case
        // on a healthy device and must send nothing at all.
        return Retire(RetireReason::NoCueNeeded);
    }
    // The one write. Built from this state's own info block, so its protocol
    // version cannot be stale.
    return PreparationStep{
        AwaitingResponse{state.info.ProtocolVersion(), 0},
        WriteCue{BeBoBBootloaderCue{state.info}}};
}

[[nodiscard]] PreparationStep OnAwaitingResponse(
    const AwaitingResponse& state, const PreparationEvent& event) noexcept {
    if (std::holds_alternative<CueWriteSucceeded>(event)) {
        return PreparationStep{state, ReadResponse{kResponseRetryDelayMs}};
    }
    if (std::holds_alternative<CueWriteFailed>(event)) {
        // The write did not land, so nothing was started and there is nothing
        // to wait for. Note this is NOT the timeout case: a timed-out write may
        // have landed, and the transport reports that as a failed read later
        // rather than as CueWriteFailed. See the header.
        return Retire(RetireReason::CueWriteFailed);
    }
    if (std::holds_alternative<ResponseReadSucceeded>(event)) {
        return Retire(RetireReason::Cued);
    }
    if (std::holds_alternative<ResponseReadFailed>(event)) {
        const uint8_t next = static_cast<uint8_t>(state.attempt + 1);
        if (next >= kMaxResponseReadAttempts) {
            // Retire without re-sending. The device may be mid-boot; the cue is
            // never written twice for one bootloader-active observation.
            return Retire(RetireReason::ResponseTimeout);
        }
        return PreparationStep{
            AwaitingResponse{state.cuedProtocolVersion, next},
            ReadResponse{kResponseRetryDelayMs}};
    }
    return Stay(state);
}

} // namespace

PreparationStep BeginPreparation() noexcept {
    return PreparationStep{ReadingInfo{0}, ReadInfoBlock{}};
}

PreparationStep AdvancePreparation(const PreparationState& state,
                                   const PreparationEvent& event) noexcept {
    // A generation change invalidates every route this machine could act on.
    // Checked before the per-state handlers so no state can ignore it, and in
    // particular so no state can respond to it by writing.
    if (std::holds_alternative<GenerationInvalidated>(event)) {
        if (std::holds_alternative<Retired>(state)) {
            return Stay(state);
        }
        return Retire(RetireReason::GenerationChanged);
    }

    if (const auto* reading = std::get_if<ReadingInfo>(&state)) {
        return OnReadingInfo(*reading, event);
    }
    if (const auto* evaluating = std::get_if<EvaluatingInfo>(&state)) {
        // EvaluatingInfo is transient: it decides from data already in hand and
        // consumes no event.
        return OnEvaluatingInfo(*evaluating);
    }
    if (const auto* awaiting = std::get_if<AwaitingResponse>(&state)) {
        return OnAwaitingResponse(*awaiting, event);
    }
    return Stay(state);
}

const char* RetireReasonName(RetireReason reason) noexcept {
    switch (reason) {
        case RetireReason::NoCueNeeded: return "no-cue-needed";
        case RetireReason::Cued: return "cued";
        case RetireReason::InfoUnavailable: return "info-unavailable";
        case RetireReason::CueWriteFailed: return "cue-write-failed";
        case RetireReason::ResponseTimeout: return "response-timeout";
        case RetireReason::GenerationChanged: return "generation-changed";
    }
    return "unknown";
}

} // namespace ASFW::Audio::Families::BeBoB::Bootloader
