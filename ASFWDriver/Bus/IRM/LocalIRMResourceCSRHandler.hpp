// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// LocalIRMResourceCSRHandler.hpp — inbound local CSR adapter for OHCI IRM resources.

#pragma once

#include "../../Async/Rx/LocalRequestDispatch.hpp"
#include "../../Hardware/HardwareInterface.hpp"

namespace ASFW::Bus {

class LocalIRMResourceCSRHandler final : public Async::ILocalAddressHandler {
public:
    explicit LocalIRMResourceCSRHandler(Driver::HardwareInterface* hardware) noexcept
        : hardware_(hardware) {}

    [[nodiscard]] const char* Name() const noexcept override { return "IRMResourceCSR"; }

    [[nodiscard]] Async::LocalRequestResult HandleLocalRequest(
        const Async::LocalRequestContext& ctx) override;

private:
    Driver::HardwareInterface* hardware_{nullptr};
};

} // namespace ASFW::Bus
