// SBP-2 Management ORB implementation.
// Ref: SBP-2 §6 (Task Management)

#include "SBP2ManagementORB.hpp"
#include "SBP2DelayedDispatch.hpp"

#include "../../Async/Interfaces/IFireWireBus.hpp"
#include "../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../Common/FWCommon.hpp"

namespace ASFW::Protocols::SBP2 {

using namespace ASFW::Protocols::SBP2::Wire;

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

SBP2ManagementORB::SBP2ManagementORB(Async::IFireWireBus& bus,
                                     Async::IFireWireBusInfo& busInfo,
                                     AddressSpaceManager& addrMgr, void* owner)
    : bus_(bus)
    , busInfo_(busInfo)
    , addrMgr_(addrMgr)
    , owner_(owner) {}

SBP2ManagementORB::~SBP2ManagementORB() {
    timerGeneration_.fetch_add(1, std::memory_order_acq_rel);
    lifetimeToken_.reset();
    DeallocateResources();
}

// ---------------------------------------------------------------------------
// Resource allocation
// ---------------------------------------------------------------------------

bool SBP2ManagementORB::AllocateResources() noexcept {
    if (orbHandle_ != 0) {
        return true; // Already allocated
    }

    // Allocate ORB address space (32 bytes)
    auto kr = addrMgr_.AllocateAddressRangeAuto(
        owner_, 0xFFFF, Wire::TaskManagementORB::kSize,
        &orbHandle_, &orbMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Async, "SBP2ManagementORB: failed to allocate ORB: 0x%08x", kr);
        return false;
    }

    // Allocate per-ORB status block address space (32 bytes)
    kr = addrMgr_.AllocateAddressRangeAuto(
        owner_, 0xFFFF, Wire::StatusBlock::kMaxSize,
        &statusBlockHandle_, &statusBlockMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Async, "SBP2ManagementORB: failed to allocate status block: 0x%08x", kr);
        addrMgr_.DeallocateAddressRange(owner_, orbHandle_);
        orbHandle_ = 0;
        return false;
    }

    // Register remote-write callback for the per-ORB status block
    addrMgr_.SetRemoteWriteCallback(
        statusBlockHandle_,
        [this](uint64_t /*handle*/, uint32_t offset, std::span<const uint8_t> payload) {
            OnStatusBlockWrite(offset, payload);
        });

    return true;
}

void SBP2ManagementORB::DeallocateResources() noexcept {
    if (statusBlockHandle_ != 0) {
        addrMgr_.DeallocateAddressRange(owner_, statusBlockHandle_);
        statusBlockHandle_ = 0;
    }
    if (orbHandle_ != 0) {
        addrMgr_.DeallocateAddressRange(owner_, orbHandle_);
        orbHandle_ = 0;
    }
    orbMeta_ = {};
    statusBlockMeta_ = {};
}

// ---------------------------------------------------------------------------
// ORB construction
// ---------------------------------------------------------------------------

void SBP2ManagementORB::BuildManagementORB() noexcept {
    std::memset(&orbBuffer_, 0, sizeof(orbBuffer_));

    const uint16_t localNode =
        NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));

    // Options: notify (bit 15) | function code (low nibble)
    const auto fn = static_cast<uint16_t>(function_);
    orbBuffer_.options = ToBE16(static_cast<uint16_t>(0x8000u | fn));
    orbBuffer_.loginID = ToBE16(loginID_);

    // For AbortTask: set target ORB address (quadlet-aligned, big-endian)
    if (function_ == Function::AbortTask) {
        orbBuffer_.orbOffsetHi = ToBE32(targetORBAddressHi_);
        orbBuffer_.orbOffsetLo = ToBE32(targetORBAddressLo_ & 0xFFFFFFFCu);
    }

    // Status FIFO address
    orbBuffer_.statusFIFOAddressHi = ToBE32(
        ComposeBusAddressHi(localNode, statusBlockMeta_.addressHi));
    orbBuffer_.statusFIFOAddressLo = ToBE32(statusBlockMeta_.addressLo);

    // Write ORB to address space
    addrMgr_.WriteLocalData(
        owner_, orbHandle_, 0,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&orbBuffer_),
                                  sizeof(orbBuffer_)});

    // Build 8-byte management agent write payload: ORB address in BE
    orbAddressBE_[0] = static_cast<uint8_t>(localNode >> 8);
    orbAddressBE_[1] = static_cast<uint8_t>(localNode & 0xFF);
    orbAddressBE_[2] = static_cast<uint8_t>(orbMeta_.addressHi >> 8);
    orbAddressBE_[3] = static_cast<uint8_t>(orbMeta_.addressHi & 0xFF);
    const uint32_t addrLoBE = ToBE32(orbMeta_.addressLo);
    std::memcpy(&orbAddressBE_[4], &addrLoBE, sizeof(uint32_t));

    ASFW_LOG(Async,
             "SBP2ManagementORB: built function=%u loginID=%u ORB at %04x:%08x status at %04x:%08x",
             fn, loginID_,
             localNode, orbMeta_.addressLo,
             localNode, statusBlockMeta_.addressLo);
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

