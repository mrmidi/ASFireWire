// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuRegisters.hpp - Register plane for MOTU FireWire protocol-v2 devices
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

#include "../../Wire/MOTU/MotuModel.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

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
    OpticalBanksV3 = 0x0c94,
    StreamConfigV3 = 0x0b1c,
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
// PCM chunk counts from the optical config (one arithmetic, two callers).
//
// Both the published endpoint description (before the first start) and
// PrepareDuplex (at every start) derive the stream's PCM chunk counts from the
// same register word, so the arithmetic lives here once. Fixed chunks per mode
// come from the model table; each optical direction in ADAT mode adds its own
// extra chunks, independently of the other direction
// (motu-protocol-v2.c:246-262, V2_IN_OUT_CONF read in
// snd_motu_protocol_v2_cache_packet_formats; 828mk2 and the other
// single-optical-port models). The input mode (bits 9:8) widens capture
// (TX, device->host); the output mode (bits 11:10) widens playback
// (RX, host->device).
//==============================================================================

struct V2PcmChunks {
    uint32_t tx{0};            ///< Capture (device->host) PCM chunks.
    uint32_t rx{0};            ///< Playback (host->device) PCM chunks.
    /// Exclude-differed-chunks bit per direction (EncodePacketFormat): true only
    /// when that direction provably carries just its fixed chunk count.
    bool txOnlyFixedChunks{false};
    bool rxOnlyFixedChunks{false};
    /// False for a reserved optical encoding. Counts then fall back to the fixed
    /// baseline and both exclude bits stay clear: claiming the fixed layout on
    /// unproven evidence would truncate the stream, while the conservative
    /// direction only costs the optimisation.
    bool opticalDecoded{false};
    OptIfaceMode inputMode{OptIfaceMode::None};
    OptIfaceMode outputMode{OptIfaceMode::None};
};

[[nodiscard]] constexpr V2PcmChunks ResolveV2PcmChunks(uint32_t inOutConfRaw,
                                                       uint32_t mode, uint32_t unitVersion = 3) noexcept {
    V2PcmChunks chunks{};
    const auto* model = Encoding::Motu::FindModel(unitVersion);
    if (!model || model->protocol != Encoding::Motu::ProtocolVersion::V2 || mode >= 3) return chunks;
    const uint32_t fixed =
        model->captureChunks[mode];
    const uint32_t fixedPlayback = model->playbackChunks[mode];
    const auto optical = DecodeOptIfaceConfig(inOutConfRaw);
    chunks.opticalDecoded = optical.has_value();
    const bool inputIsAdat = optical.has_value() && optical->input == OptIfaceMode::Adat;
    const bool outputIsAdat = optical.has_value() && optical->output == OptIfaceMode::Adat;
    if (optical.has_value()) {
        chunks.inputMode = optical->input;
        chunks.outputMode = optical->output;
    }
    const uint32_t adatExtra = model->optical == Encoding::Motu::OpticalLayout::None ? 0U :
        (mode == 1 && model->optical == Encoding::Motu::OpticalLayout::V2EightPre ? 8U : Encoding::Motu::AdatExtraChunks(mode));
    chunks.tx = fixed + (inputIsAdat ? adatExtra : 0U);
    chunks.rx = fixedPlayback + (outputIsAdat ? adatExtra : 0U);
    chunks.txOnlyFixedChunks = optical.has_value() && !inputIsAdat;
    chunks.rxOnlyFixedChunks = optical.has_value() && !outputIsAdat;
    return chunks;
}

// V1 clock/optical/fetch codecs. Linux motu-protocol-v1.c:10-118,186-248,
// 336-380,394-451. Original 828 shares clock and isoch control at 0xb00;
// its clock/fetch writes MUST clear the upper half (command strobes).
[[nodiscard]] constexpr std::optional<uint32_t> DecodeRateV1(uint32_t raw, uint32_t version) noexcept {
    if (version == 1) return (raw & 4) ? 48000U : 44100U;
    if (version == 2) return Encoding::Motu::kClockRates[(raw >> 3) & 3];
    return std::nullopt;
}
[[nodiscard]] constexpr std::optional<uint32_t> EncodeRateV1(uint32_t raw, uint32_t rate, uint32_t version) noexcept {
    const auto* model = Encoding::Motu::FindModel(version);
    if (!model || !Encoding::Motu::SupportsRate(*model, rate)) return std::nullopt;
    if (version == 1) return (raw & 0xffffU & ~4U) | (rate == 48000 ? 4U : 0U);
    if (version == 2) return (raw & ~0x18U) | (static_cast<uint32_t>(Encoding::Motu::RateToIndex(rate)) << 3);
    return std::nullopt;
}
[[nodiscard]] constexpr uint32_t EncodeFetchingV1(uint32_t raw, bool enable, uint32_t version) noexcept {
    if (version == 1) return (raw & 0xffffU & ~0x88U) | (enable ? 0x88U : 0U);
    // 896 output-on bits are irreversible; preserve them when fetching stops.
    return (raw & ~0xf0000000U) | (enable ? 0x23000000U : 0U);
}
[[nodiscard]] constexpr std::optional<ClockSourceV2> DecodeClockSourceV1(uint32_t raw, uint32_t version) noexcept {
    if (version == 2) {
        if ((raw & 7) == 2) return ClockSourceV2::AesEbuOnXlr;
        return DecodeClockSourceV2(raw);
    }
    switch (raw & 0x23) {
    case 0: return ClockSourceV2::Internal;
    case 1: return ClockSourceV2::AdatOnDsub;
    case 2: return ClockSourceV2::Spdif;
    case 3: return ClockSourceV2::Sph;
    case 0x21: return ClockSourceV2::AdatOnOpt;
    default: return std::nullopt;
    }
}

