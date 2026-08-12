// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "BeBoBBootloaderCue.hpp"

namespace ASFW::Audio::Families::BeBoB::Bootloader {

namespace {

/// Little-endian, matching the device's register window. See the endianness
/// note in the header before changing this.
void StoreLittleEndianQuadlet(std::span<uint8_t> bytes, size_t offset,
                              uint32_t value) noexcept {
    bytes[offset] = static_cast<uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    bytes[offset + 2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    bytes[offset + 3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

} // namespace

BeBoBBootloaderCue::BeBoBBootloaderCue(const BootRomInfo& info) noexcept {
    // Quadlet 0 is the only variable, and it is read live from the device
    // rather than assumed. Linux hardcodes 1 here; the vendor kext reads
    // info+0x08, which is the safer behaviour on firmware we have not seen.
    StoreLittleEndianQuadlet(bytes_, 0, info.ProtocolVersion());
    StoreLittleEndianQuadlet(bytes_, 4, kStartApplicationFirmware);
    StoreLittleEndianQuadlet(bytes_, 8, kApplicationStartMode);
}

uint32_t BeBoBBootloaderCue::ProtocolVersion() const noexcept {
    return LoadLittleEndianQuadlet(bytes_, 0);
}

bool IsPermittedBootloaderWrite(uint16_t addressHi, uint32_t addressLo,
                                std::span<const uint8_t> payload) noexcept {
    // Exactly the request register. A correct payload at the info or response
    // register, or anywhere else in the window, is still refused.
    if (addressHi != kBootloaderAddressHi || addressLo != kRequestAddressLo) {
        return false;
    }
    if (payload.size() != BeBoBBootloaderCue::kCueBytes) {
        return false;
    }
    // Quadlet 0 carries the device-supplied protocol version and is free.
    // Quadlets 1 and 2 are the command itself and must be bit-identical: this
    // is what stops a stray byte turning the cue into ProgramGUID (0x0a) or
    // ProgramHWIdVersion (0x10).
    const uint32_t command = LoadLittleEndianQuadlet(payload, 4);
    const uint32_t startMode = LoadLittleEndianQuadlet(payload, 8);
    return command == 0x0111'0000U && startMode == 0x0000'0000U;
}

} // namespace ASFW::Audio::Families::BeBoB::Bootloader
