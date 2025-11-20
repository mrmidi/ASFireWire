#pragma once
#include "../../Common/FWCommon.hpp"  // Strong types: Generation, NodeId, FwSpeed, LockOp
#include "../AsyncTypes.hpp"        // AsyncHandle, AsyncStatus, FWAddress
#include <span>
#include <functional>
#include <cstdint>
#include <array>

namespace ASFW::Async {

/**
 * @brief Simplified completion callback (Phase 1 refinement).
 *
 * Callback receives:
 * - status: kSuccess, kTimeout, kBusReset, kShortRead, etc.
 * - payload: Response data (4 bytes for quadlet, N bytes for block, empty on error)
 *
 * Callers use lambda captures for correlation instead of AsyncHandle parameter:
 *   auto handle = bus.ReadBlock(..., [nodeId, this](AsyncStatus s, auto data) {
 *       // nodeId captured for correlation
 *   });
 */
using InterfaceCompletionCallback = std::function<void(
    AsyncStatus status,
    std::span<const uint8_t> responsePayload)>;

/**
 * @brief Pure virtual interface for FireWire async bus operations.
 *
 * Provides block read/write/lock primitives without exposing CRTP command
 * internals, descriptor rings, or transaction tracking.
 *
 * Design Principles:
 * - **Minimal virtual surface**: Only block operations (quadlets are helpers)
 * - **Generation-based validation**: All ops require generation parameter
 * - **Async-only**: No blocking operations (everything uses callbacks)
 * - **Zero-cost abstraction**: Single virtual dispatch << bus latency (~5 cycles vs 1-10 µs)
 *
 * Consumers: ROMReader, ROMScanner, future isoch/PHY subsystems
 *
 * IMPORTANT: ReadCommand automatically selects tCode based on length:
 * - length == 4: tCode 0x4 (READ_QUADLET_REQUEST)
 * - length != 4: tCode 0x5 (READ_BLOCK_REQUEST)
 * This is handled internally by the async engine (ReadCommand.hpp).
 */
class IFireWireBusOps {
public:
    virtual ~IFireWireBusOps() = default;

    // -------------------------------------------------------------------------
    // Core Async Operations (Virtual Interface)
    // -------------------------------------------------------------------------

    /**
     * @brief Read block of data from remote node.
     *
     * @param generation Bus generation for validation (prevents stale reads)
     * @param nodeId Target node (0-63)
     * @param address 48-bit FireWire address
     * @param length Bytes to read (4-2048, must be quadlet-aligned)
     * @param speed Link speed (S100/S200/S400/S800)
     * @param callback Completion handler
     * @return AsyncHandle for cancellation
     *
     * Callback receives:
     * - status: kSuccess, kTimeout, kBusReset, kShortRead, etc.
     * - payload: [length] bytes on success, empty on error
     *
     * Thread Safety: Safe to call from any context (internally gated)
     *
     * Note: Driver automatically fragments into max_packet_size chunks.
     * For length==4, driver uses READ_QUADLET_REQUEST tCode internally.
     */
    virtual AsyncHandle ReadBlock(
        FW::Generation generation,
        FW::NodeId nodeId,
        FWAddress address,
        uint32_t length,
        FW::FwSpeed speed,
        InterfaceCompletionCallback callback) = 0;

    /**
     * @brief Write block of data to remote node.
     *
     * @param generation Bus generation for validation
     * @param nodeId Target node (0-63)
     * @param address 48-bit FireWire address
     * @param data Source buffer (must remain valid until callback invoked)
     * @param speed Link speed
     * @param callback Completion handler
     * @return AsyncHandle for cancellation
     *
     * Callback receives:
     * - status: kSuccess, kTimeout, kBusReset, etc.
     * - payload: Empty span (writes have no response data)
     *
     * Thread Safety: Driver copies [data] to DMA buffer before returning.
     */
    virtual AsyncHandle WriteBlock(
        FW::Generation generation,
        FW::NodeId nodeId,
        FWAddress address,
        std::span<const uint8_t> data,
        FW::FwSpeed speed,
        InterfaceCompletionCallback callback) = 0;

    /**
     * @brief Atomic lock operation (compare-swap, fetch-add, etc.).
     *
     * @param generation Bus generation for validation
     * @param nodeId Target node (0-63)
     * @param address 48-bit FireWire address
     * @param lockOp Lock operation type (CompareSwap, FetchAdd, etc.)
     * @param operand Raw operand bytes transmitted with the request (big-endian)
     * @param responseLength Expected response length in bytes (0 = infer from operand / lockOp)
     * @param speed Link speed
     * @param callback Completion handler
     * @return AsyncHandle for cancellation
     *
     * Operand layout depends on @p lockOp. Examples:
     * - LockOp::kCompareSwap: operand = [compare_value||new_value] (8 bytes for quadlet CAS)
     * - LockOp::kFetchAdd: operand = [delta]
     * - LockOp::kMaskSwap: operand = [mask||data]
     */
    virtual AsyncHandle Lock(
        FW::Generation generation,
        FW::NodeId nodeId,
        FWAddress address,
        FW::LockOp lockOp,
        std::span<const uint8_t> operand,
        uint32_t responseLength,
        FW::FwSpeed speed,
        InterfaceCompletionCallback callback) = 0;

    /**
     * @brief Cancel pending async operation.
     *
     * @param handle Handle from Read/Write/Lock operation
     * @return true if cancelled (callback will be invoked with kAborted status)
     * @return false if already completed or invalid handle
     *
     * Note: Callback is always invoked exactly once (either with result or kAborted).
     */
    virtual bool Cancel(AsyncHandle handle) = 0;

    // -------------------------------------------------------------------------
    // Non-Virtual Helpers (Convenience Wrappers)
    // -------------------------------------------------------------------------

    /**
     * @brief Read 4-byte quadlet (non-virtual helper).
     *
     * Implemented as inline wrapper around ReadBlock(length=4).
     * Driver automatically uses READ_QUADLET_REQUEST tCode (0x4) internally.
     */
    AsyncHandle ReadQuad(
        FW::Generation generation,
        FW::NodeId nodeId,
        FWAddress address,
        FW::FwSpeed speed,
        InterfaceCompletionCallback callback)
    {
        return ReadBlock(generation, nodeId, address, 4, speed, std::move(callback));
    }

    /**
     * @brief Write 4-byte quadlet (non-virtual helper).
     *
     * Implemented as inline wrapper around WriteBlock(length=4).
     */
    AsyncHandle WriteQuad(
        FW::Generation generation,
        FW::NodeId nodeId,
        FWAddress address,
        uint32_t value,
        FW::FwSpeed speed,
        InterfaceCompletionCallback callback)
    {
        std::array<uint8_t, 4> data = {
            static_cast<uint8_t>(value >> 24),
            static_cast<uint8_t>(value >> 16),
            static_cast<uint8_t>(value >> 8),
            static_cast<uint8_t>(value)
        };
        return WriteBlock(generation, nodeId, address, std::span{data}, speed, std::move(callback));
    }
};

} // namespace ASFW::Async
