// SPDX-License-Identifier: Apache-2.0
#include "BeBoBBootloaderCue.hpp"

namespace ASFW::Protocols::BeBoB::Bootloader {
namespace {
void StoreLE32(std::span<uint8_t> bytes, size_t offset, uint32_t value) noexcept {
    for (size_t i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8U)) & 0xFFU);
}
}

BeBoBBootloaderCue::BeBoBBootloaderCue(const BootRomInfo& info) noexcept {
    StoreLE32(bytes_, 0, info.ProtocolVersion());
    StoreLE32(bytes_, 4, kStartApplication);
    StoreLE32(bytes_, 8, 0);
}

bool IsPermittedBootloaderWrite(uint16_t addressHi, uint32_t addressLo,
                                std::span<const uint8_t> payload) noexcept {
    if (addressHi != kAddressHi || addressLo != kRequestAddressLo ||
        payload.size() != BeBoBBootloaderCue::kBytes) return false;
    return LoadLE32(payload, 4) == 0x0111'0000U && LoadLE32(payload, 8) == 0;
}
} // namespace ASFW::Protocols::BeBoB::Bootloader
