// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MAudioSpecialClock.hpp - M-Audio's special-firmware clock/format command: a
// VENDOR-DEPENDENT (0x00) CONTROL to the unit. Vendor extension, so it lives
// outside Commands/; device policy (when to send it, which values) stays in the
// BeBoB device layer.
//
// Layout: Linux sound/firewire/bebob/bebob_maudio.c (special clock control).
// Frame: [00][FF][00][04 00 04][source][input][output][lock][00 x6]
//   The vendor kext sends 16 bytes; Linux sends 12. The 16-byte trace shape is
//   what the M-Audio special allowlist admits (AVC_DEVICE_HAZARDS.md H1), and the
//   extra bytes are zero either way.

#pragma once

#include "../Commands/GeneralCommands.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace ASFW::AVC::MAudio {

/// Values accepted by the special firmware's clock control.
enum class SpecialClockSource : uint8_t {
    InternalWithDigitalMute = 0,
    Digital = 1,
    WordClock = 2,
    Internal = 3,
};

/// The digital input/output format fields: S/PDIF or ADAT.
enum class SpecialDigitalFormat : uint8_t {
    Spdif = 0,
    Adat = 1,
};

/// The three bytes the command carries where a company ID goes.
inline constexpr CompanyId kSpecialClockCompanyId{0x04, 0x00, 0x04};

/// The clock/format command, to be sent as CONTROL. Empty for values outside
/// the firmware's documented domain.
[[nodiscard]] inline std::optional<Cmd::RawVendorDependentCommand> SpecialClockCommand(
    SpecialClockSource source, SpecialDigitalFormat input, SpecialDigitalFormat output,
    bool lockSettings) noexcept {
    const auto sourceByte = static_cast<uint8_t>(source);
    const auto inputByte = static_cast<uint8_t>(input);
    const auto outputByte = static_cast<uint8_t>(output);
    if (sourceByte > 3 || inputByte > 1 || outputByte > 1) {
        return std::nullopt;
    }
    // Ten payload bytes after the company ID make the 16-byte frame.
    const std::array<uint8_t, 10> payload{sourceByte, inputByte, outputByte,
                                          static_cast<uint8_t>(lockSettings ? 1 : 0)};
    return Cmd::RawVendorDependentCommand{
        .operands = Cmd::RawVendorDependentOperands(kSpecialClockCompanyId, payload)};
}

/// Linux's initialization state for both special-firmware personas: internal
/// clock, S/PDIF in and out, settings unlocked.
[[nodiscard]] inline std::optional<Cmd::RawVendorDependentCommand> SpecialInitialClockCommand() noexcept {
    return SpecialClockCommand(SpecialClockSource::Internal, SpecialDigitalFormat::Spdif,
                               SpecialDigitalFormat::Spdif, false);
}

} // namespace ASFW::AVC::MAudio
