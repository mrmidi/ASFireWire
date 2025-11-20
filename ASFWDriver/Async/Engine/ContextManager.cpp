#include "ContextManager.hpp"

#include "../../Hardware/OHCIEventCodes.hpp"
#include "../../Hardware/OHCIDescriptors.hpp"

#include "../../Shared/Memory/DMAMemoryManager.hpp"
#include "../../Shared/Rings/DescriptorRing.hpp"
#include "../../Shared/Rings/BufferRing.hpp"

#include "../Contexts/ATRequestContext.hpp"
#include "../Contexts/ATResponseContext.hpp"
#include "../Contexts/ARRequestContext.hpp"
#include "../Contexts/ARResponseContext.hpp"
#include "../Contexts/ContextBase.hpp"  // For ATRequestTag, ATResponseTag

#include "../Track/CompletionQueue.hpp"
#include "../Tx/DescriptorBuilder.hpp"
#include "ATManager.hpp"  // New FSM-based AT manager

#include "../../Hardware/HardwareInterface.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOReturn.h>
#ifndef ASFW_HOST_TEST
  #include <DriverKit/IOLib.h>
#endif

#include <cstdint>
#include <memory>
#include <atomic>
#include <span>
#include <expected>

namespace ASFW::Async::Engine {

template<class T>
using Ex     = std::expected<T, kern_return_t>;
using ExVoid = std::expected<void, kern_return_t>;

// ============================================================================
// State (PIMPL)
// ============================================================================
struct ContextManager::State {
    DMAMemoryManager dmaManager{};

    std::span<HW::OHCIDescriptor> atReqDesc{};
    std::span<HW::OHCIDescriptor> atRspDesc{};
    std::span<HW::OHCIDescriptor> arReqDesc{};
    std::span<HW::OHCIDescriptor> arRspDesc{};
    std::span<uint8_t>            arReqBuf{};
    std::span<uint8_t>            arRspBuf{};

    DescriptorRing atReqRing{};
    DescriptorRing atRspRing{};
    BufferRing     arReqRing{};
    BufferRing     arRspRing{};

    ATRequestContext  atReqCtx{};
    ATResponseContext atRspCtx{};
    ARRequestContext  arReqCtx{};
    ARResponseContext arRspCtx{};

    // New FSM-based AT managers (replace old manual state tracking)
    std::unique_ptr<DescriptorBuilder> descriptorBuilder{nullptr};
    std::unique_ptr<ATManager<ATRequestContext, DescriptorRing, ASFW::Async::ATRequestTag>> atReqMgr{nullptr};
    std::unique_ptr<ATManager<ATResponseContext, DescriptorRing, ASFW::Async::ATResponseTag>> atRspMgr{nullptr};

    CompletionQueue*                completion{nullptr};
    ASFW::Driver::HardwareInterface* hw{nullptr};
    ASFW::Async::PayloadRegistry*   payloads{nullptr};

    bool provisioned{false};

    State() = default;
    ~State() = default;

