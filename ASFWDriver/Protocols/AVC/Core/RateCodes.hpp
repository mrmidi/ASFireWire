// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RateCodes.hpp - The two sampling-rate code sets AV/C carries, as separate types.
//
// They are NOT interchangeable, and mixing them is a real bug this codebase has
// shipped: the generic BeBoB code decoded stream-format rate codes with the CIP
// table, reading the Phase 88's 0x02 (32 kHz) as 48 kHz. Each field gets its own
// type so the compiler refuses the mix:
//
//   code   CipSfc (signal format FDF)   StreamFormatRate (stream format)
//   0x00   32 kHz                        22.05 kHz
//   0x02   48 kHz                        32 kHz
//   0x04   96 kHz                        48 kHz
//   0x0A   -                             88.2 kHz
//
// A third set, Apple's kMusicSubunitSampleRate_* (AVCVideoServices
// MusicSubunitController.h:87-93), belongs to a music-subunit field and is not
// modelled here.

#pragma once

#include <cstdint>
#include <optional>

namespace ASFW::AVC {

/// IEC 61883-6 SFC: the rate code in the AM824 FDF (low 3 bits of FDF byte 0)
/// of INPUT/OUTPUT PLUG SIGNAL FORMAT. Linux fcp.c:59 ("0x07 & sfc") with the
/// CIP rate table (amdtp_rate_table: 32k, 44.1k, 48k, 88.2k, 96k, 176.4k, 192k).
enum class CipSfc : uint8_t {
    k32000 = 0x00,
    k44100 = 0x01,
    k48000 = 0x02,
    k88200 = 0x03,
    k96000 = 0x04,
    k176400 = 0x05,
    k192000 = 0x06,
};

/// Rate code of a compound AM824 stream format (STREAM FORMAT SUPPORT 0x2F /
/// EXTENDED STREAM FORMAT 0xBF). Holds any byte the device sent; ToHz() says
/// whether it is known.
///   ta1394 stream-format/src/lib.rs:535-543
///   Linux bebob_stream.c:38-46 (bridgeco_freq_table), oxfw-stream.c:32-39
///   Apple AVCVideoServices MusicSubunitController.cpp:1262-1299
///   Confirmed on hardware: Phase 88 lists 0x02 0x03 0x04 0x0a 0x05 = 32..96 kHz.
enum class StreamFormatRate : uint8_t {
    k22050 = 0x00,
    k24000 = 0x01,
    k32000 = 0x02,
    k44100 = 0x03,
    k48000 = 0x04,
    k96000 = 0x05,
    k176400 = 0x06,
    k192000 = 0x07,
    k88200 = 0x0A,
};

[[nodiscard]] constexpr std::optional<uint32_t> ToHz(CipSfc code) noexcept {
    switch (code) {
        case CipSfc::k32000: return 32000U;
        case CipSfc::k44100: return 44100U;
        case CipSfc::k48000: return 48000U;
        case CipSfc::k88200: return 88200U;
        case CipSfc::k96000: return 96000U;
        case CipSfc::k176400: return 176400U;
        case CipSfc::k192000: return 192000U;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<uint32_t> ToHz(StreamFormatRate code) noexcept {
    switch (code) {
        case StreamFormatRate::k22050: return 22050U;
        case StreamFormatRate::k24000: return 24000U;
        case StreamFormatRate::k32000: return 32000U;
        case StreamFormatRate::k44100: return 44100U;
        case StreamFormatRate::k48000: return 48000U;
        case StreamFormatRate::k96000: return 96000U;
        case StreamFormatRate::k176400: return 176400U;
        case StreamFormatRate::k192000: return 192000U;
        case StreamFormatRate::k88200: return 88200U;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<CipSfc> CipSfcFromHz(uint32_t hz) noexcept {
    for (uint8_t raw = 0; raw <= 0x06; ++raw) {
        if (ToHz(static_cast<CipSfc>(raw)) == hz) {
            return static_cast<CipSfc>(raw);
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<StreamFormatRate> StreamFormatRateFromHz(uint32_t hz) noexcept {
    constexpr StreamFormatRate kAll[] = {
        StreamFormatRate::k22050, StreamFormatRate::k24000, StreamFormatRate::k32000,
        StreamFormatRate::k44100, StreamFormatRate::k48000, StreamFormatRate::k88200,
        StreamFormatRate::k96000, StreamFormatRate::k176400, StreamFormatRate::k192000,
    };
    for (const auto code : kAll) {
        if (ToHz(code) == hz) {
            return code;
        }
    }
    return std::nullopt;
}

static_assert(ToHz(CipSfc::k48000) == 48000U);
static_assert(ToHz(StreamFormatRate::k48000) == 48000U);
static_assert(static_cast<uint8_t>(CipSfc::k48000) != static_cast<uint8_t>(StreamFormatRate::k48000),
              "the two code sets differ; that is the point of two types");
static_assert(ToHz(static_cast<StreamFormatRate>(0x0B)) == std::nullopt);
static_assert(CipSfcFromHz(48000U) == CipSfc::k48000);
static_assert(StreamFormatRateFromHz(88200U) == StreamFormatRate::k88200);

} // namespace ASFW::AVC
