// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// BusManagerPolicyCoordinator.hpp — Coordinates Bus Manager policy decisions (FW-14).

#pragma once

#include "BusManagerRuntimeState.hpp"
#include <cstdint>

namespace ASFW::Driver {
class HardwareInterface;
}

namespace ASFW::Bus {

struct IBMPolicyExecutor {
    virtual ~IBMPolicyExecutor() = default;
    virtual void SendRemoteCmstr(uint8_t rootNodeId, uint32_t generation) = 0;
};

class BusManagerPolicyCoordinator {
public:
    struct Deps {
        ASFW::Driver::HardwareInterface* hardware{nullptr};
        IBMPolicyExecutor* executor{nullptr};
    };

    explicit BusManagerPolicyCoordinator(Deps deps) noexcept;
    ~BusManagerPolicyCoordinator() = default;

    void Evaluate(BusManagerRuntimeState& state) noexcept;

private:
    Deps deps_;
    uint32_t generation_{0};
};

} // namespace ASFW::Bus
