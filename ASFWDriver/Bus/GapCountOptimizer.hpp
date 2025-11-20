// SPDX-License-Identifier: MIT
// Copyright (c) 2025 ASFW Project

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace ASFW::Driver {

/**
 * @brief Gap Count Optimization for FireWire Bus
 *
 * Implements IEEE 1394a gap count optimization based on Apple IOFireWireController
 * and Linux firewire subsystem. The gap count defines the mandatory silent period
 * between packets, ensuring fair arbitration and signal propagation.
 *
 * Default gap count of 63 assumes worst-case 16-hop daisy chain, wasting ~40% bandwidth.
 * This optimizer calculates the minimum safe gap count based on actual topology.
 *
 * References:
 * - IEEE 1394a-2000 Annex C, Table C-2 (for 4.5m cables, 144ns PHY delay)
 * - Apple IOFireWireController.cpp lines 3211-3321
 * - Linux firewire/core-card.c lines 481-485
 */
class GapCountOptimizer {
public:
    /**
     * @brief IEEE 1394a Table C-2: Gap count values for different hop counts
     *
     * Assumes:
     * - Cable length: up to 4.5 meters
     * - PHY delay: up to 144 nanoseconds
     * - Standard 1394a PHYs (not 1394b beta repeaters)
     *
     * Index = max hops, Value = gap count
     */
    static constexpr uint8_t GAP_TABLE[26] = {
        63,  // 0 hops (single node - use default)
        5,   // 1 hop
        7,   // 2 hops
        8,   // 3 hops
        10,  // 4 hops
        13,  // 5 hops
        16,  // 6 hops
        18,  // 7 hops
        21,  // 8 hops
        24,  // 9 hops
        26,  // 10 hops
        29,  // 11 hops
        32,  // 12 hops
        35,  // 13 hops
        37,  // 14 hops
        40,  // 15 hops
        43,  // 16 hops
        46,  // 17 hops
        48,  // 18 hops
        51,  // 19 hops
        54,  // 20 hops
        57,  // 21 hops
        59,  // 22 hops
        62,  // 23 hops
        63,  // 24 hops
        63   // 25+ hops (worst case)
    };

    /**
     * @brief Calculate gap count based on maximum hop count
     *
     * Assumes daisy-chain topology (worst case). For a bus with N nodes,
     * the root node ID is N-1, which equals the maximum hop count in a
     * daisy chain.
     *
     * @param maxHops Maximum hop count in topology (typically == rootNodeID)
     * @return Optimal gap count for given hop count
     */
    static uint8_t CalculateFromHops(uint8_t maxHops);

    /**
     * @brief Calculate gap count based on maximum ping time
     *
     * Ping time = round-trip signal propagation delay measured during Self-ID phase.
     * This is more accurate than hop count for complex topologies (stars, trees).
     *
     * Formula from Apple (line 3309):
     *   if (maxPing >= 29)
     *       gap = GAP_TABLE[(maxPing - 20) / 9]
     *   else
     *       gap = 5
     *
     * @param maxPingNs Maximum ping time in nanoseconds (measured from hardware)
     * @return Optimal gap count for given ping time
     */
    static uint8_t CalculateFromPing(uint32_t maxPingNs);

    /**
     * @brief Calculate optimal gap count using both hop count and ping time
     *
     * Follows Apple's dual-calculation approach:
     * 1. Calculate gap from hop count (conservative, assumes daisy chain)
     * 2. Calculate gap from ping time (accurate, measures actual propagation)
     * 3. Return the LARGER value (safer)
     *
     * If ping time is unavailable (e.g., FW642E chip limitations), falls back
     * to hop-count-only calculation.
     *
     * @param maxHops Maximum hop count (typically == rootNodeID)
     * @param maxPingNs Optional maximum ping time in nanoseconds
     * @return Optimal gap count (always in range [5, 63], never 0)
     */
    static uint8_t Calculate(uint8_t maxHops, std::optional<uint32_t> maxPingNs = std::nullopt);

    /**
     * @brief Determine if gap count should be updated
     *
     * Checks:
     * 1. Gap consistency: Are all nodes using the same gap count?
     * 2. Gap validity: Is any node using gap=0 (invalid)?
     * 3. Gap match: Does current gap match either new or previous gap?
     *
     * Following Apple's logic (lines 3378-3401):
     * - If gaps inconsistent → update
     * - If any gap == 0 → update (critical error)
     * - If gap doesn't match new OR previous → update
     *
     * @param currentGaps Gap counts extracted from Self-ID packets (one per node)
     * @param newGap Newly calculated optimal gap count
     * @param prevGap Gap count we set on previous reset (0xFF = never set)
     * @return true if gap count PHY packet should be sent
     */
    static bool ShouldUpdate(const std::vector<uint8_t>& currentGaps,
                            uint8_t newGap,
                            uint8_t prevGap = 0xFF);

    /**
     * @brief Check if gap counts are consistent across all nodes
     *
     * @param gaps Gap counts from Self-ID packets
     * @return true if all gaps are equal
     */
    static bool AreGapsConsistent(const std::vector<uint8_t>& gaps);

    /**
     * @brief Check if any node has an invalid gap count (0 or inconsistent)
     *
     * @param gaps Gap counts from Self-ID packets
     * @return true if gap=0 detected or gaps inconsistent
     */
    static bool HasInvalidGap(const std::vector<uint8_t>& gaps);
};

} // namespace ASFW::Driver
