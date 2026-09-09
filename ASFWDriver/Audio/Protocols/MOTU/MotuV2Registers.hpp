// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuV2Registers.hpp - Register plane for MOTU FireWire protocol-v2 devices
// (828mk2 first; family covers Traveler, UltraLite, 896HD, 8pre).
//
// Pure value codecs; no transport. The eventual IDeviceProtocol issues these
// as async quadlet transactions at kAddrBase + offset (big-endian wire).
//
// Wire truth cross-validated with Linux sound/firewire/motu:
//   - motu-transaction.c:11-13   (address base, async message registers)
//   - motu-protocol-v2.c:10-32   (clock status and optical config registers)
//   - motu-stream.c:12-26        (iso comm control and packet format registers)
//   - motu-protocol-v2.c:190-225 (fetching mode; 828mk2/896HD are no-ops)
//   - motu.c:16-26               (clock rate table)

#pragma once

#include "../../Wire/MOTU/MotuBlockLayout.hpp"
#include <cstdint>
#include <optional>

namespace ASFW::Audio::Motu {

inline constexpr uint64_t kAddrBase = 0xfffff0000000ull;

enum class Reg : uint32_t {
    IsocCommControl = 0x0b00,
    AsyncAddrHi = 0x0b04,
    AsyncAddrLo = 0x0b08,
    PacketFormat = 0x0b10,
    ClockStatusV2 = 0x0b14,
    InOutConfV2 = 0x0c04,
};

//==============================================================================
// Clock status register (0x0b14): rate index in bits [5:3], source in [2:0]
// (motu-protocol-v2.c:10-23).
//==============================================================================

/// Raw v2 clock source codes (motu-protocol-v2.c:15-21). SPDIF refinement
/// (coax vs optical, AES/EBU on 896HD) additionally consults the optical
/// config register; that model-specific mapping lives with the protocol.
enum class ClockSourceV2 : uint32_t {
    Internal = 0,
    AdatOnOpt = 1,
    Spdif = 2,
    Sph = 3,        ///< Sync to source packet headers (bus clock).
    WordOnBnc = 4,
    AdatOnDsub = 5,
    AesEbuOnXlr = 7,
};

[[nodiscard]] constexpr std::optional<uint32_t> DecodeRateV2(
    uint32_t data) noexcept {
    const uint32_t index = (data & 0x00000038u) >> 3;
    if (index >= Encoding::Motu::kClockRateCount) {
        return std::nullopt;
    }
    return Encoding::Motu::kClockRates[index];
}

/// Read-modify-write value for a rate change; all other bits preserved
/// (motu-protocol-v2.c:59-86).
[[nodiscard]] constexpr std::optional<uint32_t> EncodeRateV2(
    uint32_t currentData, uint32_t rate) noexcept {
    const int32_t index = Encoding::Motu::RateToIndex(rate);
    if (index < 0) {
        return std::nullopt;
    }
    return (currentData & ~0x00000038u) |
           (static_cast<uint32_t>(index) << 3);
}

[[nodiscard]] constexpr std::optional<ClockSourceV2> DecodeClockSourceV2(
    uint32_t data) noexcept {
    switch (data & 0x00000007u) {
    case 0: return ClockSourceV2::Internal;
    case 1: return ClockSourceV2::AdatOnOpt;
    case 2: return ClockSourceV2::Spdif;
    case 3: return ClockSourceV2::Sph;
    case 4: return ClockSourceV2::WordOnBnc;
    case 5: return ClockSourceV2::AdatOnDsub;
    case 7: return ClockSourceV2::AesEbuOnXlr;
    default: return std::nullopt;
    }
}

//==============================================================================
// Optical interface config (0x0c04): out mode bits [11:10], in mode [9:8]
// (motu-protocol-v2.c:25-32).
//==============================================================================

enum class OptIfaceMode : uint32_t {
    None = 0,
    Adat = 1,
    Spdif = 2,
};

struct OptIfaceConfig {
    OptIfaceMode input;
    OptIfaceMode output;
};

[[nodiscard]] constexpr std::optional<OptIfaceConfig> DecodeOptIfaceConfig(
    uint32_t data) noexcept {
    const uint32_t in = (data & 0x00000300u) >> 8;
    const uint32_t out = (data & 0x00000c00u) >> 10;
    if (in > 2u || out > 2u) {
        return std::nullopt;
    }
    return OptIfaceConfig{static_cast<OptIfaceMode>(in),
                          static_cast<OptIfaceMode>(out)};
}

//==============================================================================
// Iso communication control (0x0b00): channel numbers + activation for both
// directions in one write (motu-stream.c:12-21,62-107). Device-relative
// naming: RX = host->device (playback), TX = device->host (capture).
//==============================================================================

inline constexpr uint32_t kIsoCommControlMask = 0xffff0000u;
inline constexpr uint32_t kChangeRxState = 0x80000000u;
inline constexpr uint32_t kRxActivated = 0x40000000u;
inline constexpr uint32_t kChangeTxState = 0x00800000u;
inline constexpr uint32_t kTxActivated = 0x00400000u;

/// Activate both streams with their iso channels; low 16 bits preserved
/// (motu-stream.c:62-83).
[[nodiscard]] constexpr uint32_t EncodeIsoCommStart(uint32_t currentData,
                                                    uint32_t deviceRxChannel,
                                                    uint32_t deviceTxChannel) noexcept {
    uint32_t data = currentData & ~kIsoCommControlMask;
    data |= kChangeRxState | kRxActivated | ((deviceRxChannel & 0x3fu) << 24);
    data |= kChangeTxState | kTxActivated | ((deviceTxChannel & 0x3fu) << 16);
    return data;
}

/// Deactivate both streams, leaving channel fields intact
/// (motu-stream.c:85-107).
[[nodiscard]] constexpr uint32_t EncodeIsoCommStop(uint32_t currentData) noexcept {
    uint32_t data = currentData;
    data &= ~(kRxActivated | kTxActivated);
    data |= kChangeRxState | kChangeTxState;
    return data;
}

struct IsoCommState {
    bool rxActivated;
    uint32_t rxChannel;
    bool txActivated;
    uint32_t txChannel;
};

[[nodiscard]] constexpr IsoCommState DecodeIsoCommState(uint32_t data) noexcept {
    return IsoCommState{
        (data & kRxActivated) != 0,
        (data >> 24) & 0x3fu,
        (data & kTxActivated) != 0,
        (data >> 16) & 0x3fu,
    };
}

//==============================================================================
// Packet format register (0x0b10) (motu-stream.c:23-26,201-225)
//==============================================================================

inline constexpr uint32_t kTxExcludeDifferedChunks = 0x00000080u;
inline constexpr uint32_t kRxExcludeDifferedChunks = 0x00000040u;
inline constexpr uint32_t kTxSpeedMask = 0x0000000fu;

/// Read-modify-write for the packet format: exclude-differed-chunks flags are
/// set when the direction runs only its fixed chunks (optical not in ADAT
/// mode); speed is the FireWire speed code (motu-stream.c:201-225).
[[nodiscard]] constexpr uint32_t EncodePacketFormat(uint32_t currentData,
                                                    bool txOnlyFixedChunks,
                                                    bool rxOnlyFixedChunks,
                                                    uint32_t speedCode) noexcept {
    uint32_t data = currentData &
                    ~(kTxExcludeDifferedChunks | kRxExcludeDifferedChunks |
                      kTxSpeedMask);
    if (txOnlyFixedChunks) {
        data |= kTxExcludeDifferedChunks;
    }
    if (rxOnlyFixedChunks) {
        data |= kRxExcludeDifferedChunks;
    }
    data |= speedCode & kTxSpeedMask;
    return data;
}

//==============================================================================
// Async message address registration (0x0b04/0x0b08): the device writes
// 4-byte notifications to this host address; re-register after every bus
// reset (motu-transaction.c:74-121).
//==============================================================================

struct AsyncAddrValues {
    uint32_t hi;  ///< (host nodeID << 16) | (address >> 32)
    uint32_t lo;  ///< low 32 bits of address
};

[[nodiscard]] constexpr AsyncAddrValues EncodeAsyncAddr(
    uint16_t hostNodeId, uint64_t hostAddress) noexcept {
    return AsyncAddrValues{
        (static_cast<uint32_t>(hostNodeId) << 16) |
            static_cast<uint32_t>(hostAddress >> 32),
        static_cast<uint32_t>(hostAddress),
    };
}

/// Device-claimed host address region for notifications
/// (motu-transaction.c:98-101).
inline constexpr uint64_t kAsyncMessageRegionStart = 0xffffe0000000ull;
inline constexpr uint64_t kAsyncMessageRegionEnd = 0xffffe000ffffull;

/// 828mk2 and 896HD require no fetching-mode write around streaming; other
/// v2 models do (motu-protocol-v2.c:190-225). Encoded here as a capability
/// flag for the protocol layer.
inline constexpr bool k828mk2NeedsFetchingModeWrite = false;

} // namespace ASFW::Audio::Motu
