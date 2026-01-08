// SPDX-License-Identifier: MIT
// Copyright (c) 2025 ASFW Project

#include "GapCountOptimizer.hpp"
#include <algorithm>

namespace ASFW::Driver {

uint8_t GapCountOptimizer::CalculateFromHops(uint8_t maxHops) {
    if (maxHops > 25) {
        maxHops = 25;
    }

    return GAP_TABLE[maxHops];
}

uint8_t GapCountOptimizer::CalculateFromPing(uint32_t maxPingNs) {
    if (maxPingNs > 245) {
        maxPingNs = 245;
    }

    if (maxPingNs >= 29) {
        uint32_t index = (maxPingNs - 20) / 9;
        if (index > 25) {
            index = 25;
        }
        return GAP_TABLE[index];
    } else {
        return 5;
    }
}

uint8_t GapCountOptimizer::Calculate(uint8_t maxHops, std::optional<uint32_t> maxPingNs) {
    uint8_t hopGap = CalculateFromHops(maxHops);

    if (maxPingNs.has_value()) {
        uint8_t pingGap = CalculateFromPing(*maxPingNs);
        return std::max(hopGap, pingGap);
    }

    return hopGap;
}

bool GapCountOptimizer::AreGapsConsistent(const std::vector<uint8_t>& gaps) {
    if (gaps.empty()) {
        return true;
    }

    uint8_t firstGap = gaps[0];
    for (size_t i = 1; i < gaps.size(); ++i) {
        if (gaps[i] != firstGap) {
            return false;
        }
    }

    return true;
}

bool GapCountOptimizer::HasInvalidGap(const std::vector<uint8_t>& gaps) {
    for (uint8_t gap : gaps) {
        if (gap == 0) {
            return true;
        }
    }

    return !AreGapsConsistent(gaps);
}

bool GapCountOptimizer::ShouldUpdate(const std::vector<uint8_t>& currentGaps,
                                     uint8_t newGap,
                                     uint8_t prevGap) {
    if (currentGaps.empty()) {
        return false;
    }

    if (!AreGapsConsistent(currentGaps)) {
        return true;
    }

    for (uint8_t gap : currentGaps) {
        if (gap == 0) {
            return true;
        }
    }

    uint8_t currentGap = currentGaps[0];

    if (prevGap == 0xFF) {
        return (currentGap != newGap);
    }

    bool matchesNew = (currentGap == newGap);
    bool matchesPrev = (currentGap == prevGap);

    if (matchesNew || matchesPrev) {
        return false;
    }

    return true;
}

} // namespace ASFW::Driver
