// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuStatusWord.hpp - The status word an 828 Mk3 writes to the host.
//
// The device writes it to the address the host programmed as its notification
// handler, which is the DICE notification address (0x0001'0000'0000), so it
// reaches the driver through DiceNotificationRouter like a DICE notification.
// It is NOT one, and the DICE `Notify` bits do not apply to it: MOTU reuses the
// transport, not the encoding. Reading it with DICE names is what hid a buffer
// fault behind the label "ExtStatus" for five sessions, and it is why the
// router's words are handed to the backend of the device's family rather than
// to the DICE backend.
//
// The map below is the one used by `FWA_BoxWithFxDSP` in the vendor driver,
// which is the class `FWA_Box828mk3` derives from. It was established from
// both sides independently: the host driver (its C++ symbols survive) and the
// device firmware, which assembles exactly these bits. Other MOTU families
// differ: `FWA_BoxV3HD`
// carries the buffer fault in bit 26 instead, so never apply this map by
// vendor ID alone.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ASFW::Audio::MOTU {

namespace MotuStatus {
    constexpr uint32_t kClockLocked      = 0x00000002;
    constexpr uint32_t kTimestampsValid  = 0x00000008;
    constexpr uint32_t kBufferUnderflow  = 0x00000040;
    constexpr uint32_t kBufferOverflow   = 0x00000080;
    constexpr uint32_t kPedal            = 0x01000000;
    constexpr uint32_t kFactoryReset     = 0x10000000;

    /// Either buffer fault. The vendor tests exactly this mask and calls
    /// RequestRestart on it, gated only on isoch running -- not on clock health.
    constexpr uint32_t kBufferFault = kBufferUnderflow | kBufferOverflow;

    /// Bit 31 makes the vendor handler discard the whole word before any other
    /// test, so it must be checked first and independently of the mask above.
    constexpr uint32_t kIgnoreWord = 0x80000000;

    /// The underflow/overflow bits are sticky in device firmware: accumulated
    /// with OR and cleared only when the watermark block is reset. A word with
    /// both set therefore means "the buffer ran dry at least once AND filled at
    /// least once since the last report" -- an oscillation, not a simultaneous
    /// pair, and not evidence of a monotonic rate mismatch.
    [[nodiscard]] constexpr bool HasBufferFault(uint32_t word) noexcept {
        return (word & kIgnoreWord) == 0 && (word & kBufferFault) != 0;
    }
}

namespace Detail {

/// Append a NUL-terminated token at `len`, advancing `len`. Bounds-checked.
inline void AppendStatusToken(char* buf, size_t cap, size_t& len, const char* str) noexcept {
    if (len + 1 >= cap) {
        return;
    }
    const int written = std::snprintf(buf + len, cap - len, "%s", str);
    if (written > 0) {
        len += static_cast<size_t>(written);
        if (len >= cap) {
            len = cap - 1;
        }
    }
}

} // namespace Detail

/// Decode a MOTU status word (see `MotuStatus`) for a log line. Deliberately
/// separate from DICE::FormatNotification: the same quadlet means different
/// things depending on which vendor sent it, so the caller has to pick, and a
/// log line can never silently apply the wrong map.
inline const char* FormatMotuStatus(uint32_t word, char* buf, size_t cap) noexcept {
    if (!buf || cap == 0) {
        return "";
    }
    struct BitName final {
        uint32_t bit;
        const char* name;
    };
    static constexpr BitName kFlags[] = {
        {MotuStatus::kClockLocked, "ClockLocked"},
        {MotuStatus::kTimestampsValid, "TimestampsValid"},
        {MotuStatus::kBufferUnderflow, "BufferUNDERFLOW"},
        {MotuStatus::kBufferOverflow, "BufferOVERFLOW"},
        {MotuStatus::kPedal, "Pedal"},
        {MotuStatus::kFactoryReset, "FactoryReset"},
        {MotuStatus::kIgnoreWord, "IGNORE"},
    };
    uint32_t known = 0;
    for (const auto& f : kFlags) {
        known |= f.bit;
    }
    buf[0] = '\0';
    size_t len = 0;
    bool any = false;
    for (const auto& f : kFlags) {
        if ((word & f.bit) == 0) {
            continue;
        }
        if (any) {
            Detail::AppendStatusToken(buf, cap, len, "|");
        }
        Detail::AppendStatusToken(buf, cap, len, f.name);
        any = true;
    }
    const uint32_t unknown = word & ~known;
    if (unknown != 0) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%x", unknown);
        if (any) {
            Detail::AppendStatusToken(buf, cap, len, "|");
        }
        Detail::AppendStatusToken(buf, cap, len, hex);
        any = true;
    }
    if (!any) {
        Detail::AppendStatusToken(buf, cap, len, "none");
    }
    return buf;
}

} // namespace ASFW::Audio::MOTU
