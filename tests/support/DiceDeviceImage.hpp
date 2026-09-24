// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceDeviceImage.hpp - Register images of the recorded DICE devices.
//
// Each image is the general-space state of one real device, reconstructed from
// its report in documentation/fixtures/ by `pydice export-dice-images-cpp`
// (tools/pydice/pydice/protocol/dice_report.py). SimulatedDiceDevice answers
// bus transactions from these values.

#pragma once

#include <array>
#include <cstdint>

namespace ASFW::Testing::DICE {

inline constexpr uint32_t kMaxImageStreams = 4;  // kMaxAudioStreamsPerDirection

// Offsets and sizes in quadlets, relative to the DICE base 0xFFFFE0000000,
// exactly as the section table reports them.
struct DiceSectionImage {
    uint32_t offsetQuadlets{0};
    uint32_t sizeQuadlets{0};
};

// One stream block. `speedOrSeqStart` is TX_SPEED for a device-TX stream and
// RX_SEQ_START for a device-RX stream. `names` is the DICE label-block text:
// names separated by '\', terminated by "\\".
struct DiceStreamImage {
    int32_t iso{-1};
    uint32_t pcm{0};
    uint32_t midi{0};
    uint32_t speedOrSeqStart{0};
    const char* names{""};
};

struct DiceStreamShapeImage {
    uint32_t pcm{0};
    uint32_t midi{0};
    const char* names{""};
};

struct DiceDirectionShapeImage {
    uint32_t count{0};
    std::array<DiceStreamShapeImage, kMaxImageStreams> streams{};
};

// Stream shapes for one rate mode (TCAT extension current_config).
struct DiceRateModeImage {
    DiceDirectionShapeImage tx{};
    DiceDirectionShapeImage rx{};
};

struct DiceExtSyncImage {
    uint32_t clockSource{0};
    uint32_t locked{0};
    uint32_t rateIndex{0};
    uint32_t adatUserData{0};
};

struct DiceDeviceImage {
    const char* key{""};
    // The report's model string. It is our catalog's name echoed back, not a
    // device fact: the Venice F24 report says "Venice F32".
    const char* reportedModel{""};
    const char* source{""};
    uint64_t guid{0};

    DiceSectionImage globalSection{};
    DiceSectionImage txSection{};
    DiceSectionImage rxSection{};
    DiceSectionImage extSyncSection{};
    bool hasExtension{false};

    uint64_t owner{0};
    uint32_t notification{0};
    const char* nickname{""};
    uint32_t clockSelect{0};
    uint32_t enable{0};
    uint32_t status{0};
    uint32_t extStatus{0};
    uint32_t sampleRate{0};
    uint32_t version{0};
    uint32_t clockCaps{0};
    const char* clockSourceNames{""};

    uint32_t txEntryQuadlets{0};
    uint32_t rxEntryQuadlets{0};
    uint32_t txCount{0};
    std::array<DiceStreamImage, kMaxImageStreams> tx{};
    uint32_t rxCount{0};
    std::array<DiceStreamImage, kMaxImageStreams> rx{};

    bool hasExtSync{false};
    DiceExtSyncImage extSync{};

    // Only devices with a TCAT extension report every rate mode (low, middle,
    // high). Others describe the current mode only.
    bool hasRateModeFormats{false};
    std::array<DiceRateModeImage, 3> rateModes{};
};

#include "DiceDeviceImages.inc"

} // namespace ASFW::Testing::DICE
