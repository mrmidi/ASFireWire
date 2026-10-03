// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Asynchronous register transport for the MOTU 828 Mk3 V3 protocol.

#pragma once

#include "MOTU828Mk3RegisterModel.hpp"
#include "../../../Protocols/Ports/ProtocolRegisterIO.hpp"

#include <DriverKit/IOReturn.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace ASFW::Audio::MOTU {

class MOTU828Mk3RegisterIO final {
public:
    using CompletionCallback = std::function<void(IOReturn)>;
    using ReadCallback = std::function<void(IOReturn, uint32_t)>;

    MOTU828Mk3RegisterIO(Protocols::Ports::FireWireBusOps& busOps,
                         Protocols::Ports::FireWireBusInfo& busInfo,
                         Discovery::DeviceRegistry& routeRegistry,
                         const Discovery::DeviceRouteToken& route) noexcept;

    void UpdateRoute(const Discovery::DeviceRouteToken& route) noexcept;

    void RunPrepareSequence(CompletionCallback callback);
    void ReadQuad(uint32_t offset, ReadCallback callback);
    void WriteQuad(uint32_t offset, uint32_t value, CompletionCallback callback);

private:
    struct PrepareState;

    [[nodiscard]] static Async::FWAddress RegisterAddress(uint32_t offset) noexcept;
    void SubmitPrepareStep(const std::shared_ptr<PrepareState>& state);

    Protocols::Ports::ProtocolRegisterIO io_;
};

} // namespace ASFW::Audio::MOTU
