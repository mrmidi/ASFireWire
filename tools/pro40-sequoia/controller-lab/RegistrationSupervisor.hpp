// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RegistrationLifecycle.hpp"
namespace Pro40Lab {
// Called by the outer controller's serial-queue heartbeat. Never infer retirement
// from a blocked state alone: callback ownership must first be discharged.
template<class Service>
class RegistrationSupervisor final {
    std::shared_ptr<RegistrationLifecycle<Service>> owner_;
public:
    explicit RegistrationSupervisor(std::shared_ptr<RegistrationLifecycle<Service>> owner)
        : owner_(std::move(owner)) {}
    [[nodiscard]] bool pollAndRetire() {
        if (!owner_) return true;
        owner_->poll();
        if (!owner_->readyToRetire()) return false;
        owner_.reset();
        return true;
    }
};
}