    State(const State&)            = delete;
    State& operator=(const State&) = delete;
};

// ============================================================================
// Lifecycle
// ============================================================================
ContextManager::ContextManager() noexcept = default;

ContextManager::~ContextManager() {
    if (state_ && state_->provisioned) teardown(true);
}

// ============================================================================
// Provision
// ============================================================================
kern_return_t ContextManager::provision(ASFW::Driver::HardwareInterface& hw,
                                        const ProvisionSpec& spec) noexcept {
    if (state_) {
        ASFW_LOG(Async, "ContextManager::provision - already provisioned");
        return kIOReturnExclusiveAccess;
    }

    state_     = std::make_unique<State>();
    state_->hw = &hw;

    ASFW_LOG(Async, "ContextManager::provision - DMA slab (atReq=%zu, atRsp=%zu, arReq=%zu/%zu, arRsp=%zu/%zu)",
             spec.atReqDescCount, spec.atRespDescCount,
             spec.arReqBufCount, spec.arReqBufSize,
             spec.arRespBufCount, spec.arRespBufSize);

    // Validate counts and sizes before calculating totalSize
    const bool badCounts =
        spec.atReqDescCount  == 0 ||  // need at least 1 descriptor (no sentinel)
        spec.atRespDescCount == 0 ||
        spec.arReqBufCount   == 0 ||
        spec.arRespBufCount  == 0;

    const bool badSizes =
        spec.arReqBufSize == 0 ||
        spec.arRespBufSize == 0;

    if (badCounts || badSizes) {
        ASFW_LOG_ERROR(Async,
            "ContextManager::provision: bad spec "
            "(atReq=%zu, atRsp=%zu, arReq=%zu/%zu, arRsp=%zu/%zu)",
            spec.atReqDescCount, spec.atRespDescCount,
            spec.arReqBufCount,  spec.arReqBufSize,
            spec.arRespBufCount, spec.arRespBufSize);
        return kIOReturnBadArgument;
    }

    // Align-up each region to 16B before summing to cover per-region padding
    auto align16 = [](size_t v) { return (v + 15) & ~size_t(15); };

    const size_t atReqBytes     = align16(spec.atReqDescCount  * sizeof(HW::OHCIDescriptor));
    const size_t atRspBytes     = align16(spec.atRespDescCount * sizeof(HW::OHCIDescriptor));
    const size_t arReqBytes     = align16(spec.arReqBufCount   * sizeof(HW::OHCIDescriptor));
    const size_t arRspBytes     = align16(spec.arRespBufCount  * sizeof(HW::OHCIDescriptor));
    const size_t arReqDataBytes = align16(spec.arReqBufCount   * spec.arReqBufSize);
    const size_t arRspDataBytes = align16(spec.arRespBufCount  * spec.arRespBufSize);

    const size_t totalSize = atReqBytes + atRspBytes + arReqBytes + arRspBytes
                           + arReqDataBytes + arRspDataBytes;

    if (totalSize == 0) {
        ASFW_LOG_ERROR(Async, "ContextManager::provision: totalSize computed as 0 – refusing to init DMA slab");
        return kIOReturnBadArgument;
    }

    // Detailed logging to diagnose allocation size discrepancies
    ASFW_LOG(Async,
        "ContextManager::provision: totalSize=0x%zx (%zu) (atReq=0x%zx/%zu, atRsp=0x%zx/%zu, arReqDesc=0x%zx/%zu, arRspDesc=0x%zx/%zu, arReqBuf=0x%zx/%zu, arRspBuf=0x%zx/%zu)",
        totalSize, totalSize,
        atReqBytes, atReqBytes,
        atRspBytes, atRspBytes,
        arReqBytes, arReqBytes,
        arRspBytes, arRspBytes,
        arReqDataBytes, arReqDataBytes,
        arRspDataBytes, arRspDataBytes);

    auto doProvision = [&]() -> ExVoid {

        if (!state_->dmaManager.Initialize(hw, totalSize)) {
            return std::unexpected(kIOReturnNoMemory);
        }

        auto atReqRegion    = state_->dmaManager.AllocateRegion(atReqBytes);
        auto atRspRegion    = state_->dmaManager.AllocateRegion(atRspBytes);
        auto arReqRegion    = state_->dmaManager.AllocateRegion(arReqBytes);
        auto arRspRegion    = state_->dmaManager.AllocateRegion(arRspBytes);
        auto arReqBufRegion = state_->dmaManager.AllocateRegion(arReqDataBytes);
        auto arRspBufRegion = state_->dmaManager.AllocateRegion(arRspDataBytes);

        if (!atReqRegion || !atRspRegion || !arReqRegion || !arRspRegion ||
            !arReqBufRegion || !arRspBufRegion) {
            return std::unexpected(kIOReturnNoMemory);
        }

        state_->atReqDesc = { reinterpret_cast<HW::OHCIDescriptor*>(atReqRegion->virtualBase),
                              spec.atReqDescCount };
        state_->atRspDesc = { reinterpret_cast<HW::OHCIDescriptor*>(atRspRegion->virtualBase),
                              spec.atRespDescCount };
        state_->arReqDesc = { reinterpret_cast<HW::OHCIDescriptor*>(arReqRegion->virtualBase),
                              spec.arReqBufCount };
        state_->arRspDesc = { reinterpret_cast<HW::OHCIDescriptor*>(arRspRegion->virtualBase),
                              spec.arRespBufCount };
        state_->arReqBuf  = { reinterpret_cast<uint8_t*>(arReqBufRegion->virtualBase),
                              arReqDataBytes };
        state_->arRspBuf  = { reinterpret_cast<uint8_t*>(arRspBufRegion->virtualBase),
                              arRspDataBytes };

        if (!state_->atReqRing.Initialize(state_->atReqDesc))
            return std::unexpected(kIOReturnBadArgument);
        if (!state_->atRspRing.Initialize(state_->atRspDesc))
            return std::unexpected(kIOReturnBadArgument);
        // Finalize AT rings with their device bases so the ring can form CommandPtr words
        if (!state_->atReqRing.Finalize(atReqRegion->deviceBase))
            return std::unexpected(kIOReturnInternalError);
        if (!state_->atRspRing.Finalize(atRspRegion->deviceBase))
            return std::unexpected(kIOReturnInternalError);

        if (!state_->arReqRing.Initialize(state_->arReqDesc, state_->arReqBuf,
                                          spec.arReqBufCount, spec.arReqBufSize))
            return std::unexpected(kIOReturnBadArgument);
        if (!state_->arRspRing.Initialize(state_->arRspDesc, state_->arRspBuf,
                                          spec.arRespBufCount, spec.arRespBufSize))
            return std::unexpected(kIOReturnBadArgument);

        // FIX: pass AR descriptor phys to AR rings (not AT)
        if (!state_->arReqRing.Finalize(arReqRegion->deviceBase,  arReqBufRegion->deviceBase))
            return std::unexpected(kIOReturnInternalError);
        if (!state_->arRspRing.Finalize(arRspRegion->deviceBase,  arRspBufRegion->deviceBase))
            return std::unexpected(kIOReturnInternalError);

        // Bind DMA manager to AR rings and publish all descriptors before arming
        state_->arReqRing.BindDma(&state_->dmaManager);
        state_->arRspRing.BindDma(&state_->dmaManager);
        state_->arReqRing.PublishAllDescriptorsOnce();
        state_->arRspRing.PublishAllDescriptorsOnce();

        // Final validation: verify all regions fit within the slab
        // If cursor exceeds slabSize, we'll silently overrun and corrupt memory
        const size_t slabTotal = state_->dmaManager.TotalSize();
        const size_t remainingBytes = state_->dmaManager.AvailableSize();
        const size_t usedBytes = slabTotal - remainingBytes;
        
        ASFW_LOG(Async,
            "ContextManager::provision: DMA allocation complete - used=%zu expected=%zu slab=%zu remaining=%zu",
            usedBytes, totalSize, slabTotal, remainingBytes);
        
        // Verify we used exactly what we expected (accounting for possible alignment padding in Initialize)
        if (usedBytes > slabTotal) {
            ASFW_LOG_ERROR(Async,
                "ContextManager::provision: DMA slab overflow detected - used=%zu > slab=%zu",
                usedBytes, slabTotal);
            // This should never happen - AllocateRegion checks bounds
        } else if (usedBytes > totalSize) {
            // This is OK - Initialize() may have aligned totalSize up
            ASFW_LOG(Async,
                "ContextManager::provision: DMA slab used more than expected - used=%zu expected=%zu (slab=%zu)",
                usedBytes, totalSize, slabTotal);
        } else if (usedBytes < totalSize && remainingBytes < (totalSize - usedBytes)) {
            // Under-allocation: we expected to use totalSize but used less, and remaining < difference
            ASFW_LOG_ERROR(Async,
                "ContextManager::provision: DMA slab under-allocation detected - used=%zu expected=%zu remaining=%zu",
                usedBytes, totalSize, remainingBytes);
        }

        // Contexts
        kern_return_t kr = state_->atReqCtx.Initialize(hw, state_->atReqRing, state_->dmaManager);
        if (kr != kIOReturnSuccess) return std::unexpected(kr);

        kr = state_->atRspCtx.Initialize(hw, state_->atRspRing, state_->dmaManager);
        if (kr != kIOReturnSuccess) return std::unexpected(kr);

        kr = state_->arReqCtx.Initialize(hw, state_->arReqRing);
        if (kr != kIOReturnSuccess) return std::unexpected(kr);

        kr = state_->arRspCtx.Initialize(hw, state_->arRspRing);
        if (kr != kIOReturnSuccess) return std::unexpected(kr);

        // Initialize DescriptorBuilder for AT chain building
        state_->descriptorBuilder = std::make_unique<DescriptorBuilder>(
            state_->atReqRing, state_->dmaManager);

        // Initialize FSM-based AT managers (replaces old manual state tracking)
        state_->atReqMgr = std::make_unique<ATManager<ATRequestContext, DescriptorRing, ASFW::Async::ATRequestTag>>(
            state_->atReqCtx, state_->atReqRing, *state_->descriptorBuilder);

        state_->atRspMgr = std::make_unique<ATManager<ATResponseContext, DescriptorRing, ASFW::Async::ATResponseTag>>(
            state_->atRspCtx, state_->atRspRing, *state_->descriptorBuilder);

        ASFW_LOG(Async, "ContextManager::provision - ATManager instances created");

        state_->provisioned = true;
        return {};
    }();

    if (!doProvision) { state_.reset(); return doProvision.error(); }

    ASFW_LOG(Async, "ContextManager::provision - SUCCESS");
    return kIOReturnSuccess;
}

// ============================================================================
// Teardown
// ============================================================================
void ContextManager::teardown(bool disable_hw) noexcept {
    if (!state_ || !state_->provisioned) return;

    ASFW_LOG(Async, "ContextManager::teardown - cleaning up");

    if (disable_hw && state_->hw) {
        auto s1 = state_->atReqCtx.Stop(); if (s1 != kIOReturnSuccess) ASFW_LOG(Async, "AT req stop: 0x%x", s1);
        auto s2 = state_->atRspCtx.Stop(); if (s2 != kIOReturnSuccess) ASFW_LOG(Async, "AT rsp stop: 0x%x", s2);
        auto s3 = state_->arReqCtx.Stop(); if (s3 != kIOReturnSuccess) ASFW_LOG(Async, "AR req stop: 0x%x", s3);
        auto s4 = state_->arRspCtx.Stop(); if (s4 != kIOReturnSuccess) ASFW_LOG(Async, "AR rsp stop: 0x%x", s4);
    }

    // Deterministic unmap of DMA slab before dropping State.
    state_->dmaManager.Reset();

    // Clear spans/rings for hygiene
    state_->atReqDesc = {};
    state_->atRspDesc = {};
    state_->arReqDesc = {};
    state_->arRspDesc = {};
    state_->arReqBuf  = {};
    state_->arRspBuf  = {};

    state_->completion  = nullptr;
    state_->provisioned = false;
    state_->hw          = nullptr;

    ASFW_LOG(Async, "ContextManager::teardown - complete");
    state_.reset(); // allow re-provision on same instance
}

// ============================================================================
// ARM AR — circular buffer mode (Z=1)
// ============================================================================
kern_return_t ContextManager::armAR() noexcept {
    if (!state_ || !state_->provisioned) return kIOReturnNotReady;

    ASFW_LOG(Async, "ContextManager::armAR - starting AR contexts");

    const uint32_t reqCmd = state_->arReqRing.CommandPtrWord();
    kern_return_t  kr     = state_->arReqCtx.Arm(reqCmd);
    if (kr != kIOReturnSuccess) return kr;

    const uint32_t rspCmd = state_->arRspRing.CommandPtrWord();
    kr = state_->arRspCtx.Arm(rspCmd);
    if (kr != kIOReturnSuccess) return kr;

    ASFW_LOG(Async, "ContextManager::armAR - SUCCESS");
    return kIOReturnSuccess;
}

// ============================================================================
// STOP AT — used during bus reset
// ============================================================================
kern_return_t ContextManager::stopAT() noexcept {
    if (!state_ || !state_->provisioned) return kIOReturnNotReady;

    ASFW_LOG(Async, "ContextManager::stopAT - stopping AT contexts");

    kern_return_t kr = state_->atReqCtx.Stop();
    if (kr != kIOReturnSuccess) return kr;

    kr = state_->atRspCtx.Stop();
    if (kr != kIOReturnSuccess) return kr;

    ASFW_LOG(Async, "ContextManager::stopAT - SUCCESS");
    return kIOReturnSuccess;
}

// ============================================================================
// STOP AR — for shutdown/recovery
// ============================================================================
kern_return_t ContextManager::stopAR() noexcept {
    if (!state_ || !state_->provisioned) return kIOReturnNotReady;

    ASFW_LOG(Async, "ContextManager::stopAR - stopping AR contexts");

    kern_return_t kr = state_->arReqCtx.Stop();
    if (kr != kIOReturnSuccess) return kr;

    kr = state_->arRspCtx.Stop();
    if (kr != kIOReturnSuccess) return kr;

    ASFW_LOG(Async, "ContextManager::stopAR - SUCCESS");
    return kIOReturnSuccess;
}

// ============================================================================
// FLUSH AT — contexts own their queues
// ============================================================================
void ContextManager::flushAT() noexcept {
    if (!state_ || !state_->provisioned) return;
    ASFW_LOG(Async, "ContextManager::flushAT - completions drained by context scanning");
}

// ============================================================================
// Lightweight accessors
// ============================================================================
DescriptorRing* ContextManager::AtRequestRing() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    return &state_->atReqRing;
}

