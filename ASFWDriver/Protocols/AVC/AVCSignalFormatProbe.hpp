// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCSignalFormatProbe.hpp — the one AV/C exchange that is safe on firmware
// which hangs on ordinary discovery.
//
// The M-Audio "special" BeBoB devices (FireWire 1814 / ProjectMix I/O) freeze on
// UNIT_INFO, SUBUNIT_INFO, PLUG_INFO and every BridgeCo extension, so no probe
// tier in this driver may be pointed at them. Linux still reads their sample
// rate, and it does so with exactly one command: unit PLUG SIGNAL FORMAT,
// STATUS, on input plug 0 — "Input plug shows actual rate".
//
//   references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:302-313
//     special_get_rate() -> avc_general_get_sig_fmt(AVC_GENERAL_PLUG_DIR_IN, 0)
//   references/linux-sound-firewire-stack/firewire/fcp.c:84-135
//     the frame, the response checks, and the sfc extraction
//
// Closed by construction, the same way BeBoBBootloaderCue is: there is no
// opcode parameter and no operand parameter anywhere in this interface. The
// destructive and freeze-capable neighbours have no representation here, so a
// caller cannot reach them by passing a different value.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::Protocols::AVC {

/// The complete request. Eight bytes, byte-for-byte as Linux builds it.
inline constexpr size_t kSignalFormatProbeBytes = 8;

enum class SignalFormatPlugDirection : uint8_t {
    /// Opcode 0x19. The direction special_get_rate() uses, because on these
    /// devices the input plug reports the rate actually in effect.
    Input,
    /// Opcode 0x18. cache_freq() uses this one on ordinary BeBoB firmware.
    Output,
};

/// Builds the STATUS request. `plugId` is the only caller-supplied value, and
/// it addresses a plug — it can never become a command code.
[[nodiscard]] constexpr std::array<uint8_t, kSignalFormatProbeBytes>
BuildSignalFormatProbe(SignalFormatPlugDirection direction,
                       uint8_t plugId) noexcept {
    return {
        0x01,  // AV/C STATUS
        0xFF,  // unit address
        direction == SignalFormatPlugDirection::Input ? uint8_t{0x19}
                                                      : uint8_t{0x18},
        plugId,
        // EOH_1, Form_1, FMT = AM824. Linux sends this literally on the STATUS
        // request and requires the device to echo it back (its transaction mask
        // covers bytes 1-4), so it is NOT a 0xFF "query this field" wildcard.
        0x90,
        0xFF,  // FDF-hi: the field the answer arrives in
        0xFF,  // FDF-mid: SYT hi, unused
        0xFF,  // FDF-lo: SYT lo, unused
    };
}

/// AV/C response codes this probe distinguishes. Anything else is transport.
inline constexpr uint8_t kResponseNotImplemented = 0x08;
inline constexpr uint8_t kResponseRejected = 0x0A;
inline constexpr uint8_t kResponseInTransition = 0x0B;

enum class SignalFormatProbeOutcome : uint8_t {
    /// A rate was decoded.
    Decoded,
    /// Response too short to be this command's reply.
    Malformed,
    /// Device answered NOT IMPLEMENTED — it does not support the command.
    NotImplemented,
    /// Device answered REJECTED.
    Rejected,
    /// Device answered IN TRANSITION, or the sfc field was reserved. Linux
    /// treats a reserved sfc as in-transition too and retries; both mean "ask
    /// again", not "unsupported".
    InTransition,
    /// The echoed bytes 1-4 did not match the request, so this reply does not
    /// belong to this command.
    Mismatched,
};

struct SignalFormatProbeResult final {
    SignalFormatProbeOutcome outcome{SignalFormatProbeOutcome::Malformed};
    /// Only meaningful when outcome == Decoded.
    uint32_t sampleRateHz{0};
    /// The raw sfc nibble, retained for logging even when it is reserved.
    uint8_t sfc{0xFF};
};

/// sfc -> Hz. amdtp_rate_table, amdtp-stream.c:149-157. Indices are the CIP SFC
/// codes and are a wire encoding: never reorder.
[[nodiscard]] constexpr std::optional<uint32_t> SampleRateForSfc(
    uint8_t sfc) noexcept {
    switch (sfc) {
        case 0: return 32000U;
        case 1: return 44100U;
        case 2: return 48000U;
        case 3: return 88200U;
        case 4: return 96000U;
        case 5: return 176400U;
        case 6: return 192000U;
        default: return std::nullopt;  // 7 is reserved
    }
}

/// Decodes a reply against the request that produced it.
[[nodiscard]] constexpr SignalFormatProbeResult DecodeSignalFormatProbe(
    std::span<const uint8_t> request, std::span<const uint8_t> response) noexcept {
    if (response.size() < kSignalFormatProbeBytes ||
        request.size() < kSignalFormatProbeBytes) {
        return SignalFormatProbeResult{SignalFormatProbeOutcome::Malformed, 0, 0xFF};
    }
    switch (response[0]) {
        case kResponseNotImplemented:
            return SignalFormatProbeResult{
                SignalFormatProbeOutcome::NotImplemented, 0, 0xFF};
        case kResponseRejected:
            return SignalFormatProbeResult{SignalFormatProbeOutcome::Rejected, 0,
                                           0xFF};
        case kResponseInTransition:
            return SignalFormatProbeResult{SignalFormatProbeOutcome::InTransition,
                                           0, 0xFF};
        default:
            break;
    }
    // Bytes 1-4 must come back unchanged. Linux enforces this with
    // BIT(1)|BIT(2)|BIT(3)|BIT(4) on the transaction, which is what stops a
    // reply to a different command being read as this one's answer.
    for (size_t index = 1; index <= 4; ++index) {
        if (response[index] != request[index]) {
            return SignalFormatProbeResult{SignalFormatProbeOutcome::Mismatched,
                                           0, 0xFF};
        }
    }
    const auto sfc = static_cast<uint8_t>(response[5] & 0x07U);
    const auto rate = SampleRateForSfc(sfc);
    if (!rate) {
        return SignalFormatProbeResult{SignalFormatProbeOutcome::InTransition, 0,
                                       sfc};
    }
    return SignalFormatProbeResult{SignalFormatProbeOutcome::Decoded, *rate, sfc};
}

} // namespace ASFW::Protocols::AVC