// V3 register codecs: Linux motu-protocol-v3.c:11-38,180-235.
[[nodiscard]] constexpr std::optional<uint32_t> DecodeRateV3(uint32_t raw) noexcept {
    const auto index = (raw >> 8) & 0xff;
    return index < Encoding::Motu::kClockRateCount ?
        std::optional<uint32_t>{Encoding::Motu::kClockRates[index]} : std::nullopt;
}
[[nodiscard]] constexpr std::optional<ClockSourceV2> DecodeClockSourceV3(uint32_t raw) noexcept {
    switch (raw & 0xff) {
    case 0: return ClockSourceV2::Internal;
    case 1: return ClockSourceV2::WordOnBnc;
    case 2: return ClockSourceV2::Sph;
    case 8: return ClockSourceV2::AesEbuOnXlr;
    case 0x10: return ClockSourceV2::Spdif;
    case 0x18: case 0x19: return ClockSourceV2::AdatOnOpt; // bank refinement is separate
    default: return std::nullopt;
    }
}
[[nodiscard]] constexpr V2PcmChunks ResolvePcmChunks(uint32_t raw, uint32_t mode, uint32_t version) noexcept {
    const auto* model = Encoding::Motu::FindModel(version);
    if (!model || mode >= 3 || (model->unresolvedModes & (1U << mode))) return {};
    if (model->protocol == Encoding::Motu::ProtocolVersion::V1) {
        V2PcmChunks chunks{.tx = model->captureChunks[mode], .rx = model->playbackChunks[mode], .opticalDecoded = true};
        if (!chunks.tx || !chunks.rx) return {};
        if (version == 1) {
            if (!(raw & 0x8000)) chunks.tx += 8;
            if (!(raw & 0x4000)) chunks.rx += 8;
        } else {
            // 896 has no optical mode register: reserve eight ADAT chunks at
            // 1x. The unresolved vendor/Linux 2x divergence is withheld by the model.
            chunks.tx += 8; chunks.rx += 8;
        }
        chunks.txOnlyFixedChunks = chunks.tx == model->captureChunks[mode];
        chunks.rxOnlyFixedChunks = chunks.rx == model->playbackChunks[mode];
        return chunks;
    }
    if (model->protocol == Encoding::Motu::ProtocolVersion::V2) return ResolveV2PcmChunks(raw, mode, version);
    if (model->protocol != Encoding::Motu::ProtocolVersion::V3 || (model->unresolvedModes & (1U << mode))) return {};
    V2PcmChunks chunks{.tx = model->captureChunks[mode], .rx = model->playbackChunks[mode], .opticalDecoded = true};
    if (!chunks.tx || !chunks.rx) return {};
    if (model->optical == Encoding::Motu::OpticalLayout::V3Banks && mode < 2) {
        const auto extra = [mode](bool enabled, bool spdif) { return !enabled ? 0U : (spdif ? 4U : (mode == 0 ? 8U : 4U)); };
        chunks.tx += extra(raw & 1, raw & 0x10000) + extra(raw & 2, raw & 0x100000);
        chunks.rx += extra(raw & 0x100, raw & 0x40000) + extra(raw & 0x200, raw & 0x400000);
        // Vendor hybrid S/PDIF counts differ from Linux. Withhold this shape.
        if (version == 0x35 && (((raw & 1) && (raw & 0x10000)) || ((raw & 2) && (raw & 0x100000)) ||
            ((raw & 0x100) && (raw & 0x40000)) || ((raw & 0x200) && (raw & 0x400000)))) return {};
    }
    chunks.txOnlyFixedChunks = chunks.tx == model->captureChunks[mode];
    chunks.rxOnlyFixedChunks = chunks.rx == model->playbackChunks[mode];
    return chunks;
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

//==============================================================================
// Fetching mode (motu-protocol-v2.c:190-225)
//==============================================================================

/// 828mk2 (Altera ACEX 1K) and 896HD (Altera Cyclone) need no fetching-mode write around
/// streaming. Every other v2 model does; the UltraLite and 8pre implement a Xilinx
/// Spartan XC3S200 and take the variant below.
[[nodiscard]] constexpr bool NeedsFetchingModeWrite(uint32_t unitSwVersion) noexcept {
    return unitSwVersion != DeviceProfiles::Audio::kMotu828mk2SwVersion &&
           unitSwVersion != DeviceProfiles::Audio::kMotu896hdSwVersion;
}

inline constexpr uint32_t kClockFetchEnable = 0x02000000u;
inline constexpr uint32_t kClockModelSpecific = 0x04000000u;

/// Read-modify-write for the clock status register when enabling or disabling fetching.
///
/// The Spartan variant additionally sets the model-specific bit, but only when the device
/// is slaved to source packet headers above 48 kHz (switch_fetching_mode_spartan,
/// motu-protocol-v2.c:168-189). At 44.1/48 kHz on any clock source, and at any rate on an
/// internal clock, it reduces to the plain fetch-enable write.
[[nodiscard]] constexpr uint32_t EncodeFetchingMode(uint32_t currentData,
                                                    bool enable,
                                                    bool spartan, bool traveler = false) noexcept {
    uint32_t data = currentData & ~(kClockFetchEnable | kClockModelSpecific);
    if (enable) {
        data |= kClockFetchEnable;
    }
    if (traveler) data |= kClockModelSpecific;
    if (spartan) {
        const auto source = DecodeClockSourceV2(currentData);
        const auto rate = DecodeRateV2(currentData);
        if (source.has_value() && *source == ClockSourceV2::Sph && rate.has_value() &&
            *rate > 48000U) {
            data |= kClockModelSpecific;
        }
    }
    return data;
}

} // namespace ASFW::Audio::Motu
