// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// LocalRequestDispatch.cpp — see LocalRequestDispatch.hpp.

#include "LocalRequestDispatch.hpp"

#include "../../Hardware/IEEE1394.hpp"
#include "../../Logging/Logging.hpp"
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

    // Blind-spot instrumentation (LS-9000 wedge): a request no handler claims is
    // answered addr_error and was previously invisible — if the target's ORB
    // fetch lands here, "ORB fetched, no status" is really "fetch failed".
    if (!result.claimed) {
        ASFW_LOG(Async,
                 "AR request UNCLAIMED tLabel=%u tCode=0x%X src=0x%04X addr=0x%012llx len=%u → addr_error",
                 view.tLabel, view.tCode, view.sourceID,
                 static_cast<unsigned long long>(ctx.destOffset), ctx.dataLength);
    } else if (isReadBlock) {
        // Ties each ORB/page-table fetch to the tLabel of our response so an
        // "AT-resp DROPPED: tLabel=N" line identifies exactly which fetch the
        // target never got an answer to. Happy-path correlation, so V2-gated;
        // the UNCLAIMED branch above stays at default level (real anomaly).
        ASFW_LOG_V2(Async, "AR readBlock tLabel=%u src=0x%04X addr=0x%012llx len=%u rc=%u",
                 view.tLabel, view.sourceID,
                 static_cast<unsigned long long>(ctx.destOffset), ctx.dataLength,
                 static_cast<unsigned>(rcode));
    }

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
