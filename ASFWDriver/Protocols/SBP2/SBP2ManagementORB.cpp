// SBP-2 Management ORB implementation.
// Ported from Apple IOFireWireSBP2ManagementORB.
// Ref: SBP-2 §6 (Task Management)

#include "SBP2ManagementORB.hpp"
#include "SBP2DelayedDispatch.hpp"

#include "../../Async/Interfaces/IFireWireBus.hpp"
#include "../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../Common/FWCommon.hpp"

namespace ASFW::Protocols::SBP2 {

using namespace ASFW::Protocols::SBP2::Wire;

namespace {

constexpr int kManagementTransportFailure = -1;
constexpr int kManagementTimeout = -2;
constexpr int kManagementMalformedStatus = -3;
constexpr int kManagementDeviceFailure = -4;

} // namespace

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
    inProgress_.store(false, std::memory_order_relaxed);
    timerActive_.store(false, std::memory_order_relaxed);
    timerGeneration_.fetch_add(1, std::memory_order_acq_rel);
    if (statusBlockHandle_ != 0) {
        addrMgr_.SetRemoteWriteCallback(statusBlockHandle_, {});
    }
    if (writeHandle_) {
        const Async::AsyncHandle pendingHandle = writeHandle_;
        writeHandle_ = {};
        (void)bus_.Cancel(pendingHandle);
    }
    lifetimeToken_.reset();
    DeallocateResources();
}

// ---------------------------------------------------------------------------
// Resource allocation
// ---------------------------------------------------------------------------

bool SBP2ManagementORB::AllocateResources() noexcept {
    const auto registerStatusWriteCallback = [this]() {
        const std::weak_ptr<int> weakLifetime = lifetimeToken_;
        addrMgr_.SetRemoteWriteCallback(
            statusBlockHandle_,
            [this, weakLifetime](uint64_t /*handle*/,
                                 uint32_t offset,
                                 std::span<const uint8_t> payload) {
                if (weakLifetime.expired()) {
                    return;
                }
                OnStatusBlockWrite(offset, payload);
            });
    };

    if (orbHandle_ != 0 && statusBlockHandle_ != 0) {
        registerStatusWriteCallback();
        return true;
    }

    // Allocate ORB address space (32 bytes)
    auto kr = addrMgr_.AllocateAddressRangeAuto(
        owner_, 0xFFFF, Wire::TaskManagementORB::kSize,
        &orbHandle_, &orbMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2ManagementORB: failed to allocate ORB: 0x%08x", kr);
        return false;
    }

    // Allocate per-ORB status block address space (32 bytes)
    kr = addrMgr_.AllocateAddressRangeAuto(
        owner_, 0xFFFF, Wire::StatusBlock::kMaxSize,
        &statusBlockHandle_, &statusBlockMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2ManagementORB: failed to allocate status block: 0x%08x", kr);
        addrMgr_.DeallocateAddressRange(owner_, orbHandle_);
        orbHandle_ = 0;
        return false;
    }

    // Register remote-write callback for the per-ORB status block
    registerStatusWriteCallback();

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

kern_return_t SBP2ManagementORB::BuildManagementORB() noexcept {
    if (orbHandle_ == 0 || statusBlockHandle_ == 0) {
        return kIOReturnNotReady;
    }

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
    const kern_return_t writeKr = addrMgr_.WriteLocalData(
        owner_, orbHandle_, 0,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&orbBuffer_),
                                  sizeof(orbBuffer_)});
    if (writeKr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2ManagementORB: failed to write management ORB: 0x%08x", writeKr);
        return writeKr;
    }

    // Build 8-byte management agent write payload: ORB address in BE
    orbAddressBE_[0] = static_cast<uint8_t>(localNode >> 8);
    orbAddressBE_[1] = static_cast<uint8_t>(localNode & 0xFF);
    orbAddressBE_[2] = static_cast<uint8_t>(orbMeta_.addressHi >> 8);
    orbAddressBE_[3] = static_cast<uint8_t>(orbMeta_.addressHi & 0xFF);
    const uint32_t addrLoBE = ToBE32(orbMeta_.addressLo);
    std::memcpy(&orbAddressBE_[4], &addrLoBE, sizeof(uint32_t));

    ASFW_LOG(SBP2,
             "SBP2ManagementORB: built function=%u loginID=%u ORB at %04x:%08x status at %04x:%08x",
             fn, loginID_,
             localNode, orbMeta_.addressLo,
             localNode, statusBlockMeta_.addressLo);
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

bool SBP2ManagementORB::Execute() noexcept {
    if (inProgress_.load(std::memory_order_relaxed)) {
        ASFW_LOG(SBP2, "SBP2ManagementORB::Execute: already in progress");
        return false;
    }

    if (!AllocateResources()) {
        return false;
    }

    const kern_return_t buildKr = BuildManagementORB();
    if (buildKr != kIOReturnSuccess) {
        return false;
    }

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

    const std::weak_ptr<int> weakLifetime = lifetimeToken_;
    writeHandle_ = bus_.WriteBlock(
        gen, node, mgmtAddr,
        std::span<const uint8_t>{orbAddressBE_.data(), orbAddressBE_.size()},
        speed,
        [this, weakLifetime](Async::AsyncStatus status, std::span<const uint8_t> response) {
            if (weakLifetime.expired()) {
                return;
            }
            OnWriteComplete(status, response);
        });

    if (!writeHandle_) {
        ASFW_LOG(SBP2, "SBP2ManagementORB::Execute: WriteBlock failed");
        inProgress_.store(false, std::memory_order_relaxed);
        return false;
    }

    ASFW_LOG(SBP2, "SBP2ManagementORB::Execute: wrote management ORB to agent");
    return true;
}

