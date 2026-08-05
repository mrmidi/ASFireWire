// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfordCsr.cpp — Oxford FW970/971 CSR reads.

#include "OxfordCsr.hpp"

#include "../../../Common/CallbackUtils.hpp"
#include "../../../Logging/Logging.hpp"

namespace ASFW::Audio::Oxford {

namespace {

[[nodiscard]] const char* AsicName(Asic asic) noexcept {
    switch (asic) {
        case Asic::kFw970:
            return "FW970";
        case Asic::kFw971:
            return "FW971";
        default:
            return "unknown";
    }
}

void ReadIdRegister(Async::IFireWireBusOps& busOps,
                    const Discovery::DeviceRouteToken& route,
                    Async::FWAddress address,
                    const char* registerName,
                    RouteValidator isRouteCurrent,
                    IdCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));

    if (!isRouteCurrent || !isRouteCurrent()) {
        ASFW_LOG_ERROR(Oxfw, "CSR %{public}s: route not current at issue (node=%u gen=%u)",
                       registerName, static_cast<unsigned>(route.nodeId),
                       static_cast<unsigned>(route.generation.value));
        Common::InvokeSharedCallback(callbackState, kIOReturnNotReady, 0U);
        return;
    }

    busOps.ReadBlock(
        route.generation,
        FW::NodeId{static_cast<uint8_t>(route.nodeId)},
        address,
        4,
        FW::FwSpeed::S100,
        [callbackState, isRouteCurrent, registerName](Async::AsyncStatus status,
                                                      std::span<const uint8_t> payload) {
            // A bus reset between issue and completion invalidates the answer:
            // the quadlet may have come from whatever now occupies that node.
            // All three causes return kIOReturnError, so name them here or the
            // caller cannot tell a reset from a device that answered badly.
            if (!isRouteCurrent()) {
                ASFW_LOG_ERROR(Oxfw, "CSR %{public}s: route retired during read", registerName);
                Common::InvokeSharedCallback(callbackState, kIOReturnError, 0U);
                return;
            }
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG_ERROR(Oxfw, "CSR %{public}s: read failed status=%d", registerName,
                               static_cast<int>(status));
                Common::InvokeSharedCallback(callbackState, kIOReturnError, 0U);
                return;
            }
            if (payload.size() < 4U) {
                ASFW_LOG_ERROR(Oxfw, "CSR %{public}s: short payload (%zu bytes, need 4)",
                               registerName, payload.size());
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
    ReadIdRegister(busOps, route, FirmwareIdAddress(), "firmwareId",
                   std::move(isRouteCurrent),
                   [callback = std::move(callback)](IOReturn status, uint32_t id) {
                       if (status == kIOReturnSuccess) {
                           ASFW_LOG(Oxfw, "CSR firmwareId=0x%08x", id);
                       }
                       callback(status, id);
                   });
}

void ReadHardwareId(Async::IFireWireBusOps& busOps,
                    const Discovery::DeviceRouteToken& route,
                    RouteValidator isRouteCurrent,
                    IdCallback callback) {
    ReadIdRegister(busOps, route, HardwareIdAddress(), "hardwareId",
                   std::move(isRouteCurrent),
                   [callback = std::move(callback)](IOReturn status, uint32_t id) {
                       if (status == kIOReturnSuccess) {
                           // The ASIC generation selects SYT policy, so record
                           // which one the device actually reported.
                           ASFW_LOG(Oxfw, "CSR hardwareId=0x%08x asic=%{public}s", id,
                                    AsicName(ClassifyHardwareId(id)));
                       }
                       callback(status, id);
                   });
}

} // namespace ASFW::Audio::Oxford
