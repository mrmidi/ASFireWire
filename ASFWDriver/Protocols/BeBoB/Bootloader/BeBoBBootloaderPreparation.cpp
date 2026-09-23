// SPDX-License-Identifier: Apache-2.0
#include "BeBoBBootloaderPreparation.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"

namespace ASFW::Protocols::BeBoB::Bootloader {
namespace {
PreparationStep Retire(RetireReason reason) noexcept { return {Retired{reason}, Done{}}; }
PreparationStep Stay(const PreparationState& state) noexcept { return {state, Done{}}; }

PreparationStep Evaluate(const BootRomInfo& info) noexcept {
    if (!info.BootloaderActive()) return Retire(RetireReason::FirmwareAlreadyRunning);
    if (!info.SupportsStoredFirmwareCue()) return Retire(RetireReason::UnsupportedBuild);
    return {AwaitingReenumeration{info.ProtocolVersion()}, WriteCue{BeBoBBootloaderCue{info}}};
}
}

PreparationStep BeginPreparation() noexcept { return {ReadingInfo{0}, ReadInfoBlock{}}; }

bool ShouldPrepareBootloader(
    uint32_t vendorId, uint32_t modelId,
    const ASFW::Discovery::DeviceIdentityEvidence& identity) noexcept {
    if (!IsSupportedBootloaderPersona(vendorId, modelId)) return false;
    const auto policy = ASFW::DeviceProfiles::Audio::AudioDeviceCatalog::Resolve(identity);
    return policy.has_value() &&
           policy->bootloaderCue ==
               ASFW::DeviceProfiles::Audio::BootloaderCuePolicy::BeBoBStartFirmware;
}

PreparationStep AdvancePreparation(const PreparationState& state,
                                   const PreparationEvent& event) noexcept {
    if (std::holds_alternative<GenerationInvalidated>(event)) {
        if (IsRetired(state)) return Stay(state);
        return Retire(RetireReason::GenerationChanged);
    }
    if (const auto* reading = std::get_if<ReadingInfo>(&state)) {
        if (const auto* ok = std::get_if<InfoReadSucceeded>(&event)) return Evaluate(ok->info);
        if (std::holds_alternative<InfoReadFailed>(event)) {
            const auto next = static_cast<uint8_t>(reading->attempt + 1);
            if (next >= kMaxInfoReadAttempts) return Retire(RetireReason::InfoUnavailable);
            return {ReadingInfo{next}, ReadInfoBlock{}};
        }
        return Stay(state);
    }
    if (std::holds_alternative<AwaitingReenumeration>(state)) {
        if (std::holds_alternative<CueWriteFailed>(event)) return Retire(RetireReason::CueWriteFailed);
        // A successful cue is expected to trigger a bus reset and a fresh
        // operational persona. C802:9000 belongs to firmware download and
        // must not be polled on this start-application path.
        if (std::holds_alternative<CueWriteSucceeded>(event)) return Stay(state);
    }
    return Stay(state);
}
} // namespace ASFW::Protocols::BeBoB::Bootloader
