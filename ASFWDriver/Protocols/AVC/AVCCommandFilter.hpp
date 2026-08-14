// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCCommandFilter.hpp — a per-device allowlist of AV/C command shapes.
//
// Some firmware hangs on AV/C commands it does not implement. For those devices
// the driver must be able to send a small, named set of commands and nothing
// else, including through `SendRawFCPCommand`, whose payload comes from user
// space. This header supplies the match rule and the tables; `FCPTransport`
// enforces them at `SubmitCommand`, which every AV/C frame passes through.
//
// **This is an allowlist, not a blocklist.** An empty table means "no
// restriction" and is what every ordinary device carries. A non-empty table
// admits *only* frames matching one of its entries; anything unlisted is
// refused. Widening a table is therefore a deliberate act — adding a command
// means adding a row with the reference that says the device tolerates it.
//
// The granularity is a masked byte prefix rather than an opcode, because opcode
// is the wrong unit twice over: the M-Audio clock command and the BridgeCo
// extensions that freeze these devices are both vendor-dependent (opcode 0x00
// and 0x2F respectively), so the safety boundary falls on the company ID and
// the extension byte, not on the opcode.
//
// Row evidence lives in Protocols/AVC/AVC_DEVICE_HAZARDS.md (H1). Do not add a
// row without adding its evidence there first.

#pragma once

#include "../../Discovery/DiscoveryTypes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace ASFW::Protocols::AVC {

/// Longest prefix any row needs. The widest permitted shape on record is the
/// vendor kext's 16-byte M-Audio clock command (AVC_DEVICE_HAZARDS.md H1).
inline constexpr size_t kCommandFilterPrefixBytes = 16;

/// One permitted command shape.
///
/// A frame matches when it is exactly `length` bytes long and every byte
/// satisfies `(frame[i] & care[i]) == (prefix[i] & care[i])`.
/// A `care` byte of 0x00 marks a caller-supplied operand; 0xFF pins the byte.
/// Partial masks are meaningful — an AV/C subunit address is `0x08 | id`, which
/// pins with `care = 0xF8`.
struct FCPPermittedFrame final {
    std::array<uint8_t, kCommandFilterPrefixBytes> prefix{};
    std::array<uint8_t, kCommandFilterPrefixBytes> care{};
    uint8_t length{0};
    /// Human-readable row name, used in the refusal log line.
    const char* name{nullptr};
};

/// True when `frame` matches this permitted shape.
[[nodiscard]] constexpr bool FrameMatches(const FCPPermittedFrame& permitted,
                                          std::span<const uint8_t> frame) noexcept {
    if (permitted.length == 0 || frame.size() != permitted.length) {
        return false;
    }
    for (size_t i = 0; i < permitted.length; ++i) {
        if ((frame[i] & permitted.care[i]) != (permitted.prefix[i] & permitted.care[i])) {
            return false;
        }
    }
    return true;
}

/// True when `frame` is admitted by `table`. An empty table admits everything —
/// that is the unrestricted default carried by every ordinary device.
[[nodiscard]] constexpr bool FrameIsPermitted(
    std::span<const FCPPermittedFrame> table,
    std::span<const uint8_t> frame) noexcept {
    if (table.empty()) {
        return true;
    }
    return std::ranges::any_of(table, [frame](const FCPPermittedFrame& permitted) {
        return FrameMatches(permitted, frame);
    });
}

namespace detail {

/// Builds a row from a prefix and a matching care mask, both given as the
/// leading bytes only.
consteval FCPPermittedFrame Row(const char* name,
                                std::initializer_list<uint8_t> prefix,
                                std::initializer_list<uint8_t> care) {
    FCPPermittedFrame row{};
    row.name = name;
    row.length = static_cast<uint8_t>(prefix.size());
    size_t index = 0;
    for (auto byte : prefix) {
        row.prefix[index++] = byte;
    }
    index = 0;
    for (auto byte : care) {
        row.care[index++] = byte;
    }
    return row;
}

} // namespace detail

//==============================================================================
// M-Audio "special firmware" (FireWire 1814, ProjectMix I/O)
//==============================================================================

