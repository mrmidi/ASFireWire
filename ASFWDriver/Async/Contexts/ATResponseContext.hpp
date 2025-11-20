#pragma once

#include "ATContextBase.hpp"
#include "ContextBase.hpp"

namespace ASFW::Async {

/**
 * \brief Concrete AT (Asynchronous Transmit) Response context.
 *
 * Handles asynchronous response packet transmission (replies to received requests).
 * Inherits all functionality from ATContextBase via CRTP pattern.
 *
 * **OHCI Specification References**
 * - §7.2: Asynchronous Transmit DMA (AT Response context)
 * - §7.2.3: AsRspTrContextControlSet register (0x1A0)
 * - §7.2.3: AsRspTrContextControlClear register (0x1A4)
 * - §7.2.4: AsRspTrCommandPtr register (0x1AC)
 *
 * **Apple Pattern**
 * Equivalent to AppleFWOHCI_AsyncTransmitResponse context in IOFireWireFamily.
 * Handles:
 * - Read response packets (with payload data)
 * - Write response packets (ack-only, no payload)
 * - Lock response packets (old value return)
 *
 * **Usage**
 * \code
 * ATResponseContext rspCtx;
 * rspCtx.Initialize(hw, responseRing);
 * rspCtx.Arm(firstDescriptorPhys);
 *
 * // In request completion handler
 * auto chain = builder.BuildResponseChain(...);
 * rspCtx.SubmitChain(std::move(chain));
 *
 * // In interrupt handler or timer callback
 * while (auto completion = rspCtx.ScanCompletion()) {
 *     // Response sent, can free resources
 *     ProcessResponseCompletion(completion->eventCode);
 * }
 * \endcode
 *
 * **Design Rationale**
 * Minimal class definition - all logic resides in ATContextBase template.
 * RoleTag typedef enables CRTP deduction in ContextBase.
 *
 * \note Response context typically has smaller ring than Request context,
 *       as responses are generated synchronously from received requests
 *       (no need for large outbound queue).
 */
class ATResponseContext final : public ATContextBase<ATResponseContext, ATResponseTag> {
public:
    /// Context role tag for ContextBase CRTP deduction
    using RoleTag = ATResponseTag;

    ATResponseContext() = default;
    ~ATResponseContext() = default;

    ATResponseContext(const ATResponseContext&) = delete;
    ATResponseContext& operator=(const ATResponseContext&) = delete;
};

// Compile-time validation: Ensure RoleTag satisfies ContextRole concept
static_assert(ContextRole<ATResponseContext::RoleTag>,
              "ATResponseContext::RoleTag must satisfy ContextRole concept");

} // namespace ASFW::Async
