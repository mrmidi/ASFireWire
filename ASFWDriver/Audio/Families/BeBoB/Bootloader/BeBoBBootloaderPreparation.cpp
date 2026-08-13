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

[[nodiscard]] PreparationStep OnEvaluatingInfo(const EvaluatingInfo& state) noexcept {
    if (!state.info.BootloaderActive()) {
        // The application firmware is already running. This is the common case
        // on a healthy device and must send nothing at all.
        return Retire(RetireReason::NoCueNeeded);
    }
    // The one write. Built from this state's own info block, so its protocol
    // version cannot be stale.
    return PreparationStep{
        AwaitingReenumeration{state.info.ProtocolVersion()},
        WriteCue{BeBoBBootloaderCue{state.info}}};
}

[[nodiscard]] PreparationStep OnReadingInfo(const ReadingInfo& state,
                                            const PreparationEvent& event) noexcept {
    if (const auto* ok = std::get_if<InfoReadSucceeded>(&event)) {
        // Decided here and now from the block just received. Returning
        // {EvaluatingInfo, ReadInfoBlock} instead would issue a *second* info
        // read whose payload is then discarded, because OnEvaluatingInfo
        // consumes no event and decides from the block it already holds. That
        // made forward progress depend on a redundant transaction to a device
        // in bootloader state; observed twice on hardware.
        //
        // EvaluatingInfo remains the only state that owns a BootRomInfo and the
        // only place a cue can be built, so the type invariant is unchanged --
        // it is now a value passed straight through rather than one the machine
        // parks in and waits to be poked out of.
        return OnEvaluatingInfo(EvaluatingInfo{ok->info});
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

[[nodiscard]] PreparationStep OnAwaitingReenumeration(
    const AwaitingReenumeration& state, const PreparationEvent& event) noexcept {
    if (std::holds_alternative<CueWriteSucceeded>(event)) {
        // FirmwareStart in the vendor kext writes the cue and immediately
        // schedules re-registration; it does not call FirmwareReadResponse.
        // Linux has the same normal path: cue -> bus reset -> fresh detection
        // (references/linux-sound-firewire-stack/firewire/bebob/
        // bebob_maudio.c:87-92,119-134). Keep the old generation inert rather
        // than polling C802:9000, whose read protocol belongs to firmware
        // download rather than this start cue.
        return PreparationStep{state, Done{}};
    }
    if (std::holds_alternative<CueWriteFailed>(event)) {
        // The write did not land, so nothing was started and there is nothing
        // to wait for. Note this is NOT the timeout case: a timed-out write may
        // have landed, and the transport reports that as a failed read later
        // rather than as CueWriteFailed. See the header.
        return Retire(RetireReason::CueWriteFailed);
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
        // The machine no longer parks here -- OnReadingInfo passes the block
        // straight through -- so this is reached only when a caller hands the
        // state in directly. Kept so AdvancePreparation stays total over the
        // variant, and it still consumes no event.
        return OnEvaluatingInfo(*evaluating);
    }
    if (const auto* awaiting = std::get_if<AwaitingReenumeration>(&state)) {
        return OnAwaitingReenumeration(*awaiting, event);
    }
    return Stay(state);
}

const char* PreparationStateName(const PreparationState& state) noexcept {
    if (std::holds_alternative<ReadingInfo>(state)) return "reading-info";
    if (std::holds_alternative<EvaluatingInfo>(state)) return "evaluating-info";
    // The post-cue state. Seeing this in a reset teardown log means the cue was
    // written and the device is expected to return in a fresh generation.
    if (std::holds_alternative<AwaitingReenumeration>(state)) {
        return "awaiting-reenumeration";
    }
    if (std::holds_alternative<Retired>(state)) return "retired";
    return "unknown";
}

const char* RetireReasonName(RetireReason reason) noexcept {
    switch (reason) {
        case RetireReason::NoCueNeeded: return "no-cue-needed";
        case RetireReason::InfoUnavailable: return "info-unavailable";
        case RetireReason::CueWriteFailed: return "cue-write-failed";
        case RetireReason::GenerationChanged: return "generation-changed";
    }
    return "unknown";
}

} // namespace ASFW::Audio::Families::BeBoB::Bootloader
