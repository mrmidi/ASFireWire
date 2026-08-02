// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// SpeedMapService.hpp — Local software-owned SPEED_MAP CSR register (FW-20/M9).

#pragma once

#include "CSRResponder.hpp"
#include "../../Common/FWCommon.hpp"
#include "../TopologyTypes.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ASFW::Bus {

/**
 * @brief Status of the computed SPEED_MAP.
 */
enum class SpeedMapStatus : uint8_t {
    Invalid = 0,
    Valid,
    ConservativeFallback,    ///< S100 used due to unknown/beta path.
    UnsupportedBetaPath,     ///< Beta repeaters present, path details unknown.
};

/**
 * @brief FireWire speed codes as defined in IEEE 1394.
 */
enum class FireWireSpeedCode : uint8_t {
    S100 = 0,
    S200 = 1,
    S400 = 2,
    S800 = 3,
    Unknown = 0xFF,
};

/**
 * @brief Flat snapshot of the SPEED_MAP state for diagnostics.
 */
struct SpeedMapSnapshot {
    uint32_t generation{0};
    SpeedMapStatus status{SpeedMapStatus::Invalid};

    uint8_t nodeCount{0};
    uint8_t localNodeId{0x3F};
    uint8_t rootNodeId{0x3F};

    bool topologyValid{false};
    bool betaRepeatersPresent{false};

    FireWireSpeedCode speedMatrix[64][64]{};

    uint32_t encodedLengthQuadlets{0};
    uint16_t checksum{0};
};

/**
 * @brief Manages the generation-scoped legacy SPEED_MAP CSR region.
 *
 * SPEED_MAP is obsolete in IEEE 1394-2008. ASFW still serves a bounded
 * synthetic 0x400-byte legacy image for old readers and diagnostics.
 */
class SpeedMapService final : public ISpeedMapProvider {
public:
    SpeedMapService() noexcept;
    ~SpeedMapService() override = default;

    // Disable copy/move
    SpeedMapService(const SpeedMapService&) = delete;
    SpeedMapService& operator=(const SpeedMapService&) = delete;

    /**
     * @brief Invalidates the map for a new generation.
     */
    void Invalidate(uint32_t generation) noexcept;

    /**
     * @brief Computes and publishes a new map from a topology snapshot.
     */
    bool PublishFromTopology(const ASFW::Driver::TopologySnapshot& topology) noexcept;

    /**
     * @brief Reads a quadlet from the encoded map image.
     * @param offsetWithinSpeedMap Byte offset relative to kCSR_SpeedMapBase.
     */
    [[nodiscard]] bool ReadQuadlet(uint32_t offsetWithinSpeedMap,
                                   uint32_t& outValue) const noexcept override;

    [[nodiscard]] const SpeedMapSnapshot& Snapshot() const noexcept {
        return snapshot_;
    }

    [[nodiscard]] std::span<const uint32_t> EncodedQuadlets() const noexcept {
        return {encoded_.data(), encodedQuadletCount_};
    }

private:
    bool ComputeMatrix(const ASFW::Driver::TopologySnapshot& topology) noexcept;
    void EncodeCSRImage() noexcept;

    SpeedMapSnapshot snapshot_{};
    std::array<uint32_t, 1024> encoded_{};
    uint32_t encodedQuadletCount_{0};
};

} // namespace ASFW::Bus
