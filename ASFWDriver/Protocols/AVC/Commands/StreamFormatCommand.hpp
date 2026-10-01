// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// StreamFormatCommand.hpp - Stream format query and set: STREAM FORMAT SUPPORT
// (0x2F, TA 2001002) and EXTENDED STREAM FORMAT INFORMATION (0xBF, unpublished
// draft). One codec; the opcode is a value the caller passes.
//
// Which opcode a device answers is per-unit data, never a blind probe:
//   Phase 88 (BridgeCo) answers 0x2F only; 0xBF gives NOT IMPLEMENTED (MCP, 2026-09-27).
//   Oxford devices answer 0xBF (Linux oxfw-command.c:22).
//   Apple AppleFWAudio tries 0xBF then 0x2F on every call; AVCVideoServices
//   MusicSubunitController.cpp:1785-1788 remembers the fallback. The transaction engine chooses the opcode from per-unit evidence.
//
// Layouts: ta1394 stream-format/src/lib.rs (MIT), cited per item; Linux
// bebob_command.c:289-331 (0x2F list, BridgeCo). Fresh clean-room implementation.
//
// Operands (both opcodes):
//   single (C0): [C0][plug address, 5][support status][format...]
//   list   (C1): [C1][plug address, 5][support status][index][format...]
//   STATUS sends support status 0xFF and no format; the response carries the format.
//   Verified byte for byte against the Phase 88 capture:
//     cmd  01 FF 2F C1 | 00 00 00 00 FF | FF | 00
//     resp 0C FF 2F C1 | 00 00 00 00 FF | FF | 00 | 90 40 02 01 03 08 06 02 00 01 0D

#pragma once

#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"
#include "../Core/RateCodes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::AVC::Cmd {

enum class StreamFormatOpcode : uint8_t {
    kStreamFormatSupport = 0x2F,   ///< TA 2001002; BridgeCo (Linux bebob_command.c:302)
    kExtendedStreamFormat = 0xBF,  ///< draft; Oxford (Linux oxfw-command.c:22)
};

enum class StreamFormatSubfunction : uint8_t {
    kSingle = 0xC0,  ///< ta1394 lib.rs:1175
    kList = 0xC1,    ///< ta1394 lib.rs:1250
};

// ---------------------------------------------------------------------------
// Plug address: 5 bytes [direction][mode][3 mode bytes, 0xFF-padded].
// ta1394 lib.rs:785-1060 (UnitPlugType, PlugAddrMode, PlugDirection, PlugAddr).
// The BridgeCo extended PLUG INFO uses the same 5 bytes (Linux bebob.h:172-195),
// so Extensions/BridgeCoPlugInfo.hpp reuses this type.
// ---------------------------------------------------------------------------

enum class PlugDirection : uint8_t {
    kInput = 0x00,   ///< the device receives on it (host playback)
    kOutput = 0x01,  ///< the device sends on it (host capture)
};

enum class PlugAddressMode : uint8_t {
    kUnit = 0x00,
    kSubunit = 0x01,
    kFunctionBlock = 0x02,
};

enum class UnitPlugType : uint8_t {
    kPcr = 0x00,       ///< isochronous (PCR) plug
    kExternal = 0x01,
    kAsync = 0x02,
};

struct PlugAddress {
    PlugDirection direction{PlugDirection::kInput};
    PlugAddressMode mode{PlugAddressMode::kUnit};
    UnitPlugType unitPlugType{UnitPlugType::kPcr};  ///< kUnit only
    uint8_t functionBlockType{0xFF};                ///< kFunctionBlock only
    uint8_t functionBlockId{0xFF};                  ///< kFunctionBlock only
    uint8_t plugId{0};

    [[nodiscard]] static constexpr PlugAddress UnitPlug(PlugDirection direction, UnitPlugType type,
                                                        uint8_t plugId) noexcept {
        return PlugAddress{direction, PlugAddressMode::kUnit, type, 0xFF, 0xFF, plugId};
    }
    [[nodiscard]] static constexpr PlugAddress SubunitPlug(PlugDirection direction, uint8_t plugId) noexcept {
        return PlugAddress{direction, PlugAddressMode::kSubunit, UnitPlugType::kPcr, 0xFF, 0xFF, plugId};
    }
    [[nodiscard]] static constexpr PlugAddress FunctionBlockPlug(PlugDirection direction, uint8_t fbType,
                                                                 uint8_t fbId, uint8_t plugId) noexcept {
        return PlugAddress{direction, PlugAddressMode::kFunctionBlock, UnitPlugType::kPcr, fbType, fbId,
                           plugId};
    }

