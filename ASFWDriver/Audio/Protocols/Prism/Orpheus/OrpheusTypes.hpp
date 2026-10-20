// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ASFW::Audio::Prism::Orpheus {

// MARK: - Prism Sound Orpheus Wire Constants
namespace Wire {
inline constexpr std::array<uint8_t, 3> kCompanyId{0x00, 0x11, 0x98};

inline constexpr uint8_t kCtypeControl = 0x00;
inline constexpr uint8_t kCtypeStatus = 0x01;
inline constexpr uint8_t kResponseAccepted = 0x09;
inline constexpr uint8_t kResponseRejected = 0x0A;
inline constexpr uint8_t kResponseStable = 0x0C;

inline constexpr uint8_t kSubunitAudio = 0x08;
inline constexpr uint8_t kSubunitUnit = 0xFF;
inline constexpr uint8_t kOpcodeVendorDependent = 0x00;
inline constexpr uint8_t kOpcodeSignalSource = 0x1A;

inline constexpr size_t kFrameLength = 15;
inline constexpr size_t kMixBusFrameLength = 74;
inline constexpr size_t kCommandOffset = 6;
inline constexpr size_t kPayloadOffset = 7;

inline constexpr size_t kAnalogChannelCount = 8;
inline constexpr size_t kMixBusCount = 6;
inline constexpr size_t kMixInputCount = 12;
inline constexpr uint8_t kAllMixBuses = 7;

inline constexpr uint16_t kMeterAddressHigh = 0xFFC7;
inline constexpr uint32_t kMeterAddressLow = 0x00600420;
inline constexpr size_t kMeterCount = 50;
inline constexpr uint32_t kLegacyMeterReadLength = 200;
inline constexpr uint32_t kMeterReadLength = 204;
} // namespace Wire

enum class OrpheusCommand : uint8_t {
    MasterVolume = 0xA0,
    MasterMute = 0xA1,
    VolumeSelect = 0xA2,
    MasterLock = 0xA3,
    FrontPanelMeterMode = 0xB0,
    SampleRateConverter = 0xB1,
    ClockOutput = 0xB2,
    AdatMode = 0xB3,
    Identify = 0xB4,
    FirmwareVersion = 0xB6,
    MeterBrightness = 0xB7,
    UnitState = 0xBF,

    InputType = 0xC0,
    Overkiller = 0xC1,
    Phantom = 0xC2,
    Filter = 0xC3,
    Phase = 0xC4,
    MidSide = 0xC5,
    LineInputLevel = 0xC6,
    MicGain = 0xC7,
    InstrumentGain = 0xC8,
    LineOutputLevel = 0xC9,
    HeadphoneSource = 0xCA,
    AnalogChannelState = 0xCF,

    DigitalSampleRate = 0xD0,
    DigitalBitDepth = 0xD2,
    DigitalSync = 0xD3,
    DigitalInputType = 0xD4,
    DigitalChannelStatus = 0xD7,
    DigitalState = 0xDF,

    MixDefeat = 0xE0,
    MixSolo = 0xE2,
    MixMute = 0xE3,
    MixGain = 0xE4,
    MixInputMute = 0xE5,
    MixInputGain = 0xE6,
    MixInputPan = 0xE7,
    MixInputBalance = 0xE8,
    MixBusState = 0xEF
};

enum class OrpheusSyncSource : uint8_t {
    Csp = 0,
    FireWire = 1,
    Local = 2,
    DigitalInput = 3,
    Adat = 4,
    WordClock = 5
};

enum class OrpheusAdatMode : uint8_t {
    None = 0,
    Send = 1,
    Return = 2,
    SendReturn = 3,
    Direct = 4
};

enum class OrpheusInputType : uint8_t {
    Mic = 0,
    Line = 1,
    Instrument = 2
};