DescriptorRing* ContextManager::AtResponseRing() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    return &state_->atRspRing;
}

BufferRing* ContextManager::ArRequestRing() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    return &state_->arReqRing;
}

BufferRing* ContextManager::ArResponseRing() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    return &state_->arRspRing;
}

ASFW::Shared::DMAMemoryManager* ContextManager::DmaManager() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    // State currently stores a DMAMemoryManager instance; return its address.
    return &state_->dmaManager;
}

ASFW::Async::ATRequestContext* ContextManager::GetAtRequestContext() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    return &state_->atReqCtx;
}

ASFW::Async::ATResponseContext* ContextManager::GetAtResponseContext() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    return &state_->atRspCtx;
}

ASFW::Async::ARRequestContext* ContextManager::GetArRequestContext() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    return &state_->arReqCtx;
}

ASFW::Async::ARResponseContext* ContextManager::GetArResponseContext() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    return &state_->arRspCtx;
}

// ============================================================================
// Snapshot
// ============================================================================
ContextManager::Snapshot ContextManager::snapshot() const noexcept {
    ContextManagerSnapshot snap{};
    if (!state_ || !state_->provisioned) return snap;

    snap.contextState = 0x00000001; // placeholder
    snap.magic        = 0x12345678;
    snap.crc32        = snap.CalculateCRC32();
    return snap;
}