    /// Unit:          [dir][00][type][plug][FF]
    /// Subunit:       [dir][01][plug][FF][FF]
    /// FunctionBlock: [dir][02][fb type][fb id][plug]
    [[nodiscard]] constexpr std::array<uint8_t, 5> Encode() const noexcept {
        std::array<uint8_t, 5> raw{static_cast<uint8_t>(direction), static_cast<uint8_t>(mode), 0xFF, 0xFF, 0xFF};
        switch (mode) {
            case PlugAddressMode::kUnit:
                raw[2] = static_cast<uint8_t>(unitPlugType);
                raw[3] = plugId;
                break;
            case PlugAddressMode::kSubunit:
                raw[2] = plugId;
                break;
            case PlugAddressMode::kFunctionBlock:
                raw[2] = functionBlockType;
                raw[3] = functionBlockId;
                raw[4] = plugId;
                break;
        }
        return raw;
    }

    /// kOperandsTooShort under 5 bytes; kMalformedOperands (offset 0 or 1) for an
    /// unknown direction or mode, or (offset 2) an unknown unit plug type.
    [[nodiscard]] static Expected<PlugAddress> Decode(std::span<const uint8_t> raw) noexcept;

    friend constexpr bool operator==(const PlugAddress&, const PlugAddress&) noexcept = default;
};

// Phase 88 input plug 0, as in the capture above.
static_assert(PlugAddress::UnitPlug(PlugDirection::kInput, UnitPlugType::kPcr, 0).Encode() ==
              std::array<uint8_t, 5>{0x00, 0x00, 0x00, 0x00, 0xFF});
// Linux bebob.h:185-195 music-subunit plug address, operand part.
static_assert(PlugAddress::SubunitPlug(PlugDirection::kOutput, 2).Encode() ==
              std::array<uint8_t, 5>{0x01, 0x01, 0x02, 0xFF, 0xFF});

/// ta1394 lib.rs:1080-1083.
enum class SupportStatus : uint8_t {
    kActive = 0x00,
    kInactive = 0x01,
    kNoStreamFormat = 0x02,
    kNotUsed = 0xFF,
};

// ---------------------------------------------------------------------------
// Format block. Root 0x90 = AM (ta1394 lib.rs:724); level 1: 0x00 AM824,
// 0x40 compound AM824 (lib.rs:646-649).
// Compound AM824: [90][40][rate][flags][entry count][count, format]...
//   rate:  StreamFormatRate (lib.rs:535-543)
//   flags: bit 2 = sync source, bits 1..0 = rate control (lib.rs:545-549, 570-572)
//   entry: [channel count][format code] (lib.rs:454-462)
// ---------------------------------------------------------------------------

inline constexpr uint8_t kFormatRootAm = 0x90;
inline constexpr uint8_t kFormatLevel1Am824 = 0x00;
inline constexpr uint8_t kFormatLevel1CompoundAm824 = 0x40;

/// Compound AM824 entry format codes. ta1394 lib.rs:379-392. Holds any byte the
/// device sent; unknown values pass through unchanged.
enum class Am824Format : uint8_t {
    kIec60958_3 = 0x00,  ///< IEC 60958 conformant; Linux treats it as MBLA (Phase 88: 2 channels)
    kIec61937_3 = 0x01,
    kIec61937_4 = 0x02,
    kIec61937_5 = 0x03,
    kIec61937_6 = 0x04,
    kIec61937_7 = 0x05,
    kMultiBitLinearAudioRaw = 0x06,  ///< MBLA (Phase 88: 8 channels)
    kMultiBitLinearAudioDvd = 0x07,
    kHighPrecisionMultiBitLinearAudio = 0x0C,
    kMidiConformant = 0x0D,
    kSmpteTimeCode = 0x0E,
    kSampleCount = 0x0F,
    kAncillaryData = 0x10,
    kSyncStream = 0x40,
};

/// ta1394 lib.rs:473-489.
enum class RateControl : uint8_t {
    kSupported = 0x00,
    kDontCare = 0x01,
    kNotSupported = 0x02,
};

struct CompoundEntry {
    uint8_t count{0};
    Am824Format format{Am824Format::kMultiBitLinearAudioRaw};
    friend constexpr bool operator==(const CompoundEntry&, const CompoundEntry&) noexcept = default;
};

/// Fixed capacity; a response with more entries fails with kUnsupported.
inline constexpr size_t kMaxCompoundEntries = 32;

struct CompoundAm824 {
    StreamFormatRate rate{StreamFormatRate::k48000};
    bool syncSource{false};
    RateControl rateControl{RateControl::kSupported};
    std::array<CompoundEntry, kMaxCompoundEntries> entries{};
    uint8_t entryCount{0};

