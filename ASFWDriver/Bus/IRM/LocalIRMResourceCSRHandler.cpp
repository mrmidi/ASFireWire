// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// LocalIRMResourceCSRHandler.cpp — see LocalIRMResourceCSRHandler.hpp

#include "LocalIRMResourceCSRHandler.hpp"

#include "../../Async/ResponseCode.hpp"
#include "../../Common/CSRSpace.hpp"
#include "../../Hardware/IEEE1394.hpp"
#include "../../Logging/Logging.hpp"
#include "IRMCSRConstants.hpp"

#include <cstdint>
#include <cstring>
#include <bit>
#include <optional>

namespace ASFW::Bus {

namespace {

using ASFW::Async::LocalRequestContext;
using ASFW::Async::LocalRequestResult;
using ASFW::Async::ResponseCode;
using AReq = ASFW::Async::HW::AsyncRequestHeader;

[[nodiscard]] std::optional<uint32_t> SelectorForOffset(uint32_t offset) noexcept {
    using namespace ASFW::Driver::IRMCSR;
    switch (offset) {
    case ASFW::FW::kCSR_BusManagerID:
        return static_cast<uint32_t>(CSRSelector::BusManagerId);
    case ASFW::FW::kCSR_BandwidthAvailable:
        return static_cast<uint32_t>(CSRSelector::BandwidthAvailable);
    case ASFW::FW::kCSR_ChannelsAvailableHi:
        return static_cast<uint32_t>(CSRSelector::ChannelsAvailableHi);
    case ASFW::FW::kCSR_ChannelsAvailableLo:
        return static_cast<uint32_t>(CSRSelector::ChannelsAvailableLo);
    default:
        return std::nullopt;
    }
}

[[nodiscard]] ResponseCode RCodeForStatus(
    ASFW::Driver::LocalCSRLockResult::Status status) noexcept {
    switch (status) {
    case ASFW::Driver::LocalCSRLockResult::Status::Success:
        return ResponseCode::Complete;
    case ASFW::Driver::LocalCSRLockResult::Status::Timeout:
        return ResponseCode::Busy;
    case ASFW::Driver::LocalCSRLockResult::Status::HardwareUnavailable:
        return ResponseCode::AddressError;
    }
    return ResponseCode::AddressError;
}

[[nodiscard]] bool ReadBE32(std::span<const uint8_t> bytes,
                            std::size_t offset,
                            uint32_t& out) noexcept {
    if (bytes.size() < offset + sizeof(uint32_t)) {
        return false;
    }
    uint32_t raw = 0;
    std::memcpy(&raw, bytes.data() + offset, sizeof(raw));
    if constexpr (std::endian::native == std::endian::little) {
        raw = std::byteswap(raw);
    }
    out = raw;
    return true;
}

} // namespace

LocalRequestResult LocalIRMResourceCSRHandler::HandleLocalRequest(
    const LocalRequestContext& ctx) {
    if (hardware_ == nullptr || (ctx.destOffset >> 32) != 0xFFFFu) {
        return LocalRequestResult::NotMine();
    }

    const uint32_t offset = static_cast<uint32_t>(ctx.destOffset & 0xFFFFFFFFu);
    const auto selector = SelectorForOffset(offset);
    if (!selector.has_value()) {
        return LocalRequestResult::NotMine();
    }

    // cross-validated with Linux: ohci.c:1653 Apple: IOFireWireIRM.cpp:241
    if (ctx.tCode == AReq::kTcodeReadQuad) {
        const auto result = hardware_->ReadLocalIRMResource(*selector);
        return LocalRequestResult::Quadlet(RCodeForStatus(result.status), result.value);
    }

    if (ctx.tCode == AReq::kTcodeLockRequest) {
        if (ctx.extendedTCode != static_cast<uint16_t>(ASFW::FW::LockOp::kCompareSwap) ||
            ctx.dataLength != 8) {
            return LocalRequestResult::Lock(ResponseCode::TypeError, 0);
        }

        uint32_t compareValue = 0;
        uint32_t swapValue = 0;
        if (!ReadBE32(ctx.writePayload, 0, compareValue) ||
            !ReadBE32(ctx.writePayload, sizeof(uint32_t), swapValue)) {
            return LocalRequestResult::Lock(ResponseCode::TypeError, 0);
        }

        const auto result =
            hardware_->CompareSwapLocalIRMResource(*selector, compareValue, swapValue);
        return LocalRequestResult::Lock(RCodeForStatus(result.status), result.oldValue);
    }

    ASFW_LOG(Controller,
             "IRMResourceCSR: unsupported inbound tCode=0x%x offset=0x%x",
             ctx.tCode,
             offset);
    return LocalRequestResult::Write(ResponseCode::TypeError);
}

} // namespace ASFW::Bus