namespace Scales {
inline constexpr int16_t kSilent = -32768;

/// HAPI_BIG_KNOB_VALUES: 30 master volume steps (in 1/256 dB).
inline constexpr std::array<int16_t, 30> kBigKnobValues{
    kSilent, -15872, -13312, -11776, -10752, -10240, -9728, -9216, -8704, -8192,
    -7680,   -7168,  -6656,  -6144,  -5632,  -5120,  -4608, -4096, -3584, -3072,
    -2560,   -2048,  -1792,  -0x0600, -1280,  -1024,  -768,  -512,  -256,  0
};

/// HapiMixSlider: 166 fader positions (in 1/256 dB).
inline constexpr std::array<int16_t, 166> kMixGainValues{
    kSilent, -22528, -22016, -21504, -20992, -20480, -19968, -19456, -18944, -18432,
    -17920,  -17408, -16896, -16384, -15872, -15360, -15104, -14848, -14592, -14336,
    -14080,  -13824, -13568, -13312, -13056, -0x3200, -12673, -12544, -12417, -0x3000,
    -12161,  -12032, -11905, -11776, -11649, -11520, -11393, -11264, -11137, -11008,
    -10881,  -10752, -10625, -10496, -10369, -10240, -10113, -9984,  -9857,  -9728,
    -9601,   -9472,  -9345,  -9216,  -9089,  -8960,  -8833,  -8704,  -8577,  -8448,
    -8321,   -8192,  -8065,  -7936,  -7809,  -7680,  -7553,  -7424,  -7297,  -7168,
    -7041,   -6912,  -6785,  -6656,  -6529,  -6400,  -6273,  -6144,  -6017,  -5888,
    -5761,   -5632,  -5505,  -5376,  -5249,  -5120,  -5057,  -4993,  -4929,  -4864,
    -4801,   -4737,  -4673,  -4608,  -4545,  -4481,  -4417,  -4352,  -4289,  -4225,
    -4161,   -4096,  -4033,  -3969,  -3905,  -3840,  -3777,  -3713,  -3649,  -3584,
    -3521,   -3457,  -3393,  -3328,  -3265,  -3201,  -3137,  -3072,  -3009,  -2945,
    -2881,   -2816,  -2753,  -2689,  -2625,  -2560,  -2497,  -2433,  -2369,  -2304,
    -2241,   -2177,  -2113,  -2048,  -1985,  -1921,  -1857,  -1792,  -1729,  -1665,
    -1601,   -0x0600, -1473, -1409,  -1345,  -1280,  -1217,  -1153,  -1089,  -1024,
    -961,    -897,   -833,   -768,   -705,   -641,   -577,   -512,   -449,   -385,
    -321,    -256,   -193,   -129,   -65,    0
};

inline size_t BigKnobPositionForRaw(int16_t raw) noexcept {
    if (raw == kSilent) return 0;
    size_t closest = 1;
    int minDiff = std::abs(static_cast<int>(kBigKnobValues[1]) - static_cast<int>(raw));
    for (size_t i = 2; i < kBigKnobValues.size(); ++i) {
        int diff = std::abs(static_cast<int>(kBigKnobValues[i]) - static_cast<int>(raw));
        if (diff < minDiff) {
            minDiff = diff;
            closest = i;
        }
    }
    return closest;
}

inline size_t MixGainPositionForRaw(int16_t raw) noexcept {
    if (raw == kSilent) return 0;
    size_t closest = 1;
    int minDiff = std::abs(static_cast<int>(kMixGainValues[1]) - static_cast<int>(raw));
    for (size_t i = 2; i < kMixGainValues.size(); ++i) {
        int diff = std::abs(static_cast<int>(kMixGainValues[i]) - static_cast<int>(raw));
        if (diff < minDiff) {
            minDiff = diff;
            closest = i;
        }
    }
    return closest;
}
} // namespace Scales

// MARK: - Parsed State Structures

struct OrpheusFirmwareVersion {
    uint8_t major{0};
    uint8_t minor{0};

    [[nodiscard]] constexpr bool HasMeterStatusQuadlet() const noexcept {
        return major >= 2 || (major == 1 && minor >= 3);
    }
    [[nodiscard]] constexpr bool SupportsAdatDirect() const noexcept {
        return major != 0 && (major != 1 || minor > 1);
    }
    [[nodiscard]] constexpr uint32_t MeterReadLength() const noexcept {
        return HasMeterStatusQuadlet() ? Wire::kMeterReadLength : Wire::kLegacyMeterReadLength;
    }
};

struct OrpheusUnitState {
    int16_t masterVolume{Scales::kSilent};
    uint16_t volumeSelectMask{0};
    bool masterMute{false};
    bool masterLock{false};
    bool identify{false};
    uint8_t frontPanelMeterMode{0};
    uint8_t sampleRateConverter{0};
    uint8_t clockOutput{0};
    uint8_t adatMode{0};
    uint8_t headphoneSource{0};
    uint8_t meterBrightness{15};

    [[nodiscard]] constexpr bool HasAdatInput() const noexcept {
        return adatMode >= 2 && adatMode <= 4;
    }
};

struct OrpheusAnalogChannelState {
    uint8_t inputType{0};
    uint8_t lineInputLevel{1};
    uint8_t lineOutputLevel{1};
    bool overkiller{false};
    bool midSide{false};
    bool phantom{false};
    bool phase{false};
    uint8_t filter{0};
    uint8_t micGain{10};
    uint8_t instrumentGain{10};

    [[nodiscard]] static constexpr bool HasPreamp(uint32_t channel) noexcept { return channel < 4; }
    [[nodiscard]] static constexpr bool HasInstrumentInput(uint32_t channel) noexcept { return channel < 2; }
};

struct OrpheusMixBusState {
    uint8_t busIndex{0};
    int16_t masterGain{0};
    bool masterMute{false};
    uint16_t soloMask{0};
    std::array<int16_t, 12> inputGains{};
    std::array<int8_t, 12> inputPans{};
    std::array<bool, 12> inputMutes{};
};

} // namespace ASFW::Audio::Prism::Orpheus
