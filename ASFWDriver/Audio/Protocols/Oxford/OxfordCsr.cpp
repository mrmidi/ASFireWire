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
                    Async::FWAddress address,
                    const char* registerName,
                    RouteProvider currentRoute,
                    IdCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));

    // Two distinct faults that used to share one message and one branch: a
    // provider the caller never wired is a programming error, a device with no
    // live route is ordinary bus state. Collapsing them makes the first one
    // undiagnosable.
    if (!currentRoute) {
        ASFW_LOG_ERROR(Oxfw, "CSR %{public}s: no route provider wired", registerName);
        Common::InvokeSharedCallback(callbackState, kIOReturnNotReady, 0U);
        return;
    }

    const auto issued = currentRoute();
    if (!issued.has_value()) {
        ASFW_LOG_ERROR(Oxfw, "CSR %{public}s: device has no live route", registerName);
        Common::InvokeSharedCallback(callbackState, kIOReturnNotReady, 0U);
        return;
    }

    busOps.ReadBlock(
        issued->generation,
        FW::NodeId{static_cast<uint8_t>(issued->nodeId)},
        address,
        4,
        FW::FwSpeed::S100,
        [callbackState, currentRoute = std::move(currentRoute), registerName, issued = *issued](
            Async::AsyncStatus status, std::span<const uint8_t> payload) {
            // A bus reset between issue and completion invalidates the answer:
            // the quadlet may have come from whatever now occupies that node.
            // All three causes return kIOReturnError, so name them here or the
            // caller cannot tell a reset from a device that answered badly.
            const auto now = currentRoute();
            if (!now.has_value() || *now != issued) {
                // nodeId and generation are the fields most likely to still
                // match, so logging only those cannot explain the failure.
                ASFW_LOG_ERROR(Oxfw,
                               "CSR %{public}s: route retired during read "
                               "(issued epoch=%llu incarnation=%llu, now epoch=%llu)",
                               registerName,
                               static_cast<unsigned long long>(issued.routeEpoch),
                               static_cast<unsigned long long>(issued.deviceIncarnation),
                               static_cast<unsigned long long>(
                                   now.has_value() ? now->routeEpoch : 0ULL));
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
                    RouteProvider currentRoute,
                    IdCallback callback) {
    ReadIdRegister(busOps, FirmwareIdAddress(), "firmwareId",
                   std::move(currentRoute),
                   [callback = std::move(callback)](IOReturn status, uint32_t id) {
                       if (status == kIOReturnSuccess) {
                           ASFW_LOG(Oxfw, "CSR firmwareId=0x%08x", id);
                       }
                       callback(status, id);
                   });
}

void ReadHardwareId(Async::IFireWireBusOps& busOps,
                    RouteProvider currentRoute,
                    IdCallback callback) {
    ReadIdRegister(busOps, HardwareIdAddress(), "hardwareId",
                   std::move(currentRoute),
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
