#pragma once

#include <atomic>
#include <cstdint>
#include <DriverKit/IOLib.h>
#include "../Core/KR.hpp"
#include "DescriptorBuilder.hpp"
#include "../Track/PayloadRegistry.hpp"

namespace ASFW::Async {
class DescriptorBuilder;
class ATRequestContext;
class ATResponseContext;
class PayloadRegistry;

namespace Engine { class ContextManager; }

namespace Tx {

struct SubmitResult {
    kr_t kr{ kIOReturnSuccess };
    uint32_t desc_count{0};
    bool armed_path{false};
};

class Submitter {
public:
    Submitter(Engine::ContextManager& ctxMgr, DescriptorBuilder& builder) noexcept;
    ~Submitter() = default;  // Phase 1.2: submitLock_ removed (unused, ATManager has own lock)

    // Submit to AT Request context
    // needsFlush: Apple's hybrid pattern (IDA @ 0xDBBE lines 89-129). Retained as metadata
    //   true  = descriptor originated from block/DMA path (requires extra diagnostics)
    //   false = simple quadlet path
    SubmitResult submit_tx_chain(ATRequestContext* ctx, DescriptorBuilder::DescriptorChain&& chain) noexcept;

    // Submit to AT Response context (if needed)
    SubmitResult submit_tx_chain(ATResponseContext* ctx, DescriptorBuilder::DescriptorChain&& chain) noexcept;

    // Called when AT contexts are stopped (bus reset path) to reset internal arm state
    void OnATContextsStopped() noexcept {
        // Context manager now tracks state internally, just cancel payloads.
        // Bus-reset teardown will stop and re-arm contexts as part of reset handling.
        if (payloads_) {
            payloads_->CancelAll(PayloadRegistry::CancelMode::Deferred);
        }
    }

    // Payload registry wiring (non-owning)
    void SetPayloads(ASFW::Async::PayloadRegistry* p) noexcept { payloads_ = p; }

private:
    Engine::ContextManager& ctxMgr_;
    DescriptorBuilder& descriptorBuilder_;
    ASFW::Async::PayloadRegistry* payloads_{nullptr};
    // Phase 1.2: submitLock_ removed - locking now handled by ATManager with fine granularity
};

} // namespace Tx
} // namespace ASFW::Async
