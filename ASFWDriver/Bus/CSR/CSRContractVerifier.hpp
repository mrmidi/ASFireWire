// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// CSRContractVerifier.hpp — Diagnostic verifier for CSR ownership (Milestone 9).

#pragma once

#include "CSRContract.hpp"
#include "CSRResponder.hpp"
#include "TopologyMapService.hpp"
#include "SpeedMapService.hpp"
#include "../IRM/LocalIRMResourceController.hpp"
#include <cstdint>

namespace ASFW::Bus {

/**
 * @brief Results of a CSR contract verification check.
 */
struct CSRContractCheckResult {
    bool ok{true};

    uint32_t softwareAnsweredHardwareOwned{0};
    uint32_t hardwareOwnedSoftwareHits{0};
    uint32_t unsupportedAccesses{0};
    uint32_t staleGenerationReads{0};

    bool topologyMapGenerationMatch{false};
    bool speedMapGenerationMatch{false};
};

/**
 * @brief Helper for verifying that the local node's CSR interface matches the spec contract.
 *
 * This verifier detects ownership violations (e.g. software answering hardware-owned IRM
 * registers) and cross-map generation inconsistencies.
 */
class CSRContractVerifier final {
public:
    CSRContractVerifier() noexcept = default;

    /**
     * @brief Performs a validation pass over the provided services.
     */
    [[nodiscard]] CSRContractCheckResult Verify(const CSRResponder& responder,
                                               const TopologyMapService& topologyMap,
                                               const SpeedMapService& speedMap,
                                               const LocalIRMResourceController& irm) const noexcept;
};

} // namespace ASFW::Bus
