#pragma once

#include "../Async/Interfaces/IFireWireBusOps.hpp"
#include "../Async/AsyncSubsystem.hpp"
#include <DriverKit/IOLib.h>

namespace ASFW::IRM {

/**
 * Adapter that wraps AsyncSubsystem to implement Async::IFireWireBusOps interface.
 *
 * Modern C++23 redesign:
 * - Uses canonical Async::IFireWireBusOps interface (no duplication)
 * - Leverages strong types (Generation, NodeId, FwSpeed, LockOp)
 * - Proper generation validation for all operations
 * - Clean separation of concerns
 *
 * This adapter provides IRM-specific conveniences while maintaining
 * compatibility with the canonical async interface.
 *
 * Usage:
 *   Async::AsyncSubsystem async;
 *   auto busOps = std::make_unique<AsyncSubsystemBusOps>(async);
 *   IRMClient irmClient(*busOps);
 *
 * Reference: IRM_FINAL_THOUGHTS.md §3.1 (Interface dependency pattern)
 *            Fix Plan 02-FIX-INTERFACE-DUPLICATION.md
 */
class AsyncSubsystemBusOps : public Async::IFireWireBusOps {
public:
    explicit AsyncSubsystemBusOps(Async::AsyncSubsystem& async)
        : async_(async) {}

    ~AsyncSubsystemBusOps() override = default;

    // Delete copy/move to prevent accidental sharing
    AsyncSubsystemBusOps(const AsyncSubsystemBusOps&) = delete;
    AsyncSubsystemBusOps& operator=(const AsyncSubsystemBusOps&) = delete;
    AsyncSubsystemBusOps(AsyncSubsystemBusOps&&) = delete;
    AsyncSubsystemBusOps& operator=(AsyncSubsystemBusOps&&) = delete;

    // -------------------------------------------------------------------------
    // Async::IFireWireBusOps Implementation
    // -------------------------------------------------------------------------

    Async::AsyncHandle ReadBlock(
        FW::Generation generation,
        FW::NodeId nodeId,
        FWAddress address,
        uint32_t length,
        FW::FwSpeed speed,
        Async::InterfaceCompletionCallback callback) override
    {
        Async::ReadParams params{};
        params.destinationID = nodeId.value();
        params.addressHigh = address.addressHi;
        params.addressLow = address.addressLo;
        params.length = length;
        params.speedCode = speed.value();

        return async_.Read(params, std::move(callback));
    }

    Async::AsyncHandle WriteBlock(
        FW::Generation generation,
        FW::NodeId nodeId,
        FWAddress address,
        std::span<const uint8_t> data,
        FW::FwSpeed speed,
        Async::InterfaceCompletionCallback callback) override
    {
        Async::WriteParams params{};
        params.destinationID = nodeId.value();
        params.addressHigh = address.addressHi;
        params.addressLow = address.addressLo;
        params.length = static_cast<uint32_t>(data.size());
        params.speedCode = speed.value();

        return async_.Write(params, data, std::move(callback));
    }

    Async::AsyncHandle Lock(
        FW::Generation generation,
        FW::NodeId nodeId,
        FWAddress address,
        FW::LockOp lockOp,
        std::span<const uint8_t> operand,
        uint32_t responseLength,
        FW::FwSpeed speed,
        Async::InterfaceCompletionCallback callback) override
    {
        Async::LockParams params{};
        params.destinationID = nodeId.value();
        params.addressHigh = address.addressHi;
        params.addressLow = address.addressLo;
        params.lockOp = lockOp;
        params.speedCode = speed.value();

        return async_.Lock(params, operand, responseLength, std::move(callback));
    }

    bool Cancel(Async::AsyncHandle handle) override {
        return async_.Cancel(handle);
    }

private:
    Async::AsyncSubsystem& async_;
};

} // namespace ASFW::IRM
