// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "BeBoBBootloaderPreparation.hpp"
#include "../../../Async/Interfaces/IFireWireBusOps.hpp"
#include "../../../Discovery/DeviceRegistry.hpp"

#include <DriverKit/IOLib.h>
#include <functional>
#include <set>

namespace ASFW::Protocols::BeBoB::Bootloader {

/// Owns the production identity/route/one-shot gate and starts the bounded
/// BootROM-read then closed-cue sequence. Caller supplies a liveness predicate
/// so callbacks can stop safely when their service owner shuts down.
class BeBoBBootloaderPreparationCoordinator final {
public:
    BeBoBBootloaderPreparationCoordinator(Async::IFireWireBusOps& bus,
                                          Discovery::DeviceRegistry& registry) noexcept;
    ~BeBoBBootloaderPreparationCoordinator();

    BeBoBBootloaderPreparationCoordinator(const BeBoBBootloaderPreparationCoordinator&) = delete;
    BeBoBBootloaderPreparationCoordinator& operator=(
        const BeBoBBootloaderPreparationCoordinator&) = delete;

    [[nodiscard]] bool Prepare(uint32_t vendorId, uint32_t modelId,
                               const Discovery::DeviceIdentityEvidence& identity,
                               const Discovery::DeviceRouteToken& route,
                               FW::FwSpeed speed,
                               std::function<bool()> ownerAlive);

private:
    Async::IFireWireBusOps& bus_;
    Discovery::DeviceRegistry& registry_;
    IOLock* lock_{nullptr};
    std::set<std::pair<Discovery::Guid64, uint64_t>> attemptsByIncarnation_;
};

} // namespace ASFW::Protocols::BeBoB::Bootloader
