// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Bounded AV/C command construction for M-Audio 1814 / ProjectMix special firmware.
// Wire behavior cross-checked against Linux firewire/bebob/bebob_maudio.c.

#pragma once

#include "AVCCommand.hpp"

#include <cstdint>
#include <optional>

namespace ASFW::Protocols::AVC {

/// Values accepted by Linux's special-firmware clock control.
enum class MAudioSpecialClockSource : uint8_t {
    InternalWithDigitalMute = 0,
    Digital = 1,
    WordClock = 2,
    Internal = 3,
};

/// The special clock command's digital-format fields: SPDIF or ADAT.
enum class MAudioSpecialDigitalFormat : uint8_t {
    Spdif = 0,
    Adat = 1,
};

/// Build the one vendor-dependent CONTROL command used to initialize or set
/// M-Audio special-firmware clock/format state. Returns empty for enum values
/// outside the device's documented control domain.
///
/// The CDB is deliberately 16 bytes on the wire: the nine defined bytes after
/// the command header are followed by seven zero bytes. The Linux transaction
/// sends 12 bytes; this driver's strict device allowlist records the 16-byte
/// vendor trace shape, and its pinned padding remains zero in either case.
[[nodiscard]] inline std::optional<AVCCdb> BuildMAudioSpecialClockCommand(
    MAudioSpecialClockSource clockSource,
    MAudioSpecialDigitalFormat inputFormat,
    MAudioSpecialDigitalFormat outputFormat,
    bool lockSettings) noexcept {
    const auto source = static_cast<uint8_t>(clockSource);
    const auto input = static_cast<uint8_t>(inputFormat);
    const auto output = static_cast<uint8_t>(outputFormat);
    if (source > 3 || input > 1 || output > 1) {
        return std::nullopt;
    }

    AVCCdb cdb{};
    cdb.ctype = static_cast<uint8_t>(AVCCommandType::kControl);
    cdb.subunit = kAVCSubunitUnit;
    cdb.opcode = 0x00; // Vendor-dependent; company ID below pins this to M-Audio.
    cdb.operands[0] = 0x04; // M-Audio company ID
    cdb.operands[1] = 0x00;
    cdb.operands[2] = 0x04;
    cdb.operands[3] = source;
    cdb.operands[4] = input;
    cdb.operands[5] = output;
    cdb.operands[6] = lockSettings ? 1 : 0;
    cdb.operands[7] = 0x00;
    cdb.operands[8] = 0x00;
    // `Encode()` quadlet-aligns 3 + operandLength. Thirteen operands produce
    // the exact 16-byte shape admitted by kMAudioSpecialPermittedFrames.
    cdb.operandLength = 13;
    return cdb;
}

/// Linux's initialization state for both special firmware personas.
[[nodiscard]] inline std::optional<AVCCdb> BuildMAudioSpecialInitialClockCommand() noexcept {
    return BuildMAudioSpecialClockCommand(MAudioSpecialClockSource::Internal,
                                          MAudioSpecialDigitalFormat::Spdif,
                                          MAudioSpecialDigitalFormat::Spdif,
                                          false);
}

} // namespace ASFW::Protocols::AVC
