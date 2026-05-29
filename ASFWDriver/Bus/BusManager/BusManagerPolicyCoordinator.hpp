// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// BusManagerPolicyCoordinator.hpp — Coordinates Bus Manager policy decisions (FW-14).

#pragma once

#include "BusManagerRuntimeState.hpp"
#include "../../Async/FireWireBusImpl.hpp"
#include <memory>

namespace ASFW::Driver {
class HardwareInterface;
}

namespace ASFW::Bus {

class BusManagerPolicyCoordinator {
public:
    struct Deps {
        ASFW::Driver::HardwareInterface* hardware{nullptr};
        ASFW::Async::FireWireBusImpl* busImpl{nullptr};
    };

    explicit BusManagerPolicyCoordinator(Deps deps) noexcept;
    ~BusManagerPolicyCoordinator() = default;

    void Evaluate(const BusManagerRuntimeState& state) noexcept;

private:
    Deps deps_;
    uint32_t generation_{0};
    bool remoteCmstrInFlight_{false};
};

} // namespace ASFW::Bus
