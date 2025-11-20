// SPDX-License-Identifier: MIT
// Copyright (c) 2025 ASFW Project

#include "GapCountOptimizer.hpp"
#include <algorithm>

namespace ASFW::Driver {

uint8_t GapCountOptimizer::CalculateFromHops(uint8_t maxHops) {
    // Clamp to table size (indices 0-25)
    if (maxHops > 25) {
        maxHops = 25;
    }

    return GAP_TABLE[maxHops];
}

uint8_t GapCountOptimizer::CalculateFromPing(uint32_t maxPingNs) {
    // Clamp to maximum representable ping time
    if (maxPingNs > 245) {
        maxPingNs = 245;
    }

    // Formula from Apple IOFireWireController.cpp line 3308-3311
    if (maxPingNs >= 29) {
        uint32_t index = (maxPingNs - 20) / 9;
        // Clamp index to table bounds
        if (index > 25) {
            index = 25;
        }
        return GAP_TABLE[index];
    } else {
        // Minimum gap for very short ping times
        return 5;
    }
}

uint8_t GapCountOptimizer::Calculate(uint8_t maxHops, std::optional<uint32_t> maxPingNs) {
    // Calculate gap from hop count (conservative, assumes daisy chain)
    uint8_t hopGap = CalculateFromHops(maxHops);

    // If ping time available, calculate from ping and use the larger value (safer)
    if (maxPingNs.has_value()) {
        uint8_t pingGap = CalculateFromPing(*maxPingNs);

        // Use the larger (more conservative) gap count
        // This follows Apple's logic (lines 3315-3318)
        return std::max(hopGap, pingGap);
    }

    // Fallback to hop-count-only calculation
    return hopGap;
}

bool GapCountOptimizer::AreGapsConsistent(const std::vector<uint8_t>& gaps) {
    if (gaps.empty()) {
        return true;  // Vacuously consistent
    }

    // Check if all gaps equal the first gap
    uint8_t firstGap = gaps[0];
    for (size_t i = 1; i < gaps.size(); ++i) {
        if (gaps[i] != firstGap) {
            return false;
        }
    }

    return true;
}

bool GapCountOptimizer::HasInvalidGap(const std::vector<uint8_t>& gaps) {
    // Check for gap=0 (invalid per IEEE 1394a)
    for (uint8_t gap : gaps) {
        if (gap == 0) {
            return true;
        }
    }

    // Inconsistent gaps also count as invalid
    return !AreGapsConsistent(gaps);
}

bool GapCountOptimizer::ShouldUpdate(const std::vector<uint8_t>& currentGaps,
                                     uint8_t newGap,
                                     uint8_t prevGap) {
    if (currentGaps.empty()) {
        return false;  // No nodes, no update needed
    }

    // Critical: If gaps are inconsistent, we MUST update
    // This follows Apple's logic (lines 3378-3386)
    if (!AreGapsConsistent(currentGaps)) {
        return true;
    }

    // Critical: If any node has gap=0, we MUST update
    // This follows Linux's logic (core-card.c lines 432-447)
    for (uint8_t gap : currentGaps) {
        if (gap == 0) {
            return true;
        }
    }

    // All gaps are consistent and non-zero. Check if they match what we expect.
    // Following Apple's logic (lines 3388-3401):
    // - If current gap matches newGap → no update needed (already optimal)
    // - If current gap matches prevGap → no update needed (avoid jitter)
    // - Otherwise → update needed

    uint8_t currentGap = currentGaps[0];  // All consistent, so use first

    // If never set prevGap (0xFF), only check against newGap
    if (prevGap == 0xFF) {
        return (currentGap != newGap);
    }

    // Check if current gap matches either new or previous
    // This prevents updates due to ping time jitter (Apple comment line 3371)
    bool matchesNew = (currentGap == newGap);
    bool matchesPrev = (currentGap == prevGap);

    if (matchesNew || matchesPrev) {
        return false;  // Already optimal or stable, no update
    }

    // Current gap doesn't match expected values → update needed
    return true;
}

} // namespace ASFW::Driver