bool SBP2ManagementORB::Execute() noexcept {
    if (inProgress_.load(std::memory_order_relaxed)) {
        ASFW_LOG(Async, "SBP2ManagementORB::Execute: already in progress");
        return false;
    }

    if (!AllocateResources()) {
        return false;
    }

    BuildManagementORB();

    inProgress_.store(true, std::memory_order_relaxed);

    // Write ORB address to management agent
    const FW::Generation gen{generation_};
    const FW::NodeId node{static_cast<uint8_t>(nodeID_ & 0x3Fu)};
    const Async::FWAddress mgmtAddr{
        Async::FWAddress::QualifiedAddressParts{
            .addressHi = 0xFFFF,
            .addressLo = ManagementAgentAddressLo(managementAgentOffset_),
            .nodeID = nodeID_
        }
    };
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    writeHandle_ = bus_.WriteBlock(
        gen, node, mgmtAddr,
        std::span<const uint8_t>{orbAddressBE_.data(), orbAddressBE_.size()},
        speed,
        [this](Async::AsyncStatus status, std::span<const uint8_t> response) {
            OnWriteComplete(status, response);
        });

    if (!writeHandle_) {
        ASFW_LOG(Async, "SBP2ManagementORB::Execute: WriteBlock failed");
        inProgress_.store(false, std::memory_order_relaxed);
        return false;
    }

    ASFW_LOG(Async, "SBP2ManagementORB::Execute: wrote management ORB to agent");
    return true;
}

// ---------------------------------------------------------------------------
// Completion handlers
// ---------------------------------------------------------------------------

void SBP2ManagementORB::OnWriteComplete(Async::AsyncStatus status,
                                         std::span<const uint8_t> response) noexcept {
    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(Async, "SBP2ManagementORB::OnWriteComplete: status=%s",
                 Async::ToString(status));
        Complete(-1);
        return;
    }

    // Management agent write ACK'd. Start timeout, wait for status block.
    timerActive_.store(true, std::memory_order_relaxed);
    ASFW_LOG(Async, "SBP2ManagementORB: mgmt agent ACK'd, waiting for status block (timeout=%ums)",
             timeoutMs_);

    if (workQueue_ && timeoutMs_ > 0) {
        const uint32_t timeout = timeoutMs_;
        const uint64_t expectedGeneration =
            timerGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1ULL;
        const std::weak_ptr<int> weakLifetime = lifetimeToken_;
        const uint64_t delayNs = static_cast<uint64_t>(timeout) * 1'000'000ULL;

        DispatchAfterCompat(workQueue_, delayNs, [this, weakLifetime, expectedGeneration]() {
            if (weakLifetime.expired()) {
                return;
            }
            if (timerGeneration_.load(std::memory_order_acquire) != expectedGeneration ||
                !timerActive_.load(std::memory_order_relaxed) ||
                !inProgress_.load(std::memory_order_relaxed)) {
                return;
            }
            OnTimeout();
        });
    }
}

void SBP2ManagementORB::OnStatusBlockWrite(uint32_t offset,
                                             std::span<const uint8_t> payload) noexcept {
    if (!inProgress_.load(std::memory_order_relaxed)) {
        return;
    }

    ASFW_LOG(Async, "SBP2ManagementORB: received status block (offset=%u len=%zu)",
             offset, payload.size());

    Complete(0);
}

void SBP2ManagementORB::OnTimeout() noexcept {
    if (!inProgress_.load(std::memory_order_relaxed)) {
        return;
    }
    ASFW_LOG(Async, "SBP2ManagementORB: timeout");
    Complete(-2);
}

void SBP2ManagementORB::Complete(int status) noexcept {
    inProgress_.store(false, std::memory_order_relaxed);
    timerActive_.store(false, std::memory_order_relaxed);
    timerGeneration_.fetch_add(1, std::memory_order_acq_rel);

    if (completionCallback_) {
        completionCallback_(status);
    }
}

} // namespace ASFW::Protocols::SBP2
