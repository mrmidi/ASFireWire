// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CSRContractVerifier.cpp — see CSRContractVerifier.hpp

#include "CSRContractVerifier.hpp"

namespace ASFW::Bus {

CSRContractCheckResult CSRContractVerifier::Verify(const CSRResponder& responder,
                                                   const TopologyMapService& topologyMap,
                                                   const SpeedMapService& speedMap,
                                                   const LocalIRMResourceController& irm) const noexcept {
    CSRContractCheckResult result{};

    const uint32_t currentGen = topologyMap.GetGeneration();
    const auto speedSnap = speedMap.Snapshot();

    // 1. Generation checks.
    result.topologyMapGenerationMatch = topologyMap.IsValid() && currentGen != 0;
    result.speedMapGenerationMatch =
        result.topologyMapGenerationMatch && speedSnap.status != SpeedMapStatus::Invalid &&
        speedSnap.generation == currentGen;

    // 2. Ownership & Hit checks
    result.hardwareOwnedSoftwareHits = responder.UnexpectedResourceCsrSoftwareCount();
    
    // In M9, we can only verify hits that were recorded. 
    // Manual probe mode would be needed for a full exhaustive ownership check.
    
    if (result.hardwareOwnedSoftwareHits > 0) {
        result.ok = false;
    }

    if (!result.topologyMapGenerationMatch) {
        result.ok = false;
    }

    // SPEED_MAP is obsolete in IEEE 1394-2008. Keep reporting freshness for
    // legacy/diagnostic readers, but do not classify stale/invalid SPEED_MAP
    // state as a hard CSR ownership mismatch by itself.

    return result;
}

} // namespace ASFW::Bus
