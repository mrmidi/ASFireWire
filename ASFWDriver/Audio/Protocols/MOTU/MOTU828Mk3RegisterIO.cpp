// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "MOTU828Mk3RegisterIO.hpp"

#include "../../../Common/WireFormat.hpp"
#include "../../../Logging/Logging.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace ASFW::Audio::MOTU {

struct MOTU828Mk3RegisterIO::PrepareState final {
    size_t index{0};
    std::array<uint8_t, sizeof(uint32_t) * 2> wirePayload{};
    CompletionCallback completion;
};

MOTU828Mk3RegisterIO::MOTU828Mk3RegisterIO(
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    Discovery::DeviceRegistry& routeRegistry,
    const Discovery::DeviceRouteToken& route) noexcept
    : io_(busOps, busInfo, routeRegistry, route) {}

void MOTU828Mk3RegisterIO::UpdateRoute(
    const Discovery::DeviceRouteToken& route) noexcept {
    io_.UpdateRoute(route);
}

Async::FWAddress MOTU828Mk3RegisterIO::RegisterAddress(uint32_t offset) noexcept {
    return Async::FWAddress{Async::FWAddress::AddressParts{
        .addressHi = kRegisterAddressHi,
        .addressLo = kRegisterAddressBase + offset,
    }};
}

void MOTU828Mk3RegisterIO::RunPrepareSequence(CompletionCallback callback) {
    if (!callback) {
        return;
    }

    auto state = std::make_shared<PrepareState>();
    state->completion = std::move(callback);
    SubmitPrepareStep(state);
}

void MOTU828Mk3RegisterIO::SubmitPrepareStep(
    const std::shared_ptr<PrepareState>& state) {
    if (state->index >= kPrepareWrites.size()) {
        auto completion = std::move(state->completion);
        completion(kIOReturnSuccess);
        return;
    }

    const RegisterWrite write = kPrepareWrites[state->index];
    const size_t stepIndex = state->index;
    auto onWrite = [this, state, write, stepIndex](Async::AsyncStatus status) {
        const IOReturn result = Protocols::Ports::MapAsyncStatusToIOReturn(status);
        ASFW_LOG(DICE,
                 "[MotuChoreo] prepare[%zu] offset=0x%04x status=%{public}s result=0x%08x",
                 stepIndex, write.offset, Async::ToString(status), result);
        if (result != kIOReturnSuccess && write.fatalOnFailure) {
            auto completion = std::move(state->completion);
            completion(result);
            return;
        }

        ++state->index;
        SubmitPrepareStep(state);
    };

    if (write.quadletCount == 1) {
        ASFW_LOG(DICE, "[MotuChoreo] prepare[%zu] offset=0x%04x quad0=0x%08x", stepIndex,
                 write.offset, write.quadlets[0]);
        (void)io_.WriteQuadBE(RegisterAddress(write.offset),
                              write.quadlets[0],
                              std::move(onWrite));
        return;
    }

    ASFW_LOG(DICE, "[MotuChoreo] prepare[%zu] offset=0x%04x quad0=0x%08x quad1=0x%08x", stepIndex,
             write.offset, write.quadlets[0], write.quadlets[1]);
    auto wirePayload = std::span<uint8_t>(state->wirePayload);
    for (uint8_t i = 0; i < write.quadletCount; ++i) {
        const size_t offset = sizeof(uint32_t) * i;
        FW::WriteBE32(wirePayload.subspan(offset, sizeof(uint32_t)).data(),
                      write.quadlets[i]);
    }
    const size_t payloadSize = sizeof(uint32_t) * write.quadletCount;
    (void)io_.WriteBlock(RegisterAddress(write.offset),
                         std::span<const uint8_t>(wirePayload.first(payloadSize)),
                         std::move(onWrite));
}

void MOTU828Mk3RegisterIO::ReadQuad(uint32_t offset, ReadCallback callback) {
    if (!callback) {
        return;
    }
    ASFW_LOG(DICE, "[MotuChoreo] read offset=0x%04x", offset);
    (void)io_.ReadQuadBE(
        RegisterAddress(offset),
        [callback = std::move(callback), offset](Async::AsyncStatus status, uint32_t value) mutable {
            const IOReturn result = Protocols::Ports::MapAsyncStatusToIOReturn(status);
            ASFW_LOG(DICE,
                     "[MotuChoreo] read offset=0x%04x value=0x%08x status=%{public}s result=0x%08x",
                     offset, value, Async::ToString(status), result);
            callback(result, value);
        });
}

void MOTU828Mk3RegisterIO::WriteQuad(uint32_t offset,
                                     uint32_t value,
                                     CompletionCallback callback) {
    if (!callback) {
        return;
    }
    ASFW_LOG(DICE, "[MotuChoreo] write offset=0x%04x value=0x%08x", offset, value);
    (void)io_.WriteQuadBE(
        RegisterAddress(offset),
        value,
        [callback = std::move(callback), offset, value](Async::AsyncStatus status) mutable {
            const IOReturn result = Protocols::Ports::MapAsyncStatusToIOReturn(status);
            ASFW_LOG(DICE,
                     "[MotuChoreo] write offset=0x%04x value=0x%08x status=%{public}s result=0x%08x",
                     offset, value, Async::ToString(status), result);
            callback(result);
        });
}

} // namespace ASFW::Audio::MOTU
