// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FirefaceRegisters.hpp - Fireface 400/800 register addresses and fixed values.
// Definitions only; the sequencing lives in FirefaceFamilyDriver.hpp.
#pragma once

#include <array>
#include <cstdint>

namespace ASFW::Audio::RME {

enum class FirefaceModel : uint8_t { kFF400, kFF800 };

// Register constants and clock maps are transcribed as behavior from FFADO
// fireface_def.h:84-90, fireface_flash.cpp:59-82 and ALSA former/ff400.rs:399-455,
// ff800.rs:113-174. All register values are little-endian quadlets.
namespace Register {
inline constexpr uint64_t kStatus = 0x801c0000ULL;
// Same address, write side: one quadlet per playback data channel. 0 lets the
// device fetch PCM from that channel, 1 mutes it (FFADO RME_FF_CHANNEL_MUTE_MASK,
// fireface_def.h:86; Linux FORMER_REG_FETCH_PCM_FRAMES, ff-protocol-former.c:12).
inline constexpr uint64_t kFetchMask = kStatus;
inline constexpr uint64_t kFF400Init = 0x80100500ULL;
inline constexpr uint64_t kFF400Start = 0x8010050cULL;
inline constexpr uint64_t kFF400Stop = 0x80100504ULL;
inline constexpr uint64_t kFF400FlashCommand = 0x80100520ULL;
// Read side of the same register: 0 once the flash command is done (FFADO
// RME_FF400_FLASH_STAT_OFS == RME_FF400_FLASH_CMD_OFS, fireface_def.h:58-59;
// RME 3.41 Wait 0x6c0a polls 0x80100520 for 0).
inline constexpr uint64_t kFF400FlashStatus = kFF400FlashCommand;
// Flash busy wait: 25 polls 2 ms apart (RME 3.41 Wait 0x6c0a; FFADO
// MAX_FLASH_BUSY_RETRIES and wait_while_busy(2), fireface_flash.cpp:33-62).
inline constexpr uint32_t kFF400FlashPolls = 25;
inline constexpr uint32_t kFF400FlashPollIntervalMs = 2;
inline constexpr uint64_t kFF400Revision = 0x80100290ULL;
inline constexpr uint64_t kFF800Init = 0x00020000001cULL;
inline constexpr uint64_t kFF800Start = 0x000200000028ULL;
inline constexpr uint64_t kFF800Stop = 0x000200000034ULL;
inline constexpr uint64_t kFF800Revision = 0x000200000100ULL;
// Device settings in flash (FFADO fireface_def.h:108-126). The FF800 block is
// readable directly; the FF400 one goes through a flash command: [flash
// address, byte count] to kFF400FlashBlock, READ, then the bounce buffer.
inline constexpr uint64_t kFF800FlashSettings = 0x0003000f0000ULL;
inline constexpr uint32_t kFF400FlashSettings = 0x00060000U;  // a flash address, not a bus one
inline constexpr uint64_t kFF400FlashBlock = 0x80100288ULL;
inline constexpr uint64_t kFF400FlashReadBuffer = kFF400Revision;
inline constexpr uint32_t kFF400FlashQuadletsPerRead = 32;  // fireface_flash.cpp:107
// Configuration register, 3 quadlets, write-only (FFADO RME_FF800_CONF_REG /
// RME_FF400_CONF_REG; RME 3.41 Fireface_InitHardware 0x6728 writes the same).
// Written by FirefaceFamilyDriver::WriteConfig before every init.
inline constexpr uint64_t kFF800Config = 0x0000fc88f014ULL;
inline constexpr uint64_t kFF400Config = 0x80100514ULL;
inline constexpr uint32_t kConfiguredSourceMask = 0x1c01;
// Bit 0 of the configured source wins over the saved external selection in
// bits 12:10; the device reports both together (Linux parse_clock_bits,
// ff-protocol-former.c:53-55).
inline constexpr uint32_t kConfiguredInternalFlag = 0x0001;
}

// FFADO FF_device_flash_settings_t (fireface_def.h): the saved device settings.
inline constexpr uint32_t kFlashSettingsQuadlets = 59;
using FlashSettings = std::array<uint32_t, kFlashSettingsQuadlets>;

// The only FF400 flash commands this driver sends. The same register also
// takes WRITE (1) and ERASE (0xc/0xd/0xe) (fireface_def.h:135-140); those
// values have no name here, so they cannot be sent.
enum class FF400FlashCommand : uint32_t { kRead = 0x2, kGetRevision = 0xf };

[[nodiscard]] constexpr uint32_t FirmwareMinimum(FirefaceModel model) noexcept {
    return model == FirefaceModel::kFF800 ? 0x24dU : 0x146U;
}

[[nodiscard]] constexpr std::array<uint32_t, 3> InitWords(
    FirefaceModel model, uint8_t playbackChannel, bool s800) noexcept {
    if (model == FirefaceModel::kFF800) {
        return {48000U, (28U << 11U) + playbackChannel, 28U | (s800 ? 0x800U : 0U)};
    }
    return {48000U, (18U << 11U) + playbackChannel, 18U};
}

[[nodiscard]] constexpr uint32_t StartWord(FirefaceModel model, uint8_t captureChannel,
                                            bool s800) noexcept {
    return model == FirefaceModel::kFF800
        ? 0x80000000U | 28U | (s800 ? 0x800U : 0U)
        : 0x80000000U | 18U | (static_cast<uint32_t>(captureChannel) << 5U);
}
[[nodiscard]] constexpr bool IsValidAssignedChannel(FirefaceModel model, uint8_t channel) noexcept {
    return channel < (model == FirefaceModel::kFF400 ? 8U : 64U);
}

} // namespace ASFW::Audio::RME
