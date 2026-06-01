// SPDX-License-Identifier: MIT
// Copyright (c) 2024 ASFireWire Project
//
// LocalRequestDispatch.cpp — see LocalRequestDispatch.hpp.

#include "LocalRequestDispatch.hpp"

#include "../../Hardware/IEEE1394.hpp"
#include "../PacketHelpers.hpp"
#include "../Tx/ResponseSender.hpp"
#include "PacketRouter.hpp"

namespace ASFW::Async {

namespace {
using AReq = HW::AsyncRequestHeader;

[[nodiscard]] uint32_t ReadQuadletDataLE(std::span<const uint8_t> header) noexcept {
    // Write-quadlet data lives in Q3 (header bytes 12..15), stored little-endian
    // by OHCI AR DMA; an LE read yields the host-order data quadlet.
    if (header.size() < 16) {
        return 0;
    }
    return static_cast<uint32_t>(header[12]) | (static_cast<uint32_t>(header[13]) << 8) |
           (static_cast<uint32_t>(header[14]) << 16) | (static_cast<uint32_t>(header[15]) << 24);
}
} // namespace

void LocalRequestDispatch::AddHandler(std::unique_ptr<ILocalAddressHandler> handler) {
    if (handler) {
        handlers_.push_back(std::move(handler));
    }
}

LocalRequestResult LocalRequestDispatch::Route(const LocalRequestContext& ctx) const {
    for (const auto& handler : handlers_) {
        const auto result = handler->HandleLocalRequest(ctx);
        if (result.claimed) {
            return result;
        }
    }
    return LocalRequestResult::NotMine();
}

ResponseCode LocalRequestDispatch::DispatchView(const ARPacketView& view, uint32_t generation) {
    LocalRequestContext ctx{};
    ctx.tCode = view.tCode;
    ctx.sourceID = view.sourceID;
    ctx.generation = generation;
    ctx.destOffset = ExtractDestOffset(view.header);

    const bool isReadQuad = (view.tCode == AReq::kTcodeReadQuad);
    const bool isReadBlock = (view.tCode == AReq::kTcodeReadBlock);
    const bool isLock = (view.tCode == AReq::kTcodeLockRequest);

    if (view.tCode == AReq::kTcodeWriteQuad && view.header.size() >= 16) {
        ctx.quadletData = ReadQuadletDataLE(view.header);
        ctx.writePayload = std::span<const uint8_t>(view.header.data() + 12, 4);
    } else if (view.tCode == AReq::kTcodeWriteBlock) {
        ctx.writePayload = view.payload;
        ctx.dataLength = ExtractDataLength(view.header);
    } else if (isReadBlock) {
        ctx.dataLength = ExtractDataLength(view.header);
    } else if (isLock) {
        ctx.writePayload = view.payload;
        ctx.dataLength = ExtractDataLength(view.header);
        ctx.extendedTCode = ExtractExtendedTCode(view.header);
    }

    const auto result = Route(ctx);
    const ResponseCode rcode = result.claimed ? result.rcode : ResponseCode::AddressError;

    if (isReadQuad) {
        if (sender_ != nullptr) {
            sender_->SendReadQuadletResponse(view, rcode, result.readQuadlet);
        }
        return ResponseCode::NoResponse;
    }
    if (isReadBlock) {
        if (sender_ != nullptr) {
            if (rcode == ResponseCode::Complete) {
                sender_->SendReadBlockResponse(view, rcode, result.readBlockDeviceAddress,
                                               result.readBlockLength);
            } else {
                sender_->SendReadBlockResponse(view, rcode, 0, 0);
            }
        }
        return ResponseCode::NoResponse;
    }
    if (isLock) {
        if (sender_ != nullptr) {
            sender_->SendLockResponse(view, rcode, result.lockResponseQuadlet);
        }
        return ResponseCode::NoResponse;
    }

    // Write tCodes: PacketRouter sends the write response from this rcode.
    return rcode;
}

void LocalRequestDispatch::Install(PacketRouter& router, ResponseSender* sender) {
    sender_ = sender;

    // Deliver generation to LocalRequestDispatch so that context has correct generation.
    const auto wire = [this](const ARPacketView& view, uint32_t generation) {
        return DispatchView(view, generation);
    };

    router.RegisterRequestHandler(AReq::kTcodeWriteQuad, wire);
    router.RegisterRequestHandler(AReq::kTcodeWriteBlock, wire);
    router.RegisterRequestHandler(AReq::kTcodeReadQuad, wire);
    router.RegisterRequestHandler(AReq::kTcodeReadBlock, wire);
    router.RegisterRequestHandler(AReq::kTcodeLockRequest, wire);
}

} // namespace ASFW::Async
