// SBP-2 Normal Command ORB implementation.
// Ref: SBP-2 §5.1.1 (Normal Command ORB format)

#include "SBP2CommandORB.hpp"
#include <DriverKit/IOLib.h>
#include "SBP2DelayedDispatch.hpp"
#include "../../Common/FWCommon.hpp"

#include <algorithm>

namespace ASFW::Protocols::SBP2 {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

SBP2CommandORB::SBP2CommandORB(AddressSpaceManager& addrMgr, void* owner,
                               uint32_t maxCommandBlockSize)
    : addrMgr_(addrMgr)
    , owner_(owner)
    , maxCommandBlockSize_(maxCommandBlockSize)
{
    AllocateResources();
}

SBP2CommandORB::~SBP2CommandORB() {
    CancelTimer();
    lifetimeToken_.reset();
    DeallocateResources();
}

// ---------------------------------------------------------------------------
// Resource allocation
// ---------------------------------------------------------------------------

bool SBP2CommandORB::AllocateResources() noexcept {
    const uint32_t orbSize = Wire::NormalORB::kHeaderSize + maxCommandBlockSize_;

    orbStorage_.resize(orbSize, 0);

    const kern_return_t kr = addrMgr_.AllocateAddressRangeAuto(
        owner_, 0xFFFF, orbSize,
        &orbHandle_, &orbMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Async, "SBP2CommandORB: failed to allocate ORB address space: 0x%08x", kr);
        return false;
    }

    ASFW_LOG(Async, "SBP2CommandORB: allocated %u-byte ORB at %04x:%08x",
             orbSize, orbMeta_.addressHi, orbMeta_.addressLo);
    return true;
}

void SBP2CommandORB::DeallocateResources() noexcept {
    if (orbHandle_ != 0) {
        addrMgr_.DeallocateAddressRange(owner_, orbHandle_);
        orbHandle_ = 0;
    }
    orbMeta_ = {};
    orbStorage_.clear();
}

// ---------------------------------------------------------------------------
// Command block (CDB)
// ---------------------------------------------------------------------------

bool SBP2CommandORB::SetCommandBlock(std::span<const uint8_t> cdb) noexcept {
    // Reject (do not silently truncate) a CDB that won't fit the command block.
    if (cdb.size() > static_cast<size_t>(maxCommandBlockSize_)) {
        ASFW_LOG(Async, "SBP2CommandORB: CDB size %zu exceeds max command block %u",
                 cdb.size(), maxCommandBlockSize_);
        return false;
    }

    const uint32_t copyLen = static_cast<uint32_t>(cdb.size());
    if (copyLen > 0) {
        std::memcpy(orbStorage_.data() + Wire::NormalORB::kHeaderSize,
                     cdb.data(), copyLen);
    }

    // Zero remaining command block area
    if (copyLen < maxCommandBlockSize_) {
        std::memset(orbStorage_.data() + Wire::NormalORB::kHeaderSize + copyLen,
                     0, maxCommandBlockSize_ - copyLen);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Prepare for execution (fills in dynamic fields)
// ---------------------------------------------------------------------------

kern_return_t SBP2CommandORB::PrepareForExecution(uint16_t localNodeID,
                                                  FW::FwSpeed speed,
                                                  uint16_t maxPayloadLog) noexcept {
    if (!IsValid()) {
        return kIOReturnNotReady;
    }

    auto* orb = reinterpret_cast<Wire::NormalORB*>(orbStorage_.data());
    const uint16_t busNodeID = Wire::NormalizeBusNodeID(localNodeID);

    // Null next-ORB pointer (bit 31 set = null terminator)
    orb->nextORBAddressHi = OSSwapHostToBigInt32(Wire::NormalORB::kNextORBNull);
    orb->nextORBAddressLo = 0;

    // Data descriptor: fill in localNodeID in the hi word
    if (dataDescriptor_.isDirect) {
        // Direct mode: preserve addressHi and inject local node ID.
        orb->dataDescriptorHi = OSSwapHostToBigInt32(
            Wire::ComposeBusAddressHi(
                busNodeID,
                static_cast<uint16_t>(OSSwapBigToHostInt32(dataDescriptor_.dataDescriptorHi) & 0xFFFFu)));
        orb->dataDescriptorLo = dataDescriptor_.dataDescriptorLo;
    } else {
        // Page table mode: dataDescriptorHi already has nodeID + addressHi from Build()
        orb->dataDescriptorHi = dataDescriptor_.dataDescriptorHi;
        orb->dataDescriptorLo = dataDescriptor_.dataDescriptorLo;
    }

    // Build options word from flags + negotiated parameters
    uint16_t options = 0;

    // ORB format: Normal = 0x0000, Reserved = 0x2000, Vendor = 0x4000, Dummy = 0x6000
    if (flags_ & kDummyORB) {
        options |= OSSwapHostToBigInt16(0x6000);
    } else if (flags_ & kVendorORB) {
        options |= OSSwapHostToBigInt16(0x4000);
    } else if (flags_ & kReservedORB) {
        options |= OSSwapHostToBigInt16(0x2000);
    }
    // kNormalORB (default) → 0x0000, no bits needed

    // Notify bit
    if (flags_ & kNotify) {
        options |= Wire::Options::kNotify;
    }

    // Direction: data from target (read)
    if (flags_ & kDataFromTarget) {
        options |= Wire::Options::kDirectionRead;
    }

    // Speed
    switch (speed) {
        case FW::FwSpeed::S200: options |= Wire::Options::kSpeed200; break;
        case FW::FwSpeed::S400: options |= Wire::Options::kSpeed400; break;
        case FW::FwSpeed::S800: options |= Wire::Options::kSpeed800; break;
        default: break; // S100 = 0
    }

    // Max payload size (log2 of max payload in quadlets, shifted left by 4)
    options |= OSSwapHostToBigInt16(
        static_cast<uint16_t>(maxPayloadLog << Wire::Options::kMaxPayloadShift));

    // Page table format bits from data descriptor
    options |= dataDescriptor_.options;

    orb->options = options;

    // Data size: byte count (direct) or PTE count (page table)
    orb->dataSize = dataDescriptor_.dataSize;

    // Flush ORB to address space
    return WriteORBToAddressSpace();
}

// ---------------------------------------------------------------------------
// Write ORB buffer to DMA-backed address space
// ---------------------------------------------------------------------------

kern_return_t SBP2CommandORB::WriteORBToAddressSpace() noexcept {
    const auto span = std::span<const uint8_t>(orbStorage_.data(), orbStorage_.size());
    const kern_return_t kr = addrMgr_.WriteLocalData(
        owner_, orbHandle_, 0, span);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Async, "SBP2CommandORB: failed to write ORB to address space: 0x%08x", kr);
    }
    return kr;
}

