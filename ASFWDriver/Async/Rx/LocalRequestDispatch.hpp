// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// LocalRequestDispatch.hpp — single owner of inbound AR *request* routing to the
// local node's address space.
//
// Previously each protocol grabbed a whole tCode handler slot in PacketRouter
// (SBP-2 took 0x0/0x1/0x4/0x5, DICE took 0x0, FCP took 0x1) and silently
// clobbered one another. This component is the one place that owns tCodes
// 0x0/0x1/0x4/0x5 and routes each request, by destination address, to a list of
// registered participants (CSR / SBP-2 / FCP / DICE). It mirrors Linux's
// fw_core_add_address_handler model: each participant declares the addresses it
// owns and answers only those.
//
// The Route() core is pure logic (host-testable). Install() and DispatchView()
// are the thin production glue that build a context from an ARPacketView and emit
// the response via ResponseSender.

#pragma once

#include "../ResponseCode.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ASFW::Async {

struct ARPacketView;
class PacketRouter;
class ResponseSender;

// Normalized view of an inbound local request, independent of OHCI byte layout.
struct LocalRequestContext {
    uint64_t destOffset{0};  // 48-bit destination offset (high16 == 0xFFFF for CSR space)
    uint8_t tCode{0};
    uint16_t sourceID{0};
    uint32_t generation{0};
    uint32_t quadletData{0};            // host-order data quadlet (write-quadlet)
    std::span<const uint8_t> writePayload{}; // raw bytes: write-quadlet = 4B, write-block = N
    uint32_t dataLength{0};             // block length (block read/write)
};

// Result of a participant claiming (or declining) a request.
struct LocalRequestResult {
    bool claimed{false};                 // false => try the next participant
    ResponseCode rcode{ResponseCode::AddressError};
    uint32_t readQuadlet{0};             // read-quadlet response value
    uint64_t readBlockDeviceAddress{0};  // read-block DMA payload address
    uint32_t readBlockLength{0};         // read-block payload length

    static LocalRequestResult NotMine() noexcept { return {}; }
    static LocalRequestResult Write(ResponseCode rc) noexcept {
        return {.claimed = true, .rcode = rc};
    }
    static LocalRequestResult Quadlet(ResponseCode rc, uint32_t value) noexcept {
        return {.claimed = true, .rcode = rc, .readQuadlet = value};
    }
    static LocalRequestResult Block(ResponseCode rc, uint64_t addr, uint32_t len) noexcept {
        return {.claimed = true, .rcode = rc, .readBlockDeviceAddress = addr, .readBlockLength = len};
    }
};

// A protocol participant that owns some local address range(s).
struct ILocalAddressHandler {
    virtual ~ILocalAddressHandler() = default;
    [[nodiscard]] virtual LocalRequestResult HandleLocalRequest(const LocalRequestContext& ctx) = 0;
    [[nodiscard]] virtual const char* Name() const noexcept = 0;
};

class LocalRequestDispatch {
public:
    LocalRequestDispatch() = default;

    // Participants are tried in registration order; first claim wins. The
    // dispatch owns the handler.
    void AddHandler(std::unique_ptr<ILocalAddressHandler> handler);

    // Pure routing core. Returns the first participant's claim, or NotMine.
    [[nodiscard]] LocalRequestResult Route(const LocalRequestContext& ctx) const;

    [[nodiscard]] size_t HandlerCount() const noexcept { return handlers_.size(); }

    // Production: register this dispatch as the single owner of request tCodes
    // 0x0/0x1/0x4/0x5 on the router, and remember the sender for read responses.
    void Install(PacketRouter& router, ResponseSender* sender);

private:
    // Build a context from an ARPacketView, route it, and emit the response.
    // Returns the rcode for the PacketRouter auto write-response path, or
    // NoResponse when the read response was sent directly.
    ResponseCode DispatchView(const ARPacketView& view, uint32_t generation);

    std::vector<std::unique_ptr<ILocalAddressHandler>> handlers_;
    ResponseSender* sender_{nullptr};
};

} // namespace ASFW::Async