/// The commands this driver may send to booted M-Audio special firmware.
///
/// Everything absent from this table is refused, including UNIT_INFO (0x30),
/// SUBUNIT_INFO (0x31), PLUG_INFO (0x02) and the BridgeCo extended stream
/// format commands (0x2F with 0xC0/0xC1) — the four shapes AVC_DEVICE_HAZARDS.md
/// H1 records as freeze-capable on this firmware.
///
/// Rows arrive with their caller, not ahead of it. H1's remaining safe entry —
/// the `03 00 01` LED command — is deliberately absent because nothing sends it.
inline constexpr std::array kMAudioSpecialPermittedFrames{
    // STATUS, unit, INPUT PLUG SIGNAL FORMAT, plug id free, FMT pinned to
    // AM824 (0x90) because the device must echo it back, FDF is the answer.
    //   references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:302-313
    //   special_get_rate() -> avc_general_get_sig_fmt(AVC_GENERAL_PLUG_DIR_IN, 0)
    detail::Row("sig-fmt STATUS input plug",
                {0x01, 0xFF, 0x19, 0x00, 0x90, 0x00, 0x00, 0x00},
                {0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00}),
    // STATUS, unit, OUTPUT PLUG SIGNAL FORMAT. ALSA's cache_freq() reads the
    // rate from this one on the same firmware.
    //   references/alsa-userspace-control-protocols-impl/protocols/bebob/src/maudio/special.rs:101-119
    detail::Row("sig-fmt STATUS output plug",
                {0x01, 0xFF, 0x18, 0x00, 0x90, 0x00, 0x00, 0x00},
                {0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00}),
    // CONTROL signal format. Same shape as the STATUS rows above, differing in
    // ctype and in carrying a real FDF instead of 0xFF. This is how the rate is
    // set, and the 1814 additionally needs it re-sent after DMA start (H8).
    //
    // Sending CONTROL to this firmware is attested twice over, which is why it
    // is admitted without a first-contact ceremony of our own:
    //   references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:315-338
    //     special_set_rate() — OUT, settle, IN, on these exact model IDs
    //   the vendor kext binds FWRev1AudioDevice::AVCControlPlugSignalFormat
    //     (blind 0x18/0x19 CONTROL) into the 1814's vtable at 0x37510
    detail::Row("sig-fmt CONTROL input plug",
                {0x00, 0xFF, 0x19, 0x00, 0x90, 0x00, 0x00, 0x00},
                {0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00}),
    detail::Row("sig-fmt CONTROL output plug",
                {0x00, 0xFF, 0x18, 0x00, 0x90, 0x00, 0x00, 0x00},
                {0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00}),
    // M-Audio vendor-dependent clock/format command. Carries clk_src,
    // dig_in_fmt, dig_out_fmt and clk_lock — and dig_in_fmt/dig_out_fmt are what
    // select the hardcoded formation table (H5), so no geometry is reachable
    // without this row.
    //
    // The company ID at bytes 3..5 is pinned. That is the entire safety
    // boundary here: BridgeCo's own extensions are vendor-dependent too, share
    // opcode 0x00, and freeze this firmware. Only these three bytes separate
    // the command M-Audio's driver sends every boot from the ones that hang it.
    //   references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:186-198
    //     avc_maudio_set_special_clk(); same 12 meaningful bytes
    //   vendor kext SetClockSourceInternal @ 0xe25c and FireBug trace:
    //     16-byte frame with four additional zero pad bytes
    detail::Row("M-Audio vendor clock/format",
                {0x00, 0xFF, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
                 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}),
    // Exact second half of the vendor driver's blank-slate clock sequence:
    // Audio subunit 0, selector FB 4, current attribute, input interface 0.
    // Keep every byte pinned. A generic FUNCTION BLOCK allowance would expose
    // this freeze-prone firmware to unrelated mixer commands.
    //
    //   vendor kext SetClockSourceInternal @ 0xe45a -> base
    //     SetClockSourceInternal @ 0x23148 -> AVCSelectorCommand(4, 0)
    //   references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:461-462,514
    detail::Row("M-Audio blank-slate input selector",
                {0x00, 0x08, 0xB8, 0x80, 0x04, 0x10,
                 0x02, 0x00, 0x01, 0x00, 0x00, 0x00},
                {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}),
};

/// Resolves a filter id to its table. `Unrestricted` yields an empty span.
[[nodiscard]] constexpr std::span<const FCPPermittedFrame>
PermittedFramesFor(Discovery::AvcCommandFilterId id) noexcept {
    switch (id) {
        case Discovery::AvcCommandFilterId::MAudioSpecialBeBoB:
            return kMAudioSpecialPermittedFrames;
        case Discovery::AvcCommandFilterId::Unrestricted:
            break;
    }
    return {};
}

} // namespace ASFW::Protocols::AVC
