// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Publishes one ASFWSBP2Nub for each currently discovered SBP-2 unit.

#pragma once

#include "../Discovery/IDeviceManager.hpp"

#include <DriverKit/IOLib.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

class IOService;
class IODispatchQueue;
class ASFWSBP2Nub;

namespace ASFW::Protocols::SBP2 {

class SBP2NubPublisher final : public Discovery::IUnitObserver,
                               public std::enable_shared_from_this<SBP2NubPublisher> {
public:
    SBP2NubPublisher(IOService* driver,
                     Discovery::IDeviceManager& deviceManager,
                     IODispatchQueue* workQueue) noexcept;
    ~SBP2NubPublisher() override;

    SBP2NubPublisher(const SBP2NubPublisher&) = delete;
    SBP2NubPublisher& operator=(const SBP2NubPublisher&) = delete;

    // Begin observing real discovered units. Existing ready units are adopted.
    void Start();

    // Unregister, terminate every published child, and reject queued work.
    // This is terminal for this publisher instance.
    void Shutdown() noexcept;

    void OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitResumed(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) override;

private:
    using UnitKey = Discovery::UnitInstanceId;

    struct PublishedNub {
        std::weak_ptr<Discovery::FWUnit> unit;
        ASFWSBP2Nub* nub{nullptr};
    };

    [[nodiscard]] static bool IsSBP2Unit(const std::shared_ptr<Discovery::FWUnit>& unit) noexcept;
    [[nodiscard]] static std::optional<UnitKey> MakeKey(
        const std::shared_ptr<Discovery::FWUnit>& unit) noexcept;

    void QueueEnsure(std::shared_ptr<Discovery::FWUnit> unit);
    void QueueTerminate(std::shared_ptr<Discovery::FWUnit> unit);
    void EnsureNub(const std::shared_ptr<Discovery::FWUnit>& unit) noexcept;
    void TerminateNub(const std::shared_ptr<Discovery::FWUnit>& unit,
                      const char* reasonTag) noexcept;

    IOService* driver_{nullptr};
    Discovery::IDeviceManager& deviceManager_;
    IODispatchQueue* workQueue_{nullptr};
    IOLock* lock_{nullptr};
    bool observing_{false};
    bool stopping_{false};
    std::unordered_map<UnitKey, PublishedNub, Discovery::UnitInstanceIdHash> nubsByUnit_;
};

} // namespace ASFW::Protocols::SBP2
