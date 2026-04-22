// SBP-2 Normal Command ORB implementation.
// Ported from Apple IOFireWireSBP2ORB.
// Ref: SBP-2 §5.1.1 (Normal Command ORB format)

#include "SBP2CommandORB.hpp"
#include "../../Common/FWCommon.hpp"

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
    DeallocateResources();
}

// ---------------------------------------------------------------------------
// Resource allocation
// ---------------------------------------------------------------------------

bool SBP2CommandORB::AllocateResources() noexcept {
    const uint32_t orbSize = Wire::NormalORB::kHeaderSize + maxCommandBlockSize_;

    orbStorage_.resize(orbSize, 0);

    const kern_return_t kr = addrMgr_.AllocateAddressRange(
        owner_, 0xFFFF, 0, orbSize,
        &orbHandle_, &orbMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2CommandORB: failed to allocate ORB address space: 0x%08x", kr);
        return false;
    }

    ASFW_LOG(SBP2, "SBP2CommandORB: allocated %u-byte ORB at %04x:%08x",
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

void SBP2CommandORB::SetCommandBlock(std::span<const uint8_t> cdb) noexcept {
    const uint32_t copyLen = static_cast<uint32_t>(
        std::min(cdb.size(), static_cast<size_t>(maxCommandBlockSize_)));

    if (copyLen > 0) {
        std::memcpy(orbStorage_.data() + Wire::NormalORB::kHeaderSize,
                     cdb.data(), copyLen);
    }

    // Zero remaining command block area
    if (copyLen < maxCommandBlockSize_) {
        std::memset(orbStorage_.data() + Wire::NormalORB::kHeaderSize + copyLen,
                     0, maxCommandBlockSize_ - copyLen);
    }
}

// ---------------------------------------------------------------------------
// Prepare for execution (fills in dynamic fields)
// ---------------------------------------------------------------------------

void SBP2CommandORB::PrepareForExecution(uint16_t localNodeID,
                                         FW::FwSpeed speed,
                                         uint16_t maxPayloadLog) noexcept {
    auto* orb = reinterpret_cast<Wire::NormalORB*>(orbStorage_.data());

    // Null next-ORB pointer (bit 31 set = null terminator)
    orb->nextORBAddressHi = Wire::ToBE32(Wire::NormalORB::kNextORBNull);
    orb->nextORBAddressLo = 0;

    // Data descriptor: fill in localNodeID in the hi word
    if (dataDescriptor_.isDirect) {
        // Direct mode: dataDescriptorLo already has the address,
        // just need to set nodeID in hi word
        orb->dataDescriptorHi = Wire::ToBE32(
            static_cast<uint32_t>(localNodeID) << 16);
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
        options |= Wire::ToBE16(0x6000);
    } else if (flags_ & kVendorORB) {
        options |= Wire::ToBE16(0x4000);
    } else if (flags_ & kReservedORB) {
        options |= Wire::ToBE16(0x2000);
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
    options |= Wire::ToBE16(
        static_cast<uint16_t>(maxPayloadLog << Wire::Options::kMaxPayloadShift));

    // Page table format bits from data descriptor
    options |= dataDescriptor_.options;

    orb->options = options;

    // Data size: byte count (direct) or PTE count (page table)
    orb->dataSize = dataDescriptor_.dataSize;

    // Flush ORB to address space
    WriteORBToAddressSpace();
}

// ---------------------------------------------------------------------------
// Write ORB buffer to DMA-backed address space
// ---------------------------------------------------------------------------

void SBP2CommandORB::WriteORBToAddressSpace() noexcept {
    const auto span = std::span<const uint8_t>(orbStorage_.data(), orbStorage_.size());
    const kern_return_t kr = addrMgr_.WriteLocalData(
        owner_, orbHandle_, 0, span);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2CommandORB: failed to write ORB to address space: 0x%08x", kr);
    }
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

void SBP2CommandORB::SetNextORBAddress(uint32_t hi, uint32_t lo) noexcept {
    auto* orb = reinterpret_cast<Wire::NormalORB*>(orbStorage_.data());
    orb->nextORBAddressHi = hi;
    orb->nextORBAddressLo = lo;
    WriteORBToAddressSpace();
}

void SBP2CommandORB::SetToDummy() noexcept {
    // Set rq_fmt=3 (bits [13:12] = 11) to make device skip this ORB
    auto* orb = reinterpret_cast<Wire::NormalORB*>(orbStorage_.data());
    uint16_t hostOptions = Wire::FromBE16(orb->options);
    hostOptions = (hostOptions & ~0x3000u) | 0x6000u;
    orb->options = Wire::ToBE16(hostOptions);
    WriteORBToAddressSpace();
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
    inProgress_ = true;

    // Capture values by copy for block safety
    const uint32_t timeout = timeoutDuration_;
    auto cb = completionCallback_;

#ifndef ASFW_HOST_TEST
    queue->DispatchAsync(^{
        IOSleep(timeout);
        if (inProgress_ && completionCallback_) {
            ASFW_LOG(SBP2, "SBP2CommandORB: ORB timeout after %u ms", timeout);
            inProgress_ = false;
            completionCallback_(-1);
        }
    });
#endif
}

void SBP2CommandORB::CancelTimer() noexcept {
    inProgress_ = false;
    timerQueue_ = nullptr;
}

} // namespace ASFW::Protocols::SBP2