// Payload registry wiring
void ContextManager::SetPayloads(ASFW::Async::PayloadRegistry* p) noexcept {
    if (!state_) return;
    state_->payloads = p;
}

ASFW::Async::PayloadRegistry* ContextManager::Payloads() noexcept {
    if (!state_) return nullptr;
    return state_->payloads;
}

// ============================================================================
// Snapshot
// ============================================================================
uint32_t ContextManagerSnapshot::CalculateCRC32() const noexcept {
    uint32_t crc = magic ^ contextState ^ atReqRingHead ^ atReqRingTail
                 ^ atRspRingHead ^ atRspRingTail ^ outstandingCount;
    return crc;
}

// ============================================================================
// ATManager accessors (new FSM-based API)
// ============================================================================
ATManager<ATRequestContext, DescriptorRing, ASFW::Async::ATRequestTag>* ContextManager::GetATRequestManager() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    return state_->atReqMgr.get();
}

ATManager<ATResponseContext, DescriptorRing, ASFW::Async::ATResponseTag>* ContextManager::GetATResponseManager() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    return state_->atRspMgr.get();
}

DescriptorBuilder* ContextManager::GetDescriptorBuilder() noexcept {
    if (!state_ || !state_->provisioned) return nullptr;
    return state_->descriptorBuilder.get();
}

} // namespace ASFW::Async::Engine
