#pragma once

#include "../Async/Interfaces/IAsyncControllerPort.hpp"
#include "../Async/Interfaces/IFireWireBusOps.hpp"
#include <DriverKit/IOLib.h>

namespace ASFW::IRM {

/**
 * Adapter that wraps the async controller port to implement Async::IFireWireBusOps.
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
 *   Async::IAsyncControllerPort& async = ...;
 *   auto busOps = std::make_unique<AsyncSubsystemBusOps>(async);
 *   IRMClient irmClient(*busOps);
 *
 * Reference: IRM_FINAL_THOUGHTS.md §3.1 (Interface dependency pattern)
 *            Fix Plan 02-FIX-INTERFACE-DUPLICATION.md
 */
class AsyncSubsystemBusOps : public Async::IFireWireBusOps {
  public:
    explicit AsyncSubsystemBusOps(Async::IAsyncControllerPort& async) : async_(async) {}

    ~AsyncSubsystemBusOps() override = default;

    // Delete copy/move to prevent accidental sharing
    AsyncSubsystemBusOps(const AsyncSubsystemBusOps&) = delete;
    AsyncSubsystemBusOps& operator=(const AsyncSubsystemBusOps&) = delete;
    AsyncSubsystemBusOps(AsyncSubsystemBusOps&&) = delete;
    AsyncSubsystemBusOps& operator=(AsyncSubsystemBusOps&&) = delete;

    // -------------------------------------------------------------------------
    // Async::IFireWireBusOps Implementation
    // -------------------------------------------------------------------------

    Async::AsyncHandle ReadBlock(FW::Generation generation, FW::NodeId nodeId, FWAddress address,
                                 uint32_t length, FW::FwSpeed speed,
                                 Async::InterfaceCompletionCallback callback) override {
        if (!HasCurrentGeneration(generation, callback)) {
            return Async::AsyncHandle{0};
        }

        Async::ReadParams params{};
        params.destinationID = nodeId.value;
        params.addressHigh = address.addressHi;
        params.addressLow = address.addressLo;
        params.length = length;
        params.speedCode = static_cast<uint8_t>(speed);

        return async_.Read(params, AdaptCompletion(std::move(callback)));
    }

    Async::AsyncHandle WriteBlock(FW::Generation generation, FW::NodeId nodeId, FWAddress address,
                                  std::span<const uint8_t> data, FW::FwSpeed speed,
                                  Async::InterfaceCompletionCallback callback) override {
        if (!HasCurrentGeneration(generation, callback)) {
            return Async::AsyncHandle{0};
        }

        Async::WriteParams params{};
        params.destinationID = nodeId.value;
        params.addressHigh = address.addressHi;
        params.addressLow = address.addressLo;
        params.payload = data.data();
        params.length = static_cast<uint32_t>(data.size());
        params.speedCode = static_cast<uint8_t>(speed);

        return async_.Write(params, AdaptCompletion(std::move(callback)));
    }

    Async::AsyncHandle Lock(FW::Generation generation, FW::NodeId nodeId, FWAddress address,
                            FW::LockOp lockOp, std::span<const uint8_t> operand,
                            uint32_t responseLength, FW::FwSpeed speed,
                            Async::InterfaceCompletionCallback callback) override {
        if (!HasCurrentGeneration(generation, callback)) {
            return Async::AsyncHandle{0};
        }

        Async::LockParams params{};
        params.destinationID = nodeId.value;
        params.addressHigh = address.addressHi;
        params.addressLow = address.addressLo;
        params.operand = operand.data();
        params.operandLength = static_cast<uint32_t>(operand.size());
        params.responseLength = responseLength;
        params.speedCode = static_cast<uint8_t>(speed);

        return async_.Lock(params, static_cast<uint16_t>(lockOp),
                           AdaptCompletion(std::move(callback)));
    }

    bool Cancel(Async::AsyncHandle handle) override { return async_.Cancel(handle); }

  private:
    [[nodiscard]] bool HasCurrentGeneration(FW::Generation generation,
                                            const Async::InterfaceCompletionCallback& callback) {
        const auto busState = async_.GetBusStateSnapshot();
        if (generation == FW::Generation{busState.generation16}) {
            return true;
        }

        async_.PostToWorkloop(^{
          if (callback) {
              callback(Async::AsyncStatus::kStaleGeneration, std::span<const uint8_t>{});
          }
        });
        return false;
    }

    [[nodiscard]] static Async::CompletionCallback
    AdaptCompletion(Async::InterfaceCompletionCallback callback) {
        return [callback = std::move(callback)](Async::AsyncHandle, Async::AsyncStatus status,
                                                uint8_t, std::span<const uint8_t> payload) {
            if (callback) {
                callback(status, payload);
            }
        };
    }

    Async::IAsyncControllerPort& async_;
};

} // namespace ASFW::IRM
