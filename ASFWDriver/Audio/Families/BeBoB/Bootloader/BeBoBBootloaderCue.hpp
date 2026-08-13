// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBBootloaderCue.hpp — the one write this driver may make to a BeBoB
// bootloader.
//
// The cue tells a BridgeCo BeBoB bootloader to start the application firmware.
// It writes nothing persistent, so a malformed cue costs a power cycle and no
// more. Its *neighbours* are another matter. Every one of these is the same
// 12-byte block write to the same address, differing only in the command-code
// byte at offset 6 (bebob_dl_codes.h:39-54):
//
//   0x04/0x05/0x06  DownloadStart/Block/End  overwrite the firmware image
//   0x0a            ProgramGUID              rewrites the device GUID, in flash
//   0x0b            ProgramMAC               rewrites the MAC, in flash
//   0x0c            InitPersParams           rewrites persistent parameters
//   0x10            ProgramHWIdVersion       rewrites hardware ID/version, in flash
//
// Command 0x11 is therefore frozen here and no opcode parameter exists anywhere
// in this interface — the destructive commands have no representation in the
// driver at all. The second guard is the user client, which refuses any write
// into this register window so the app cannot hand-assemble one either.
//
// Wire behaviour cross-validated with
// references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:41-49,119-121
// (MAUDIO_BOOTLOADER_CUE1/2/3) and
// references/libffado-2.5.0/src/bebob/bebob_dl_codes.h:54,285-292 plus
// bebob_dl_mgr.cpp:45-49 (address map). Fresh implementation; no reference code
// is copied.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::Audio::Families::BeBoB::Bootloader {

// BeBoB bootloader register window. These are vendor registers, not CSR space.
inline constexpr uint16_t kBootloaderAddressHi = 0xFFFF;
inline constexpr uint32_t kInfoAddressLo = 0xC802'0000;
inline constexpr uint32_t kRequestAddressLo = 0xC802'1000;
inline constexpr uint32_t kResponseAddressLo = 0xC802'9000;

/// Bytes of the BootROM info block the vendor driver reads. It reads only this
/// base record and never the longer 104-byte form.
inline constexpr uint32_t kInfoBlockBytes = 80;

/// Byte offsets within the info block.
inline constexpr size_t kInfoOffsetProtocolVersion = 0x08;
inline constexpr size_t kInfoOffsetBootloaderVersion = 0x0C;

// ---------------------------------------------------------------------------
// ENDIANNESS — deliberate exception to the house rule.
//
// IEEE 1394 payloads are big-endian and this driver decodes them with
// FromBusOrder. This register window is the exception: its quadlets are
// LITTLE-endian. Do not "fix" this to FromBusOrder.
//
// Three independent confirmations:
//   * Linux writes the cue with cpu_to_le32 (bebob_maudio.c:119-121).
//   * Linux reads the info block through snd_fw_transaction with no swap
//     (bebob.h:120-134), i.e. little-endian on a little-endian host.
//   * The vendor kext hands native _DWORD[] straight to IOFWWriteCommand with
//     no swap anywhere, which produces the same bytes on x86.
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr uint32_t LoadLittleEndianQuadlet(
    std::span<const uint8_t> bytes, size_t offset) noexcept {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24U);
}

/// Parsed BootROM info block. Owning this type is the only way to reach a cue,
/// which is what keeps a cue's protocol version from ever being stale.
struct BootRomInfo final {
    std::array<uint8_t, kInfoBlockBytes> raw{};

    [[nodiscard]] uint32_t ProtocolVersion() const noexcept {
        return LoadLittleEndianQuadlet(raw, kInfoOffsetProtocolVersion);
    }

    /// The BootROM "bootloader version" field, used by the vendor driver as a
    /// state flag rather than a version: non-zero means the bootloader is the
    /// running image and the application firmware has not been started.
    [[nodiscard]] uint32_t BootloaderVersion() const noexcept {
        return LoadLittleEndianQuadlet(raw, kInfoOffsetBootloaderVersion);
    }

    [[nodiscard]] bool BootloaderActive() const noexcept {
        return BootloaderVersion() != 0;
    }
};

/// The complete, frozen bootloader request this driver is able to emit.
class BeBoBBootloaderCue final {
public:
    static constexpr size_t kCueBytes = 12;

    /// The only constructor. There is no command parameter, here or anywhere:
    /// the sole variable in the request is the protocol version, which must come
    /// from a BootRomInfo the caller has actually read.
    explicit BeBoBBootloaderCue(const BootRomInfo& info) noexcept;

    [[nodiscard]] std::span<const uint8_t, kCueBytes> Bytes() const noexcept {
        return std::span<const uint8_t, kCueBytes>{bytes_};
    }

    [[nodiscard]] uint32_t ProtocolVersion() const noexcept;

private:
    /// Command 0x11, start mode 0 — start the application firmware.
    static constexpr uint32_t kStartApplicationFirmware = 0x0111'0000;
    static constexpr uint32_t kApplicationStartMode = 0x0000'0000;

    static_assert(((kStartApplicationFirmware >> 16U) & 0xFFU) == 0x11U,
                  "command code must remain start-application-firmware (0x11); "
                  "0x10 and 0x0a in this field permanently reprogram device "
                  "identity in flash");
    static_assert(kApplicationStartMode == 0,
                  "start mode must remain application (0); 2 boots the debugger");

    std::array<uint8_t, kCueBytes> bytes_{};
};

/// The choke point. Returns true only for the exact frozen cue at exactly the
/// request register — every other write anywhere in the bootloader window is
/// rejected, including a correct-looking cue at the wrong address.
///
/// This is a hard gate, not advice: callers treat false as a programming error.
[[nodiscard]] bool IsPermittedBootloaderWrite(
    uint16_t addressHi, uint32_t addressLo,
    std::span<const uint8_t> payload) noexcept;

/// True when the address falls inside the bootloader register window at all.
/// Used to assert that no other code path writes there.
[[nodiscard]] constexpr bool IsBootloaderWindow(uint16_t addressHi,
                                                uint32_t addressLo) noexcept {
    return addressHi == kBootloaderAddressHi &&
           (addressLo & 0xFFFF'0000U) == (kInfoAddressLo & 0xFFFF'0000U);
}

} // namespace ASFW::Audio::Families::BeBoB::Bootloader
