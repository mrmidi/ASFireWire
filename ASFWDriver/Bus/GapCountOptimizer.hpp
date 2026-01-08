// SPDX-License-Identifier: MIT
// Copyright (c) 2025 ASFW Project

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace ASFW::Driver {

class GapCountOptimizer {
public:
    static constexpr uint8_t GAP_TABLE[26] = {
        63, 5, 7, 8, 10, 13, 16, 18, 21, 24, 26, 29, 32, 35, 37, 40,
        43, 46, 48, 51, 54, 57, 59, 62, 63, 63
    };

    static uint8_t CalculateFromHops(uint8_t maxHops);

    static uint8_t CalculateFromPing(uint32_t maxPingNs);

    static uint8_t Calculate(uint8_t maxHops, std::optional<uint32_t> maxPingNs = std::nullopt);

    static bool ShouldUpdate(const std::vector<uint8_t>& currentGaps,
                            uint8_t newGap,
                            uint8_t prevGap = 0xFF);

    static bool AreGapsConsistent(const std::vector<uint8_t>& gaps);

    static bool HasInvalidGap(const std::vector<uint8_t>& gaps);
};

} // namespace ASFW::Driver
