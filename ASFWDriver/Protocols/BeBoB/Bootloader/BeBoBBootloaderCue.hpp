// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::Protocols::BeBoB::Bootloader {

inline constexpr uint16_t kAddressHi = 0xFFFF;
inline constexpr uint32_t kMAudioVendorId = 0x00000D6C;
inline constexpr uint32_t kFireWire1814BootloaderModelId = 0x00010070;
inline constexpr uint32_t kInfoAddressLo = 0xC802'0000;
inline constexpr uint32_t kRequestAddressLo = 0xC802'1000;
inline constexpr uint32_t kResponseAddressLo = 0xC802'9000;
inline constexpr size_t kInfoBlockBytes = 80;
inline constexpr size_t kProtocolVersionOffset = 0x08;
inline constexpr size_t kBootloaderVersionOffset = 0x0C;
inline constexpr size_t kSoftwareDateOffset = 0x20;

// The bootloader register window is little-endian in memory. Linux writes
// cpu_to_le32 cue quadlets (bebob_maudio.c:119-127); the vendor kext passes
// native quadlets to IOFWWriteCommand (docs/MAUDIO_1814_KEXT_RE.md).
[[nodiscard]] constexpr uint32_t LoadLE32(std::span<const uint8_t> bytes,
                                          size_t offset) noexcept {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] constexpr uint64_t LoadLE64(std::span<const uint8_t> bytes,
                                          size_t offset) noexcept {
    return static_cast<uint64_t>(LoadLE32(bytes, offset)) |
           (static_cast<uint64_t>(LoadLE32(bytes, offset + 4)) << 32U);
}

struct BootRomInfo final {
    std::array<uint8_t, kInfoBlockBytes> raw{};

    [[nodiscard]] uint32_t ProtocolVersion() const noexcept {
        return LoadLE32(raw, kProtocolVersionOffset);
    }
    [[nodiscard]] uint32_t BootloaderVersion() const noexcept {
        return LoadLE32(raw, kBootloaderVersionOffset);
    }
    [[nodiscard]] bool BootloaderActive() const noexcept {
        return BootloaderVersion() != 0;
    }
    // Linux's threshold is the ASCII date "20070401" interpreted in the
    // register's native little-endian representation (bebob_maudio.c:99-113).
    [[nodiscard]] bool SupportsStoredFirmwareCue() const noexcept {
        return LoadLE64(raw, kSoftwareDateOffset) >= 0x3230303730343031ULL;
    }
};

class BeBoBBootloaderCue final {
public:
    static constexpr size_t kBytes = 12;
    explicit BeBoBBootloaderCue(const BootRomInfo& info) noexcept;
    [[nodiscard]] std::span<const uint8_t, kBytes> Bytes() const noexcept { return bytes_; }
    [[nodiscard]] uint32_t ProtocolVersion() const noexcept {
        return LoadLE32(bytes_, 0);
    }
private:
    // Only command 0x11, start application firmware, is representable here.
    static constexpr uint32_t kStartApplication = 0x0111'0000;
    static_assert(((kStartApplication >> 16U) & 0xFFU) == 0x11U);
    std::array<uint8_t, kBytes> bytes_{};
};

[[nodiscard]] bool IsPermittedBootloaderWrite(
    uint16_t addressHi, uint32_t addressLo,
    std::span<const uint8_t> payload) noexcept;

[[nodiscard]] constexpr bool IsSupportedBootloaderPersona(
    uint32_t vendorId, uint32_t modelId) noexcept {
    return vendorId == kMAudioVendorId &&
           modelId == kFireWire1814BootloaderModelId;
}

} // namespace ASFW::Protocols::BeBoB::Bootloader