// ---------------------------------------------------------------------------
// Completion handlers
// ---------------------------------------------------------------------------

void SBP2ManagementORB::OnWriteComplete(Async::AsyncStatus status,
                                         std::span<const uint8_t> response) noexcept {
    writeHandle_ = {};

    if (!inProgress_.load(std::memory_order_relaxed)) {
        return;
    }

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(SBP2, "SBP2ManagementORB::OnWriteComplete: status=%s",
                 Async::ToString(status));
        Complete(kManagementTransportFailure);
        return;
    }

    // Management agent write ACK'd. Start timeout, wait for status block.
    timerActive_.store(true, std::memory_order_relaxed);
    ASFW_LOG(SBP2, "SBP2ManagementORB: mgmt agent ACK'd, waiting for status block (timeout=%ums)",
             timeoutMs_);

    IODispatchQueue* effectiveTimeoutQueue = timeoutQueue_ != nullptr ? timeoutQueue_ : workQueue_;
    if (workQueue_ && effectiveTimeoutQueue && timeoutMs_ > 0) {
        const uint32_t timeout = timeoutMs_;
        const uint64_t expectedGeneration =
            timerGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1ULL;
        const std::weak_ptr<int> weakLifetime = lifetimeToken_;
        const uint64_t delayNs = static_cast<uint64_t>(timeout) * 1'000'000ULL;

        DispatchAfterCompat(effectiveTimeoutQueue, delayNs, [this,
                                                             weakLifetime,
                                                             expectedGeneration]() {
            if (weakLifetime.expired()) {
                return;
            }
            DispatchAsyncCompat(workQueue_, [this, weakLifetime, expectedGeneration]() {
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
        });
    }
}

void SBP2ManagementORB::OnStatusBlockWrite(uint32_t offset,
                                             std::span<const uint8_t> payload) noexcept {
    if (!inProgress_.load(std::memory_order_relaxed)) {
        return;
    }

    ASFW_LOG(SBP2, "SBP2ManagementORB: received status block (offset=%u len=%zu)",
             offset, payload.size());

    if (offset != 0 || payload.size() < 8 || payload.size() > Wire::StatusBlock::kMaxSize) {
        Complete(kManagementMalformedStatus);
        return;
    }

    Wire::StatusBlock block{};
    std::memcpy(&block, payload.data(), payload.size());

    const uint16_t orbOffsetHi = FromBE16(block.orbOffsetHi);
    const uint32_t orbOffsetLo = FromBE32(block.orbOffsetLo);
    if (orbOffsetHi != orbMeta_.addressHi || orbOffsetLo != orbMeta_.addressLo) {
        ASFW_LOG(SBP2,
                 "SBP2ManagementORB: status block ORB mismatch expected=%04x:%08x got=%04x:%08x",
                 orbMeta_.addressHi,
                 orbMeta_.addressLo,
                 orbOffsetHi,
                 orbOffsetLo);
        Complete(kManagementMalformedStatus);
        return;
    }

    if (block.Response() != 0 || block.DeadBit() != 0) {
        ASFW_LOG(SBP2,
                 "SBP2ManagementORB: device rejected management ORB resp=%u dead=%u status=%u",
                 block.Response(),
                 block.DeadBit(),
                 block.sbpStatus);
        Complete(kManagementDeviceFailure);
        return;
    }

    if (block.sbpStatus != Wire::SBPStatus::kNoAdditionalInfo) {
        ASFW_LOG(SBP2,
                 "SBP2ManagementORB: management ORB completed with sbpStatus=%u",
                 block.sbpStatus);
        Complete(kManagementDeviceFailure);
        return;
    }

    Complete(0);
}

void SBP2ManagementORB::OnTimeout() noexcept {
    if (!inProgress_.load(std::memory_order_relaxed)) {
        return;
    }
    ASFW_LOG(SBP2, "SBP2ManagementORB: timeout");
    Complete(kManagementTimeout);
}

void SBP2ManagementORB::Complete(int status) noexcept {
    if (!inProgress_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    timerActive_.store(false, std::memory_order_relaxed);
    timerGeneration_.fetch_add(1, std::memory_order_acq_rel);
    if (statusBlockHandle_ != 0) {
        addrMgr_.SetRemoteWriteCallback(statusBlockHandle_, {});
    }
    if (writeHandle_) {
        const Async::AsyncHandle pendingHandle = writeHandle_;
        writeHandle_ = {};
        (void)bus_.Cancel(pendingHandle);
    }

    if (completionCallback_) {
        completionCallback_(status);
    }
}

} // namespace ASFW::Protocols::SBP2
