// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Pure MOTU 828 Mk3 V3 stream geometry. This file models captured wire facts;
// it performs no device I/O and owns no runtime protocol state.

#pragma once

#include <cstdint>
#include <optional>

namespace ASFW::Audio::MOTU {

enum class RateFamily : uint8_t {
    k1x = 1,
    k2x = 2,
    k4x = 4,
};

enum class OpticalBankMode : uint8_t {
    kDisabled,
    kAdat,
    kToslink,
};

struct OpticalBankState final {
    OpticalBankMode inputA{OpticalBankMode::kDisabled};
    OpticalBankMode inputB{OpticalBankMode::kDisabled};
    OpticalBankMode outputA{OpticalBankMode::kDisabled};
    OpticalBankMode outputB{OpticalBankMode::kDisabled};

    [[nodiscard]] constexpr bool AnyEnabled() const noexcept {
        return inputA != OpticalBankMode::kDisabled ||
               inputB != OpticalBankMode::kDisabled ||
               outputA != OpticalBankMode::kDisabled ||
               outputB != OpticalBankMode::kDisabled;
    }
};

struct StreamGeometry final {
    uint32_t sampleRateHz{0};
    uint8_t rateIndex{0};
    RateFamily family{RateFamily::k1x};
    uint32_t framesPerDataPacket{0};
    uint32_t hostToDevicePcm{0};
    uint32_t deviceToHostPcm{0};
    uint32_t hostToDeviceDbs{0};
    uint32_t deviceToHostDbs{0};
    uint32_t hostToDevicePacketBytes{0};
    uint32_t deviceToHostPacketBytes{0};
    uint32_t sphTicksNumerator{24576000};
    uint32_t sphTicksDenominator{0};
    uint32_t presentationLeadTicks{9216};
    OpticalBankState optical{};
};

class SphTickStepper final {
public:
    constexpr SphTickStepper(uint32_t sampleRateHz,
                             uint64_t initialTicks = 0,
                             uint32_t fractionalPhase = 0) noexcept
        : sampleRateHz_(sampleRateHz)
        , ticks_(initialTicks)
        , remainder_(sampleRateHz == 0 ? 0 : fractionalPhase % sampleRateHz) {}

    [[nodiscard]] constexpr uint64_t Current() const noexcept { return ticks_; }

    constexpr uint64_t Advance() noexcept {
        if (sampleRateHz_ == 0) {
            return ticks_;
        }
        constexpr uint32_t kMotuClockHz = 24576000;
        ticks_ += kMotuClockHz / sampleRateHz_;
        remainder_ += kMotuClockHz % sampleRateHz_;
        if (remainder_ >= sampleRateHz_) {
            ++ticks_;
            remainder_ -= sampleRateHz_;
        }
        return ticks_;
    }

private:
    uint32_t sampleRateHz_{0};
    uint64_t ticks_{0};
    uint32_t remainder_{0};
};

// V3 AudioBankControl / optical-interface mode register (0xf0000c94).
inline constexpr uint32_t kEnableOpticalInputA = 0x00000001u;
inline constexpr uint32_t kEnableOpticalInputB = 0x00000002u;
inline constexpr uint32_t kEnableOpticalOutputA = 0x00000100u;
inline constexpr uint32_t kEnableOpticalOutputB = 0x00000200u;
inline constexpr uint32_t kToslinkOpticalInputA = 0x00010000u;
inline constexpr uint32_t kToslinkOpticalInputB = 0x00100000u;
inline constexpr uint32_t kToslinkOpticalOutputA = 0x00040000u;
inline constexpr uint32_t kToslinkOpticalOutputB = 0x00400000u;

[[nodiscard]] constexpr OpticalBankMode DecodeBank(uint32_t value,
                                                    uint32_t enableBit,
                                                    uint32_t toslinkBit) noexcept {
    if ((value & enableBit) == 0) {
        return OpticalBankMode::kDisabled;
    }
    return (value & toslinkBit) != 0
        ? OpticalBankMode::kToslink
        : OpticalBankMode::kAdat;
}

[[nodiscard]] constexpr OpticalBankState DecodeOpticalBanks(uint32_t value) noexcept {
    return {
        .inputA = DecodeBank(value, kEnableOpticalInputA, kToslinkOpticalInputA),
        .inputB = DecodeBank(value, kEnableOpticalInputB, kToslinkOpticalInputB),
        .outputA = DecodeBank(value, kEnableOpticalOutputA, kToslinkOpticalOutputA),
        .outputB = DecodeBank(value, kEnableOpticalOutputB, kToslinkOpticalOutputB),
    };
}

[[nodiscard]] constexpr uint32_t OpticalPcmChunks(OpticalBankMode mode,
                                                   RateFamily family) noexcept {
    if (mode == OpticalBankMode::kDisabled || family == RateFamily::k4x) {
        return 0;
    }
    if (mode == OpticalBankMode::kToslink || family == RateFamily::k2x) {
        return 4;
    }
    return 8;
}

[[nodiscard]] constexpr uint32_t MotuV3Dbs(uint32_t pcmChunks) noexcept {
    constexpr uint32_t kSphQuadlets = 1;
    constexpr uint32_t kMessageChunks = 2;
    return kSphQuadlets + ((kMessageChunks + pcmChunks) * 3u + 3u) / 4u;
}

[[nodiscard]] constexpr std::optional<StreamGeometry>
Build828Mk3Geometry(uint32_t sampleRateHz, uint32_t audioBankControl = 0) noexcept {
    uint8_t rateIndex = 0;
    RateFamily family = RateFamily::k1x;
    uint32_t baseItPcm = 14;
    uint32_t baseIrPcm = 18;
    switch (sampleRateHz) {
        case 44100:  rateIndex = 0; family = RateFamily::k1x; break;
        case 48000:  rateIndex = 1; family = RateFamily::k1x; break;
        case 88200:  rateIndex = 2; family = RateFamily::k2x; break;
        case 96000:  rateIndex = 3; family = RateFamily::k2x; break;
        case 176400: rateIndex = 4; family = RateFamily::k4x; baseItPcm = 10; baseIrPcm = 14; break;
        case 192000: rateIndex = 5; family = RateFamily::k4x; baseItPcm = 10; baseIrPcm = 14; break;
        default: return std::nullopt;
    }

    const auto optical = DecodeOpticalBanks(audioBankControl);
    const uint32_t itPcm = baseItPcm +
        OpticalPcmChunks(optical.outputA, family) +
        OpticalPcmChunks(optical.outputB, family);
    const uint32_t irPcm = baseIrPcm +
        OpticalPcmChunks(optical.inputA, family) +
        OpticalPcmChunks(optical.inputB, family);
    const uint32_t itDbs = MotuV3Dbs(itPcm);
    const uint32_t irDbs = MotuV3Dbs(irPcm);
    const uint32_t frames = 8u * static_cast<uint32_t>(family);

    return StreamGeometry{
        .sampleRateHz = sampleRateHz,
        .rateIndex = rateIndex,
        .family = family,
        .framesPerDataPacket = frames,
        .hostToDevicePcm = itPcm,
        .deviceToHostPcm = irPcm,
        .hostToDeviceDbs = itDbs,
        .deviceToHostDbs = irDbs,
        .hostToDevicePacketBytes = 8u + frames * itDbs * 4u,
        .deviceToHostPacketBytes = 8u + frames * irDbs * 4u,
        .sphTicksDenominator = sampleRateHz,
        .optical = optical,
    };
}

} // namespace ASFW::Audio::MOTU
