#include "Submitter.hpp"

#include "../Engine/ContextManager.hpp"
#include "../Engine/ATManager.hpp"
#include "../AsyncTypes.hpp"  // For AsyncCmdOptions
#include "DescriptorBuilder.hpp"
#include "../Contexts/ATRequestContext.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Hardware/OHCIDescriptors.hpp"
#include "../../Debug/AsyncTraceCapture.hpp"
#include "../../Shared/ASFWDiagnosticsABI.h"
#include <DriverKit/IOLib.h>

namespace ASFW::Async::Tx {

// Phase 2.0: OutstandingTable removed (not used)
// Phase 1.2: submitLock_ removed - locking now handled by ATManager
Submitter::Submitter(Engine::ContextManager& ctxMgr, DescriptorBuilder& builder) noexcept
    : ctxMgr_(ctxMgr), descriptorBuilder_(builder) {
    // No lock allocation needed - ATManager has its own lock with fine-grained locking
}

// ============================================================================
// Trace Outgoing Event Helper
// ============================================================================
namespace {
void TraceOutgoingEvent(Debug::AsyncTraceCapture* traceCapture, uint32_t contextType, const DescriptorBuilder::DescriptorChain& chain) noexcept {
    if (!traceCapture || chain.Empty()) {
        return;
    }
    
    auto* immDesc = reinterpret_cast<const HW::OHCIDescriptorImmediate*>(chain.first);
    if (!immDesc) {
        return;
    }
    
    ASFWDiagAsyncEvent event{};
    
    static mach_timebase_info_data_t timebase{};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    event.timestampNs = (mach_absolute_time() * timebase.numer) / timebase.denom;
    event.generation = 0; // Outgoing context doesn't track gen easily, can default to 0
    event.direction = 1;  // 1 for TX (outgoing)
    event.context = contextType;
    
    const uint32_t q0 = OSSwapLittleToHostInt32(immDesc->immediateData[0]);
    const uint32_t q1 = OSSwapLittleToHostInt32(immDesc->immediateData[1]);
    const uint32_t q2 = OSSwapLittleToHostInt32(immDesc->immediateData[2]);
    
    event.destinationId = static_cast<uint16_t>(q0 >> 16);
    event.tLabel = static_cast<uint8_t>((q0 >> 10) & 0x3F);
    event.tCode = static_cast<uint8_t>((q0 >> 4) & 0x0F);
    event.sourceId = static_cast<uint16_t>(q1 >> 16);
    
    const uint64_t offset_high = q1 & 0xFFFF;
    const uint64_t offset_low = q2;
    
    if (event.tCode == 0x0 || event.tCode == 0x1 || event.tCode == 0x4 || event.tCode == 0x5 || event.tCode == 0x9) {
        event.address = (offset_high << 32) | offset_low;
    }
    
    if (event.tCode == 0x0) {
        event.quadletData = OSSwapLittleToHostInt32(immDesc->immediateData[3]);
    }
    
    if (chain.last && chain.last != chain.first) {
        event.payloadBytes = chain.last->control & 0xFFFF;
    } else {
        event.payloadBytes = 0;
    }
    
    event.ackCode = 0; // Fill when completion is received if possible, else 0
    event.rCode = 0;
    event.speed = (q0 >> 16) & 0x07;
    
    traceCapture->CaptureEvent(event);
}
} // namespace

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

    // Trace outgoing request before moving chain
    TraceOutgoingEvent(traceCapture_, 0, chain);

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
    SubmitResult res{};
    if (!ctx) {
        res.kr = kIOReturnNotReady;
        return res;
    }

    if (chain.Empty()) {
        res.kr = kIOReturnBadArgument;
        return res;
    }

    auto* atMgr = ctxMgr_.GetATResponseManager();
    if (!atMgr) {
        ASFW_LOG_ERROR(Async, "Submitter: ATResponseManager not available");
        res.kr = kIOReturnNotReady;
        return res;
    }

    AsyncCmdOptions opts{};
    opts.needsFlush = false;
    opts.timeoutMs = 200;

    const uint32_t txid = chain.txid;
    const uint8_t totalBlocks = chain.TotalBlocks();

    // Trace outgoing response before moving chain
    TraceOutgoingEvent(traceCapture_, 1, chain);

    const kern_return_t kr = atMgr->Submit(std::move(chain), opts);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Async, "ATResponseManager::Submit failed for txid=%u: kr=0x%x", txid, kr);
        res.kr = kr;
        return res;
    }

    ASFW_LOG_V2(Async, "✓ ATResponseManager::Submit succeeded for txid=%u (blocks=%u)", txid, totalBlocks);
    res.kr = kIOReturnSuccess;
    res.desc_count = totalBlocks;
    res.armed_path = true;
    return res;
}

} // namespace ASFW::Async::Tx
