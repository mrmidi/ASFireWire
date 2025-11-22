#include "Submitter.hpp"

#include "../Engine/ContextManager.hpp"
#include "../Engine/ATManager.hpp"
#include "../AsyncTypes.hpp"  // For AsyncCmdOptions
#include "DescriptorBuilder.hpp"
#include "../Contexts/ATRequestContext.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"

namespace ASFW::Async::Tx {

// Phase 2.0: OutstandingTable removed (not used)
// Phase 1.2: submitLock_ removed - locking now handled by ATManager
Submitter::Submitter(Engine::ContextManager& ctxMgr, DescriptorBuilder& builder) noexcept
    : ctxMgr_(ctxMgr), descriptorBuilder_(builder) {
    // No lock allocation needed - ATManager has its own lock with fine-grained locking
}

// ============================================================================
// FSM-based submission via ATManager
// ============================================================================
SubmitResult Submitter::submit_tx_chain(ATRequestContext* ctx, DescriptorBuilder::DescriptorChain&& chain) noexcept {
    SubmitResult res{};
    if (!ctx) {
        res.kr = kIOReturnNotReady;
        return res;
    }

    if (chain.Empty()) {
        res.kr = kIOReturnBadArgument;
        return res;
    }

    // Get ATManager from ContextManager
    auto* atMgr = ctxMgr_.GetATRequestManager();
    if (!atMgr) {
        ASFW_LOG_ERROR(Async, "Submitter: ATManager not available");
        res.kr = kIOReturnNotReady;
        return res;
    }

    // Prepare command options for ATManager
    // TODO: Pass needsFlush from caller when AsyncSubsystem integration is complete
    AsyncCmdOptions opts{};
    opts.needsFlush = false;  // Override default: keep context running (simple operations)
    opts.timeoutMs = 200;      // 200ms default timeout

    const uint32_t txid = chain.txid;
    const uint8_t totalBlocks = chain.TotalBlocks();

    // Submit via ATManager (handles PATH 1/PATH 2 decision, WAKE guardrails, fallback)
    const kern_return_t kr = atMgr->Submit(std::move(chain), opts);

    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Async, "ATManager::Submit failed for txid=%u: kr=0x%x", txid, kr);
        res.kr = kr;
        return res;
    }

    ASFW_LOG_V2(Async, "✓ ATManager::Submit succeeded for txid=%u (blocks=%u)", txid, totalBlocks);
    res.kr = kIOReturnSuccess;
    res.desc_count = totalBlocks;
    res.armed_path = true;
    return res;
}

SubmitResult Submitter::submit_tx_chain(ATResponseContext* ctx, DescriptorBuilder::DescriptorChain&& chain) noexcept {
    // AT Response path maps to same AT Request behavior for submission semantics
    // Cast to ATRequestContext if possible via ContextManager usage; fallback to thin behavior
    // For simplicity, treat as request path using the same ring
    // Note: ATResponseContext also inherits ContextBase and provides WriteControlSet/WriteCommandPtr
    // We'll reuse AtRequestRing as the ring for both transmit paths if response ring is not explicit.
    // If the Engine provides separate ring accessors for AT Response, Submitter can be extended.
    (void)ctx; // not used explicitly here
    // Delegating to ATRequest variant by asking ContextManager for the AT Request ring
    DescriptorBuilder::DescriptorChain movedChain = std::move(chain);
    ATRequestContext* reqCtx = ctxMgr_.GetAtRequestContext();
    if (!reqCtx) {
        SubmitResult res; res.kr = kIOReturnNotReady; return res;
    }
    return submit_tx_chain(reqCtx, std::move(movedChain));
}

} // namespace ASFW::Async::Tx
