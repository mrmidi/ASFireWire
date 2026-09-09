// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AmdtpTransferDelay.hpp
// ASFWDriver
//
// Shared IEC 61883-6 AMDTP transfer delay calculation matching Linux amdtp-stream.c:302-308.

#pragma once

#include <cstdint>

namespace ASFW::Encoding {

enum class CipStreamMode : uint8_t {
    NonBlocking,
    Blocking,
};

/// Transfer delay per IEC 61883-6 §7.3.
/// Linux amdtp-stream.c:302-308 derives transfer delay from:
///   1. Default device buffering: TRANSFER_DELAY_TICKS (0x2E00 / 11776) - TICKS_PER_CYCLE (3072) = 8704 ticks.
///   2. Additional buffering in blocking mode to absorb NO-DATA packets: (TICKS_PER_SECOND * syt_interval) / rate.
[[nodiscard]] constexpr uint32_t AmdtpTransferDelayTicks(
    uint32_t sampleRateHz,
    uint8_t sytInterval,
    CipStreamMode mode = CipStreamMode::Blocking) noexcept {
    if (sampleRateHz == 0) {
        return 0;
    }
    constexpr uint32_t kTransferDelayTicks = 0x2E00; // 11776
    constexpr uint32_t kTicksPerCycle = 3072;
    constexpr uint64_t kTicksPerSecond = 24'576'000ULL;

    constexpr uint32_t kBaseDelayTicks = kTransferDelayTicks - kTicksPerCycle; // 8704 ticks (~354 µs)
    if (mode == CipStreamMode::NonBlocking) {
        return kBaseDelayTicks;
    }
    return kBaseDelayTicks +
        static_cast<uint32_t>((kTicksPerSecond * sytInterval) / sampleRateHz);
}

static_assert(AmdtpTransferDelayTicks(48'000, 8, CipStreamMode::Blocking) == 12'800,
              "48 kHz blocking transfer delay must match Linux derivation (12800 ticks)");
static_assert(AmdtpTransferDelayTicks(96'000, 16, CipStreamMode::Blocking) == 12'800,
              "96 kHz blocking transfer delay must match Linux derivation (12800 ticks)");
static_assert(AmdtpTransferDelayTicks(192'000, 32, CipStreamMode::Blocking) == 12'800,
              "192 kHz blocking transfer delay must match Linux derivation (12800 ticks)");
static_assert(AmdtpTransferDelayTicks(48'000, 8, CipStreamMode::NonBlocking) == 8'704,
              "Non-blocking transfer delay must match Linux base buffering (8704 ticks)");
static_assert(AmdtpTransferDelayTicks(0, 8, CipStreamMode::Blocking) == 0,
              "Unknown rate must report 0 ticks");

} // namespace ASFW::Encoding
