#pragma once

#include "../AsyncTypes.hpp"
#include "../Tx/PayloadContext.hpp"
#include <memory>

namespace ASFW::Async {

// Forward declarations
class AsyncSubsystem;
struct TxMetadata;

/**
 * AsyncCommand - CRTP base class for async transaction commands.
 * 
 * DESIGN: Template-based polymorphism (zero runtime overhead, no vtable).
 * Derived classes implement:
 *   - TxMetadata BuildMetadata(const TransactionContext&)
 *   - size_t BuildHeader(uint8_t label, const PacketContext&, uint8_t* buffer)
 *   - std::unique_ptr<PayloadContext> PreparePayload(Driver::HardwareInterface&)
 * 
 * Submit() calls derived implementations via static_cast<Derived*>(this)->Method().
 * Compiler inlines everything at compile time - zero indirection cost.
 * 
 * LIFETIME: Stack-allocated for immediate submission (Phase 2.3: no void* context):
 *   AsyncHandle h = ReadCommand{params, cb}.Submit(subsys);
 * Heap-allocated for command queue:
 *   auto cmd = std::make_unique<ReadCommand>(params, cb);
 *   commandQueue.push(std::move(cmd));
 * 
 * Reference: Apple's IOFWAsyncCommand uses virtual methods (vtable overhead).
 *            Linux firewire-core uses function pointers (indirect call overhead).
 *            We use CRTP for zero-cost abstraction matching Rust/C++ best practices.
 */
template<typename Derived>
class AsyncCommand {
public:
    /**
     * Submit transaction to hardware via AsyncSubsystem.
     * 
     * Sequence (matches Apple's executeCommandElement @ 0xDBBE):
     * 1. PrepareTransactionContext() - validate bus state, read NodeID, query generation
     * 2. BuildMetadata() - populate TxMetadata (tCode, length, destination)
     * 3. RegisterTx() - allocate slot in OutstandingTable, get handle
     * 4. GetLabelFromHandle() - extract 6-bit transaction label
     * 5. BuildHeader() - construct IEEE 1394 packet header (PacketBuilder delegation)
     * 6. PreparePayload() - allocate DMA buffer for Write/Lock (null for Read/Phy)
     * 7. BuildTransactionChain() - create OHCI descriptor chain
     * 8. Tag descriptor.softwareTag with handle (for completion matching)
     * 9. submit_tx_chain() - program AT context (CommandPtr or branchWord)
     * 10. OnTxPosted() - schedule timeout in TimeoutEngine
     * 11. Attach payload to PayloadRegistry (if non-null)
     * 
     * @param subsys AsyncSubsystem providing hardware/tracking/submission services
     * @return AsyncHandle for tracking/cancellation, or {0} on failure
     */
    AsyncHandle Submit(AsyncSubsystem& subsys);

protected:
    // Constructor for derived classes (Phase 2.3: std::function, no void* context)
    AsyncCommand(CompletionCallback callback)
        : callback_(std::move(callback)) {}

    // Derived classes access callback for completion notification
    // Phase 2.3: Now std::function (type-safe, captures context via lambda)
    CompletionCallback callback_;
};

} // namespace ASFW::Async

// Include implementation (template must be in header)
#include "AsyncCommandImpl.hpp"
