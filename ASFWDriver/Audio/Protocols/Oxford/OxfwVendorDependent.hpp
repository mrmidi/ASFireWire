// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfwVendorDependent.hpp - AV/C VENDOR-DEPENDENT framing shared by OXFW vendors.
//
// Every OXFW vendor opens a vendor-dependent frame the same way:
//
//     OUI(3) | magic prefix(3) | command code(1) | ...vendor-specific tail...
//
// What follows the code is NOT common. Apogee always emits two argument bytes
// before its payload (ApogeeVendorCodec), while Tascam FireOne puts a single
// operand straight after the code - its frames are 8 bytes total
// (tascam.rs:453, :465) against Apogee's 9-byte header. So this template owns
// the 7-byte prefix and nothing beyond it; the argument shape belongs to the
// device codec.
//
// | Device      | OUI         | Magic       | Reference                    |
// |-------------|-------------|-------------|------------------------------|
// | Apogee Duet | 00:03:db    | 'P','C','M' | apogee.rs:127, :871          |
// | Tascam F1   | 00:02:2e    | 'F','I','1' | tascam.rs:10, :249           |
//
// References read as behavioural sources only; no code copied.

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ASFW::Audio::Oxford {

//==============================================================================
// Shared frame geometry
//==============================================================================

inline constexpr size_t kVendorOuiSize = 3;
inline constexpr size_t kVendorMagicSize = 3;

/// OUI + magic + code. Everything after this is the device's own business.
inline constexpr size_t kVendorPrefixSize = kVendorOuiSize + kVendorMagicSize + 1U;

/// A spec supplies the two byte strings that identify its vendor frames.
template <typename Spec>
concept VendorDependentSpec = requires {
    { Spec::kOui } -> std::convertible_to<std::array<uint8_t, kVendorOuiSize>>;
    { Spec::kMagic } -> std::convertible_to<std::array<uint8_t, kVendorMagicSize>>;
};

//==============================================================================
// Response acceptance
//==============================================================================

/// AV/C response codes a vendor-dependent exchange may legitimately return.
/// CONTROL is answered ACCEPTED and STATUS is answered IMPLEMENTED/STABLE
/// (linux oxfw-spkr.c:39-45).
inline constexpr uint8_t kResponseAccepted = 0x09;
inline constexpr uint8_t kResponseImplementedStable = 0x0C;

/// Whether the OUI echoed back in a response must match the one we sent.
///
/// This is overridable because it is genuinely not universal: Tascam FireOne
/// answers vendor-dependent control with IMPLEMENTED/STABLE rather than the
/// spec-mandated ACCEPTED, and echoes company_id 0xffffff instead of its own
/// OUI (tascam.rs:405-406). Hardcoding the check here would mean surgery on
/// this file the first time such a device is supported.
template <typename Spec>
struct VendorResponsePolicy {
    [[nodiscard]] static constexpr bool EchoesOui() noexcept {
        if constexpr (requires { Spec::kEchoesOuiInResponse; }) {
            return Spec::kEchoesOuiInResponse;
        } else {
            return true;
        }
    }

    [[nodiscard]] static constexpr bool AcceptsResponseCode(uint8_t code,
                                                            bool isStatus) noexcept {
        if constexpr (requires { Spec::AcceptsResponseCode(code, isStatus); }) {
            return Spec::AcceptsResponseCode(code, isStatus);
        } else {
            return isStatus ? code == kResponseImplementedStable : code == kResponseAccepted;
        }
    }
};

//==============================================================================
// Framing
//==============================================================================

template <VendorDependentSpec Spec>
struct VendorDependentFraming {
    using Policy = VendorResponsePolicy<Spec>;

    static constexpr size_t kPrefixSize = kVendorPrefixSize;

    /// Append OUI + magic + command code.
    static void WritePrefix(std::vector<uint8_t>& out, uint8_t code) {
        out.insert(out.end(), Spec::kOui.begin(), Spec::kOui.end());
        out.insert(out.end(), Spec::kMagic.begin(), Spec::kMagic.end());
        out.push_back(code);
    }

    /// Structural check of a status response: long enough, right vendor, and
    /// echoing the command code we asked about. A response that fails this must
    /// be rejected rather than parsed - the operand bytes behind a wrong
    /// prefix mean something else entirely.
    [[nodiscard]] static bool ValidatePrefix(std::span<const uint8_t> payload,
                                             uint8_t code) noexcept {
        if (payload.size() < kPrefixSize) {
            return false;
        }
        if (Policy::EchoesOui()) {
            for (size_t i = 0; i < kVendorOuiSize; ++i) {
                if (payload[i] != Spec::kOui[i]) {
                    return false;
                }
            }
        }
        for (size_t i = 0; i < kVendorMagicSize; ++i) {
            if (payload[kVendorOuiSize + i] != Spec::kMagic[i]) {
                return false;
            }
        }
        return payload[kVendorOuiSize + kVendorMagicSize] == code;
    }
};

//==============================================================================
// AV/C boolean operand encoding
//==============================================================================

/// 0x70 / 0x60 are the *standard* AV/C mute operand values (linux
/// oxfw-spkr.c:58 writes `*value ? 0x70 : 0x60` for a Feature Block mute).
/// Apogee reused that encoding for all of its vendor booleans, so these are
/// not arbitrary Apogee constants and belong at this layer.
inline constexpr uint8_t kAvcBoolOn = 0x70;
inline constexpr uint8_t kAvcBoolOff = 0x60;

[[nodiscard]] constexpr uint8_t ToAvcBool(bool value) noexcept {
    return value ? kAvcBoolOn : kAvcBoolOff;
}

[[nodiscard]] constexpr bool FromAvcBool(uint8_t value) noexcept {
    return value == kAvcBoolOn;
}

} // namespace ASFW::Audio::Oxford
