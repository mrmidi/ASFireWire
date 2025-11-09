#pragma once

#include "ATContextBase.hpp"
#include "ContextBase.hpp"

namespace ASFW::Async {

/**
 * \brief Concrete AT (Asynchronous Transmit) Request context.
 *
 * Handles asynchronous request packet transmission (read/write/lock transactions).
 * Inherits all functionality from ATContextBase via CRTP pattern.
 *
 * \par OHCI Specification References
 * - §7.2: Asynchronous Transmit DMA (AT Request context)
 * - §7.2.3: AsReqTrContextControlSet register (0x180)
 * - §7.2.3: AsReqTrContextControlClear register (0x184)
 * - §7.2.4: AsReqTrCommandPtr register (0x18C)
 *
 * \par Apple Pattern
 * Equivalent to AppleFWOHCI_AsyncTransmitRequest context in IOFireWireFamily.
 * Handles:
 * - Quadlet/block read requests
 * - Quadlet/block write requests
 * - Lock (compare-swap) requests
 * - PHY configuration packets
 *
 * \par Usage
 * \code
 * ATRequestContext reqCtx;
 * reqCtx.Initialize(hw, requestRing);
 * reqCtx.Arm(firstDescriptorPhys);
 *
 * // Submit descriptor chain
 * auto chain = builder.BuildTransactionChain(...);
 * reqCtx.SubmitChain(std::move(chain));
 *
 * // In interrupt handler or timer callback
 * while (auto completion = reqCtx.ScanCompletion()) {
 *     ProcessCompletion(completion->eventCode, completion->tLabel);
 * }
 * \endcode
 *
 * \par Design Rationale
 * Minimal class definition - all logic resides in ATContextBase template.
 * RoleTag typedef enables CRTP deduction in ContextBase.
 */
class ATRequestContext final : public ATContextBase<ATRequestContext, ATRequestTag> {
public:
    /// Context role tag for ContextBase CRTP deduction
    using RoleTag = ATRequestTag;

    ATRequestContext() = default;
    ~ATRequestContext() = default;

    ATRequestContext(const ATRequestContext&) = delete;
    ATRequestContext& operator=(const ATRequestContext&) = delete;
};

// Compile-time validation: Ensure RoleTag satisfies ContextRole concept
static_assert(ContextRole<ATRequestContext::RoleTag>,
              "ATRequestContext::RoleTag must satisfy ContextRole concept");

} // namespace ASFW::Async
