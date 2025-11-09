#pragma once

#include "DmaContextManagerBase.hpp"
#include "ATTrace.hpp"
#include "../AsyncTypes.hpp"
#include "../Tx/DescriptorBuilder.hpp"
#include "../Contexts/ATRequestContext.hpp"
#include "../Contexts/ATResponseContext.hpp"
#include "../Rings/DescriptorRing.hpp"
#include "../../Core/OHCIConstants.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Async::Engine {

/**
 * ATState - AT context state machine enum
 * Explicit states for clarity and diagnostics
 */
enum class ATState : uint8_t {
    IDLE,      ///< Context stopped, may use PATH 1
    ARMING,    ///< PATH 1: CommandPtr write in progress
    RUNNING,   ///< Context active, may use PATH 2
    STOPPING,  ///< Drain in progress
    ERROR      ///< Fatal error, requires reset
};

inline const char* ToString(ATState s) noexcept;

/**
 * ATSubmitPolicy - Policy for AT context submission behavior
 * Centralizes Z-nibble computation, publish span calculations, and state management
 */
struct ATSubmitPolicy {
    using State = ATState;

    static constexpr bool kHasFsm = true;
    static constexpr State kInitialState = State::IDLE;

    static const char* ToStr(State s) noexcept {
        return ToString(s);
    }

    /**
     * Compute Z field for CommandPtr/branchWord
     * @param firstIsImmediate true if first descriptor is 32-byte immediate
     * @return Z value: 0x2 for immediate, 0x0 for standard
     */
    static uint8_t ComputeZ(bool firstIsImmediate) noexcept {
        return firstIsImmediate ? 0x2 : 0x0;
    }

    /**
     * Compute publish span for PATH 2 branch patch
     * @param prevLastBlocks Block count of previous LAST descriptor (1 or 2)
     * @param prevIsImmediate true if previous descriptor is immediate
     * @return Bytes to publish: 64 for immediate, 16 for standard
     */
    static size_t PublishSpanBytes(uint8_t prevLastBlocks, bool prevIsImmediate) noexcept {
        if (prevIsImmediate) {
            return 64;  // Immediate descriptor: 2 blocks × 32 bytes
        }
        return 16;  // Standard descriptor: 1 block × 16 bytes
    }
};

/**
 * ATManager - AT context manager with FSM, PATH 1/2, guarded WAKE, hybrid stop
 * 
 * Manages AT context lifecycle with explicit state machine:
 * - PATH 1: First submission or re-arm after stop (CommandPtr + RUN)
 * - PATH 2: Chaining to running context (branch patch + WAKE)
 * - Hybrid stop: Immediate for needsFlush, AR-side for outstanding==0
 * - WAKE guardrails: Check RUN/DEAD before WAKE, poll ACTIVE, fallback on failure
 * 
 * @tparam ContextT AT context type (ATRequestContext, ATResponseContext)
 * @tparam RingT DescriptorRing type
 * @tparam RoleTag Role tag for context name (ATRequestTag, ATResponseTag)
 */
template<typename ContextT, typename RingT, typename RoleTag>
class ATManager : public DmaContextManagerBase<ContextT, RingT, RoleTag, ATSubmitPolicy> {
public:
    using Base = DmaContextManagerBase<ContextT, RingT, RoleTag, ATSubmitPolicy>;
    using State = ATSubmitPolicy::State;
    using DescriptorChain = DescriptorBuilder::DescriptorChain;

    ATManager(ContextT& ctx, RingT& ring, DescriptorBuilder& builder)
        : Base(ctx, ring), builder_(builder), generation_(0)
    {}

    /**
     * Submit descriptor chain with Apple-mirrored command options
     * @param chain Descriptor chain to submit
     * @param opts Command options (needsFlush, timeout, etc.)
     * @return kIOReturnSuccess or error code
     */
    kern_return_t Submit(DescriptorChain&& chain, const AsyncCmdOptions& opts);

    /**
     * Request stop (idempotent, called from hybrid stop policy or AR-side drain)
     * @param txid Transaction ID for correlation
     * @param why Reason string for logging
     */
    void RequestStop(uint32_t txid, const char* why) noexcept;

    /**
     * Get current bus generation (for correlation)
     */
    uint16_t GetGeneration() const noexcept { return generation_; }

    /**
     * Set bus generation (called on bus reset)
     */
    void SetGeneration(uint16_t gen) noexcept { generation_ = gen; }

    /**
     * Dump trace ring (for panic/ERROR state debugging)
     */
    void DumpTrace() const noexcept { trace_.dump(); }

private:
    // PATH 1: First submission or re-arm after stop
    kern_return_t SubmitPath1_(const DescriptorChain& chain, uint32_t txid, const AsyncCmdOptions& opts);
    
    // PATH 2: Chain to running context (branch patch + WAKE)
    kern_return_t SubmitPath2_(const DescriptorChain& chain, uint32_t txid, const AsyncCmdOptions& opts);

    // Internal stop implementation
    void requestStop_(uint32_t txid, const char* why) noexcept;

    // Helper: Clear RUN and poll for ACTIVE=0
    void clearRunAndPoll_() noexcept;

    // Helper: Rotate ring by fixed stride (+2) to avoid address caching
    void rotateRingBy2_() noexcept;

    // Helper: Publish entire chain
    void PublishChain_(const DescriptorChain& chain);

    // Helper: Update ring tail after successful submission
    void UpdateRingTail_(const DescriptorChain& chain);

    // Helper: Link tail to new chain (PATH 2)
    void LinkTailTo_(const DescriptorChain& chain);

    // Helper: Unlink tail (PATH 2 fallback)
    void UnlinkTail_() noexcept;

    // Helper: Publish previous LAST span (PATH 2)
    void PublishPrevLast_(const DescriptorChain& chain);

    // Descriptor builder for chain operations
    DescriptorBuilder& builder_;

    // Black box trace ring
    ATTraceRing trace_;

    // Bus generation for correlation
    uint16_t generation_;

    // Accessor helpers (avoid repeating this->)
    ContextT& ctx() { return this->ctx_; }
    const ContextT& ctx() const { return this->ctx_; }
    RingT& ring() { return this->ring_; }
    const RingT& ring() const { return this->ring_; }
    IOLock* lock() { return this->lock_; }
};

} // namespace ASFW::Async::Engine

// Include implementation (template must be in header)
#include "ATManagerImpl.hpp"