// ---------------------------------------------------------------------------
// ORB address / chaining
// ---------------------------------------------------------------------------

Async::FWAddress SBP2CommandORB::GetORBAddress() const noexcept {
    Async::FWAddress::QualifiedAddressParts parts{};
    parts.addressHi = orbMeta_.addressHi;
    parts.addressLo = orbMeta_.addressLo;
    parts.nodeID = 0; // filled in by login session with localNodeID
    return Async::FWAddress(parts);
}

kern_return_t SBP2CommandORB::SetNextORBAddress(uint32_t hi, uint32_t lo) noexcept {
    if (!IsValid()) {
        return kIOReturnNotReady;
    }
    auto* orb = reinterpret_cast<Wire::NormalORB*>(orbStorage_.data());
    orb->nextORBAddressHi = hi;
    orb->nextORBAddressLo = lo;
    const kern_return_t kr = WriteORBToAddressSpace();
    // Bring-up trace (doorbell chain): what the target will see when it
    // re-reads this ORB's next_ORB field — read back from the address space,
    // not from orbStorage_, to catch publish failures.
    uint32_t rbHi = 0;
    uint32_t rbLo = 0;
    const uint64_t base = (static_cast<uint64_t>(orbMeta_.addressHi) << 32) | orbMeta_.addressLo;
    (void)addrMgr_.ReadQuadlet(base, &rbHi);
    (void)addrMgr_.ReadQuadlet(base + 4, &rbLo);
    ASFW_LOG(Async,
             "SBP2CommandORB: next_ORB linked @ %04x:%08x — wrote %08x:%08x readback %08x:%08x kr=0x%x",
             orbMeta_.addressHi, orbMeta_.addressLo,
             OSSwapBigToHostInt32(hi), OSSwapBigToHostInt32(lo),
             OSSwapBigToHostInt32(rbHi), OSSwapBigToHostInt32(rbLo), kr);
    return kr;
}

kern_return_t SBP2CommandORB::SetToDummy() noexcept {
    if (!IsValid()) {
        return kIOReturnNotReady;
    }
    // Set rq_fmt=3 (bits [13:12] = 11) to make device skip this ORB
    auto* orb = reinterpret_cast<Wire::NormalORB*>(orbStorage_.data());
    uint16_t hostOptions = OSSwapBigToHostInt16(orb->options);
    hostOptions = (hostOptions & ~0x3000u) | 0x6000u;
    orb->options = OSSwapHostToBigInt16(hostOptions);
    return WriteORBToAddressSpace();
}

// ---------------------------------------------------------------------------
// Timer management
// ---------------------------------------------------------------------------

void SBP2CommandORB::StartTimer(IODispatchQueue* queue) noexcept {
    CancelTimer();

    if (queue == nullptr || timeoutDuration_ == 0) {
        return;
    }

    timerQueue_ = queue;
    inProgress_.store(true, std::memory_order_relaxed);
    const uint32_t timeout = timeoutDuration_;
    const uint64_t expectedGeneration =
        timerGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1ULL;
    const std::weak_ptr<int> weakLifetime = lifetimeToken_;
    const uint64_t delayNs = static_cast<uint64_t>(timeout) * 1'000'000ULL;

    DispatchAfterCompat(queue, delayNs, [this, weakLifetime, expectedGeneration, timeout]() {
        if (weakLifetime.expired()) {
            return;
        }
        if (timerGeneration_.load(std::memory_order_acquire) != expectedGeneration ||
            !inProgress_.load(std::memory_order_relaxed) ||
            !completionCallback_) {
            return;
        }

        ASFW_LOG(Async, "SBP2CommandORB: ORB timeout after %u ms", timeout);
        inProgress_.store(false, std::memory_order_relaxed);
        timerGeneration_.fetch_add(1, std::memory_order_acq_rel);
        completionCallback_(-1, Wire::SBPStatus::kUnspecifiedError);
    });
}

void SBP2CommandORB::CancelTimer() noexcept {
    inProgress_.store(false, std::memory_order_relaxed);
    timerQueue_ = nullptr;
    timerGeneration_.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace ASFW::Protocols::SBP2
