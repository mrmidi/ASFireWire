// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once
#include "FirefaceSettings.hpp"

namespace ASFW::Audio::RME {

struct ClockStatus {
    uint32_t configuredSource{0};
    uint32_t activeSource{0};
    uint32_t q0{0};
    uint32_t q1{0};
    bool configured48k{false};
    bool externalLocked48k{false};
};

// Shared by synchronous health queries and the asynchronous FF800 start.
// RE audit (behavior unchanged): RME 3.41 Settings x86_64 0x100006014-0x100006084
// agrees with FirefaceSettings.cpp's lock/sync labels, not all labels below.
// Both bits are required here, so swapping a pair leaves that boolean unchanged.
// Its source table at 0x100006fb0 maps LTC to 0x01400000; this helper expects
// 0x01800000 for FF800 TCO. See documentation/ff800-vendor-status.md before
// changing external-clock admission; the vendor UI also gates on external rate.
[[nodiscard]] inline ClockStatus DecodeClockStatus(FirefaceModel model, uint32_t q0, uint32_t q1) noexcept {
    ClockStatus s{.configuredSource = q1 & Register::kConfiguredSourceMask,
                  .activeSource = q0 & 0x01c00000U, .q0 = q0, .q1 = q1};
    s.configured48k = DecodeStatus(q0, q1).configuredRateHz == 48000U;
    if ((s.configuredSource & Register::kConfiguredInternalFlag) != 0U) return s;
    uint32_t expectedActive = 0xffffffffU;
    if (s.configuredSource == 0x1000U) expectedActive = 0x01000000U;
    else if (s.configuredSource == 0x0c00U) expectedActive = 0x00c00000U;
    else if (s.configuredSource == 0U) expectedActive = 0U;
    else if (model == FirefaceModel::kFF400 && s.configuredSource == 0x1400U) {
        // FF400 LTC status is in its separate 0x801f0000 page; until that
        // page is decoded, it cannot pass the external-locked-48k gate.
        return s;
    }
    else if (model == FirefaceModel::kFF800 && s.configuredSource == 0x0400U) expectedActive = 0x00400000U;
    else if (model == FirefaceModel::kFF800 && s.configuredSource == 0x1800U) expectedActive = 0x01800000U;
    if (s.activeSource != expectedActive) return s;
    if (model == FirefaceModel::kFF400) {
        const uint32_t active = s.activeSource;
        bool locked = false, synced = false, rate48 = false;
        if (active == 0x01000000U) { locked = (q0 & 0x20000000U); synced = (q0 & 0x40000000U); rate48 = (q0 & 0x1e000000U) == 0x06000000U; }
        else if (active == 0x00c00000U) { locked = (q0 & 0x00040000U); synced = (q0 & 0x00100000U); rate48 = (q0 & 0x0003c000U) == 0x0000c000U; }
        else if (active == 0) { locked = (q0 & 0x1000U); synced = (q0 & 0x400U); rate48 = (q0 & 0x1e000000U) == 0x06000000U; }
    s.externalLocked48k = locked && synced && rate48;
    } else {
        const uint32_t active = s.activeSource;
        bool locked = false, synced = false, rate48 = false;
        if (active == 0x01000000U) { locked = (q0 & 0x20000000U); synced = (q0 & 0x40000000U); rate48 = (q0 & 0x1e000000U) == 0x06000000U; }
        else if (active == 0x00c00000U) { locked = (q0 & 0x00040000U); synced = (q0 & 0x00100000U); rate48 = (q0 & 0x0003c000U) == 0x0000c000U; }
        else if (active == 0) { locked = (q0 & 0x1000U); synced = (q0 & 0x400U); rate48 = (q0 & 0x1e000000U) == 0x06000000U; }
        else if (active == 0x00400000U) { locked = (q0 & 0x2000U); synced = (q0 & 0x800U); rate48 = (q0 & 0x1e000000U) == 0x06000000U; }
        else if (active == 0x01800000U) { locked = (q1 & 0x00400000U); synced = (q1 & 0x00800000U); rate48 = (q0 & 0x1e000000U) == 0x06000000U; }
        s.externalLocked48k = locked && synced && rate48;
    }
    return s;
}

[[nodiscard]] inline bool IsClockReady(const ClockStatus& s) noexcept {
    return s.configured48k && ((s.configuredSource & Register::kConfiguredInternalFlag) != 0U ||
                               s.externalLocked48k);
}

} // namespace ASFW::Audio::RME
