// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FirefaceSettings.hpp - the settings a Fireface 400/800 keeps in flash, and
// the configuration register built from them. Pure functions, no bus I/O.
//
// The configuration register (FF800 0xfc88f014, FF400 0x80100514, 3 quadlets)
// is write-only and the card waits for the host to send it after power-up; its
// red HOST LED says that has not happened yet (FFADO fireface_hw.cpp:84-89).
// The values come from the card's own flash, so sending them changes nothing
// the user saved. Bit choices and the references behind each are in
// FirefaceSettings.cpp.
#pragma once

#include "FirefaceRegisters.hpp"

#include <array>
#include <cstdint>
#include <expected>

namespace ASFW::Audio::RME {

// Quadlet indices into the flash settings block (FFADO
// FF_device_flash_settings_t, fireface_def.h). The FF400 reuses some FF800
// fields; see DecodeFlashSettings.
namespace FlashField {
inline constexpr uint32_t kSpdifInputMode = 4;
inline constexpr uint32_t kSpdifOutputEmphasis = 5;
inline constexpr uint32_t kSpdifOutputProfessional = 6;
inline constexpr uint32_t kClockMode = 7;
inline constexpr uint32_t kSpdifOutputNonAudio = 8;
inline constexpr uint32_t kSyncReference = 9;
inline constexpr uint32_t kSpdifOutputMode = 10;
inline constexpr uint32_t kChannelLimit = 26;          // limit_bandwidth
inline constexpr uint32_t kInputLevel = 29;
inline constexpr uint32_t kOutputLevel = 30;
inline constexpr uint32_t kPlugSelect0 = 31;           // FF800 input 7 jack; FF400 phones level
inline constexpr uint32_t kPlugSelect1 = 32;           // FF800 input 8 jack
inline constexpr uint32_t kPhantom0 = 33;              // FF800 input 7; FF400 input 1
inline constexpr uint32_t kPhantom1 = 34;              // FF800 input 8; FF400 input 2
inline constexpr uint32_t kPhantom2 = 35;              // FF800 input 9; FF400 input 3 pad
inline constexpr uint32_t kPhantom3 = 36;              // FF800 input 10; FF400 input 4 pad
inline constexpr uint32_t kInstrumentPlugSelect = 37;  // FF800 input 1 jack
inline constexpr uint32_t kFilter = 38;                // FF800 speaker emulation; FF400 input 4 instrument
inline constexpr uint32_t kFuzz = 39;                  // FF800 drive; FF400 input 3 instrument
inline constexpr uint32_t kSampleRate = 43;
inline constexpr uint32_t kWordClockSingleSpeed = 46;
inline constexpr uint32_t kLimiterOff = 49;            // p12db_an[0]: nonzero turns the FF800 limiter off
}

enum class InputLevel : uint8_t { kLowGain, kPlus4dBu, kMinus10dBV };
enum class OutputLevel : uint8_t { kHighGain, kPlus4dBu, kMinus10dBV };
enum class PhonesLevel : uint8_t { kHighGain, kPlus4dBu, kMinus10dBV };
enum class SyncReference : uint8_t { kAdat1, kAdat2, kSpdif, kWordClock, kTimecode };
enum class JackSelect : uint8_t { kRear, kFront, kFrontAndRear };
enum class ChannelLimit : uint8_t { kAllChannels, kNoAdat2, kAnalogAndSpdif, kAnalogOnly };

struct FirefaceSettings {
    bool clockMaster{true};
    SyncReference syncReference{SyncReference::kAdat1};
    bool spdifInputOptical{false};
    bool spdifOutputOptical{false};
    bool spdifOutputEmphasis{false};
    bool spdifOutputProfessional{false};
    bool spdifOutputNonAudio{false};
    bool wordClockSingleSpeed{false};
    InputLevel inputLevel{InputLevel::kLowGain};
    OutputLevel outputLevel{OutputLevel::kHighGain};
    std::array<bool, 4> phantom{};          // FF800 inputs 7-10; FF400 inputs 1-2
    std::array<bool, 2> ff400Pad{};         // FF400 inputs 3, 4
    std::array<bool, 2> ff400Instrument{};  // FF400 inputs 3, 4
    PhonesLevel ff400Phones{PhonesLevel::kHighGain};
    JackSelect ff800Input1{JackSelect::kFront};
    JackSelect ff800Input7{JackSelect::kFront};
    JackSelect ff800Input8{JackSelect::kFront};
    bool ff800SpeakerEmulation{false};
    bool ff800Drive{false};
    bool ff800Limiter{true};
    ChannelLimit channelLimit{ChannelLimit::kAllChannels};  // a saved host preference, not encoded
    uint32_t sampleRateHz{0};                               // informational, not encoded
};

/// The first flash field that is unset (0xffffffff) or holds no known value.
struct FlashDecodeError {
    uint32_t quadlet{0};
    uint32_t value{0};
};

using ConfigWords = std::array<uint32_t, 3>;

/// Refuses rather than guesses: every field the encoder reads must hold a
/// defined value, phantom power exactly 0 or 1.
[[nodiscard]] std::expected<FirefaceSettings, FlashDecodeError>
DecodeFlashSettings(FirefaceModel model, const FlashSettings& flash) noexcept;

[[nodiscard]] ConfigWords EncodeConfig(FirefaceModel model, const FirefaceSettings& settings) noexcept;

/// The configuration fields the status quadlet at 0x801c0004 mirrors, as the
/// encoded configuration predicts them and as the card reports them.
struct StatusComparison {
    uint32_t expected{0};
    uint32_t reported{0};
    uint32_t mask{0};
    [[nodiscard]] bool Matches() const noexcept { return expected == reported; }
};

[[nodiscard]] StatusComparison CompareWithStatus(FirefaceModel model, const ConfigWords& config,
                                                 uint32_t status1) noexcept;

/// The status quadlets at 0x801c0000 (status0) and 0x801c0004 (status1),
/// decoded. One layout for both models: Linux dump_sync_status serves the FF400
/// and FF800 alike (ff-protocol-former.c:159-255).
enum class ClockSource : uint8_t { kInternal, kAdat1, kAdat2, kSpdif, kWordClock, kTimecode, kNone, kUnknown };
enum class LockState : uint8_t { kNone, kLock, kSync };
struct DecodedStatus {
    ClockSource configured{ClockSource::kUnknown};     // status1 bit 0, else bits 12:10
    uint32_t configuredRateHz{0};                      // status1 bits 4:1; 0 if unknown
    ClockSource syncReference{ClockSource::kUnknown};  // status0 bits 24:22: what a slave follows
    uint32_t syncRateHz{0};                            // status0 bits 28:25; 0 if unknown
    LockState wordClock{LockState::kNone};
    LockState spdif{LockState::kNone};
    LockState adat1{LockState::kNone};
    LockState adat2{LockState::kNone};
};

[[nodiscard]] DecodedStatus DecodeStatus(uint32_t status0, uint32_t status1) noexcept;

[[nodiscard]] const char* Name(ClockSource source) noexcept;
[[nodiscard]] const char* Name(LockState state) noexcept;
[[nodiscard]] const char* Name(InputLevel level) noexcept;
[[nodiscard]] const char* Name(OutputLevel level) noexcept;
[[nodiscard]] const char* Name(PhonesLevel level) noexcept;
[[nodiscard]] const char* Name(SyncReference reference) noexcept;
[[nodiscard]] const char* Name(JackSelect jack) noexcept;
[[nodiscard]] const char* Name(ChannelLimit limit) noexcept;

} // namespace ASFW::Audio::RME
