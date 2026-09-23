// SPDX-License-Identifier: Apache-2.0
// Pure state machine for the M-Audio bootloader preparation slice. It owns no
// transport and is intentionally not connected to live discovery.
#pragma once

#include "BeBoBBootloaderCue.hpp"
#include <cstdint>
#include <variant>

namespace ASFW::Protocols::BeBoB::Bootloader {
inline constexpr uint8_t kMaxInfoReadAttempts = 3;

struct ReadingInfo final { uint8_t attempt{0}; };
struct AwaitingReenumeration final { uint32_t protocolVersion{0}; };
enum class RetireReason : uint8_t {
    FirmwareAlreadyRunning, UnsupportedBuild, InfoUnavailable,
    CueWriteFailed, GenerationChanged
};
struct Retired final { RetireReason reason{RetireReason::FirmwareAlreadyRunning}; };
using PreparationState = std::variant<ReadingInfo, AwaitingReenumeration, Retired>;

struct InfoReadSucceeded final { BootRomInfo info{}; };
struct InfoReadFailed final {};
struct CueWriteSucceeded final {};
struct CueWriteFailed final {};
struct GenerationInvalidated final {};
using PreparationEvent = std::variant<InfoReadSucceeded, InfoReadFailed,
    CueWriteSucceeded, CueWriteFailed, GenerationInvalidated>;

struct ReadInfoBlock final {};
struct WriteCue final { BeBoBBootloaderCue cue; };
struct Done final {};
using PreparationAction = std::variant<ReadInfoBlock, WriteCue, Done>;
struct PreparationStep final { PreparationState state; PreparationAction action; };

[[nodiscard]] PreparationStep BeginPreparation() noexcept;
[[nodiscard]] PreparationStep AdvancePreparation(
    const PreparationState& state, const PreparationEvent& event) noexcept;
[[nodiscard]] inline bool IsRetired(const PreparationState& state) noexcept {
    return std::holds_alternative<Retired>(state);
}
} // namespace ASFW::Protocols::BeBoB::Bootloader