    [[nodiscard]] constexpr std::span<const CompoundEntry> Entries() const noexcept {
        return {entries.data(), entryCount};
    }

    /// Total channels of one format: CountOf(kMidiConformant) = MIDI slots.
    [[nodiscard]] constexpr uint32_t CountOf(Am824Format format) const noexcept {
        uint32_t total = 0;
        for (const auto& entry : Entries()) {
            if (entry.format == format) {
                total += entry.count;
            }
        }
        return total;
    }

    /// PCM channels as Linux counts them: IEC 60958-3 ("currently handled as
    /// MBLA") plus MBLA raw. Linux bebob_stream.c:733-738.
    [[nodiscard]] constexpr uint32_t PcmChannels() const noexcept {
        return CountOf(Am824Format::kIec60958_3) + CountOf(Am824Format::kMultiBitLinearAudioRaw);
    }

    [[nodiscard]] constexpr uint32_t MidiChannels() const noexcept {
        return CountOf(Am824Format::kMidiConformant);  // bebob_stream.c:740-742
    }

    /// True when every entry is PCM (0x00, 0x06) or MIDI (0x0d). Linux rejects a
    /// formation with any other entry (-ENOSYS, bebob_stream.c:743-762). The codec
    /// still parses such blocks; whether to use them is family policy.
    [[nodiscard]] constexpr bool OnlyPcmAndMidi() const noexcept {
        for (const auto& entry : Entries()) {
            if (entry.format != Am824Format::kIec60958_3 &&
                entry.format != Am824Format::kMultiBitLinearAudioRaw &&
                entry.format != Am824Format::kMidiConformant) {
                return false;
            }
        }
        return true;
    }
};

/// A parsed format block. Raw bytes are owned so the reply can outlive the FCP buffer.
/// `compound` is filled only for kCompoundAm824.
struct StreamFormat {
    enum class Kind : uint8_t { kCompoundAm824, kOther };
    Kind kind{Kind::kOther};
    CompoundAm824 compound{};
    std::array<uint8_t, kMaxOperandBytes> rawBytes{};
    uint16_t rawLength{0};

    [[nodiscard]] std::span<const uint8_t> Raw() const noexcept {
        return {rawBytes.data(), rawLength};
    }
};

/// Parse a format block (starting at the 0x90 root). Anything that is not a
/// compound AM824 block is Kind::kOther with `raw` set, not an error. Errors:
/// kOperandsTooShort, kMalformedOperands (entry count disagrees with the bytes),
/// kUnsupported (more than kMaxCompoundEntries).
[[nodiscard]] Expected<StreamFormat> DecodeStreamFormatBlock(std::span<const uint8_t> block) noexcept;

/// Encode a compound AM824 block into `out`; returns the byte count
/// (5 + 2 * entryCount). kFrameTooLong when `out` is too small.
[[nodiscard]] Expected<size_t> EncodeCompoundAm824(const CompoundAm824& format, std::span<uint8_t> out) noexcept;

// ---------------------------------------------------------------------------
// STREAM FORMAT SUPPORT and EXTENDED STREAM FORMAT INFORMATION
// ---------------------------------------------------------------------------

struct StreamFormatReply {
    StreamFormatSubfunction form{StreamFormatSubfunction::kSingle};
    PlugAddress plug{};
    SupportStatus status{SupportStatus::kNotUsed};
    uint8_t index{0};
    StreamFormat format{};
};

struct StreamFormatOperands {
    static constexpr Opcode kOpcode = Opcode::kStreamFormatSupport;

    StreamFormatSubfunction form{StreamFormatSubfunction::kSingle};
    StreamFormatOpcode opcode{StreamFormatOpcode::kStreamFormatSupport};
    PlugAddress plug{};
    uint8_t index{0};
    std::optional<CompoundAm824> controlFormat{std::nullopt};

    using Reply = StreamFormatReply;

    [[nodiscard]] constexpr Opcode GetOpcode() const noexcept {
        return static_cast<Opcode>(opcode);
    }

    [[nodiscard]] constexpr Expected<void> ValidateAddress(SubunitAddress address) const noexcept {
        if (plug.mode == PlugAddressMode::kUnit && !address.IsUnit()) {
            return Fail(AvcErrorKind::kInvalidArgument);
        }
        if (plug.mode == PlugAddressMode::kSubunit && address.IsUnit()) {
            return Fail(AvcErrorKind::kInvalidArgument);
        }
        return {};
    }

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept;
    [[nodiscard]] Expected<Reply> Read(std::span<const uint8_t> in) const noexcept;
};

using StreamFormatCommand = Command<StreamFormatOperands>;

} // namespace ASFW::AVC::Cmd
