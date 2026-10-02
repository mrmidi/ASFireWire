// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "BeBoBBootloaderPreparation.hpp"
#include "../../../Async/Interfaces/IFireWireBusOps.hpp"
#include "../../../Discovery/DeviceRegistry.hpp"

#include <DriverKit/IOLib.h>
#include <functional>
#include <set>

namespace ASFW::Protocols::BeBoB::Bootloader {

/// Owns the production identity/route gate and starts the bounded BootROM-read
/// then closed-cue sequence. One run per device incarnation at a time, and at
/// most one cue per incarnation: a later run (the device back on a new route
/// after the cue) only reads, to confirm the firmware is running. Caller
/// supplies a liveness predicate so callbacks stop when their owner shuts down;
/// the coordinator must outlive every owner for which that predicate is true.
class BeBoBBootloaderPreparationCoordinator final {
public:
    BeBoBBootloaderPreparationCoordinator(Async::IFireWireBusOps& bus,
                                          Discovery::DeviceRegistry& registry) noexcept;
    ~BeBoBBootloaderPreparationCoordinator();

    BeBoBBootloaderPreparationCoordinator(const BeBoBBootloaderPreparationCoordinator&) = delete("runs call back into this coordinator; it must keep one address");
    BeBoBBootloaderPreparationCoordinator& operator=(
        const BeBoBBootloaderPreparationCoordinator&) = delete("runs call back into this coordinator; it must keep one address");

    [[nodiscard]] bool Prepare(const DeviceProfiles::Audio::StaticAudioEndpointPlan& plan,
                               uint32_t vendorId, uint32_t modelId,
                               const Discovery::DeviceRouteToken& route,
                               FW::FwSpeed speed,
                               std::function<bool()> ownerAlive,
                               std::function<void(PreparationState)> completion = {});

private:
    Async::IFireWireBusOps& bus_;
    Discovery::DeviceRegistry& registry_;
    IOLock* lock_{nullptr};
    std::set<std::pair<Discovery::Guid64, uint64_t>> runsInFlight_;
    std::set<std::pair<Discovery::Guid64, uint64_t>> cuedIncarnations_;
};

} // namespace ASFW::Protocols::BeBoB::Bootloader
