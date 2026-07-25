// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfordCsr.cpp — Oxford FW970/971 CSR reads.

#include "OxfordCsr.hpp"

#include "../../../Common/CallbackUtils.hpp"

namespace ASFW::Audio::Oxford {

namespace {

void ReadIdRegister(Async::IFireWireBusOps& busOps,
                    const Discovery::DeviceRouteToken& route,
                    Async::FWAddress address,
                    RouteValidator isRouteCurrent,
                    IdCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));

    if (!isRouteCurrent || !isRouteCurrent()) {
        Common::InvokeSharedCallback(callbackState, kIOReturnNotReady, 0U);
        return;
    }

    busOps.ReadBlock(
        route.generation,
        FW::NodeId{static_cast<uint8_t>(route.nodeId)},
        address,
        4,
        FW::FwSpeed::S100,
        [callbackState, isRouteCurrent](Async::AsyncStatus status,
                                        std::span<const uint8_t> payload) {
            // A bus reset between issue and completion invalidates the answer:
            // the quadlet may have come from whatever now occupies that node.
            if (!isRouteCurrent() || status != Async::AsyncStatus::kSuccess ||
                payload.size() < 4U) {
                Common::InvokeSharedCallback(callbackState, kIOReturnError, 0U);
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess,
                                         DecodeIdQuadlet(payload));
        });
}

} // namespace

void ReadFirmwareId(Async::IFireWireBusOps& busOps,
                    const Discovery::DeviceRouteToken& route,
                    RouteValidator isRouteCurrent,
                    IdCallback callback) {
    ReadIdRegister(busOps, route, FirmwareIdAddress(), std::move(isRouteCurrent),
                   std::move(callback));
}

void ReadHardwareId(Async::IFireWireBusOps& busOps,
                    const Discovery::DeviceRouteToken& route,
                    RouteValidator isRouteCurrent,
                    IdCallback callback) {
    ReadIdRegister(busOps, route, HardwareIdAddress(), std::move(isRouteCurrent),
                   std::move(callback));
}

} // namespace ASFW::Audio::Oxford
