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

using ManagementAsyncState = SBP2ManagementORB::AsyncState;

Async::AsyncHandle TakeWriteHandle(const std::shared_ptr<ManagementAsyncState>& state) noexcept {
    return Async::AsyncHandle{state->writeHandleValue.exchange(0, std::memory_order_acq_rel)};
}

void ClearStatusBlockCallback(const std::shared_ptr<ManagementAsyncState>& state) noexcept {
    const uint64_t handle = state->statusBlockHandle.load(std::memory_order_acquire);
    if (handle != 0 && state->addrMgr != nullptr) {
        state->addrMgr->SetRemoteWriteCallback(handle, {});
    }
}

bool CompleteAsyncOperation(const std::shared_ptr<ManagementAsyncState>& state,
                            int status) noexcept {
    if (!state->inProgress.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }

    state->timerActive.store(false, std::memory_order_relaxed);
    state->timerGeneration.fetch_add(1, std::memory_order_acq_rel);
    ClearStatusBlockCallback(state);

    const Async::AsyncHandle pendingWrite = TakeWriteHandle(state);
    if (pendingWrite && state->bus != nullptr) {
        (void)state->bus->Cancel(pendingWrite);
    }

    if (!state->destroyed.load(std::memory_order_acquire) &&
        state->completionCallback) {
        state->completionCallback(status);
    }
    return true;
}

void HandleStatusBlockWrite(const std::shared_ptr<ManagementAsyncState>& state,
                            uint32_t offset,
                            std::span<const uint8_t> payload) noexcept {
    if (!state->inProgress.load(std::memory_order_relaxed)) {
        return;
    }

    ASFW_LOG(SBP2, "SBP2ManagementORB: received status block (offset=%u len=%zu)",
             offset, payload.size());

    if (offset != 0 || payload.size() < 8 || payload.size() > Wire::StatusBlock::kMaxSize) {
        (void)CompleteAsyncOperation(state, kManagementMalformedStatus);
        return;
    }

    Wire::StatusBlock block{};
    std::memcpy(&block, payload.data(), payload.size());

    const uint16_t orbOffsetHi = FromBE16(block.orbOffsetHi);
    const uint32_t orbOffsetLo = FromBE32(block.orbOffsetLo);
    if (orbOffsetHi != state->expectedORBAddressHi.load(std::memory_order_acquire) ||
        orbOffsetLo != state->expectedORBAddressLo.load(std::memory_order_acquire)) {
        ASFW_LOG(SBP2,
                 "SBP2ManagementORB: status block ORB mismatch expected=%04x:%08x got=%04x:%08x",
                 state->expectedORBAddressHi.load(std::memory_order_relaxed),
                 state->expectedORBAddressLo.load(std::memory_order_relaxed),
                 orbOffsetHi,
                 orbOffsetLo);
        (void)CompleteAsyncOperation(state, kManagementMalformedStatus);
        return;
    }

    if (block.Response() != 0 || block.DeadBit() != 0) {
        ASFW_LOG(SBP2,
                 "SBP2ManagementORB: device rejected management ORB resp=%u dead=%u status=%u",
                 block.Response(),
                 block.DeadBit(),
                 block.sbpStatus);
        (void)CompleteAsyncOperation(state, kManagementDeviceFailure);
        return;
    }

    if (block.sbpStatus != Wire::SBPStatus::kNoAdditionalInfo) {
        ASFW_LOG(SBP2,
                 "SBP2ManagementORB: management ORB completed with sbpStatus=%u",
                 block.sbpStatus);
        (void)CompleteAsyncOperation(state, kManagementDeviceFailure);
        return;
    }

    (void)CompleteAsyncOperation(state, 0);
}

void HandleTimeout(const std::shared_ptr<ManagementAsyncState>& state) noexcept {
    if (!state->inProgress.load(std::memory_order_relaxed)) {
        return;
    }

    ASFW_LOG(SBP2, "SBP2ManagementORB: timeout");
    (void)CompleteAsyncOperation(state, kManagementTimeout);
}

void HandleWriteComplete(const std::shared_ptr<ManagementAsyncState>& state,
                         Async::AsyncStatus status,
                         std::span<const uint8_t> /*response*/,
                         uint32_t timeoutMs,
                         IODispatchQueue* workQueue,
                         IODispatchQueue* timeoutQueue) noexcept {
    state->writeHandleValue.store(0, std::memory_order_release);

    if (!state->inProgress.load(std::memory_order_relaxed)) {
        return;
    }

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(SBP2, "SBP2ManagementORB::OnWriteComplete: status=%s",
                 Async::ToString(status));
        (void)CompleteAsyncOperation(state, kManagementTransportFailure);
        return;
    }

    state->timerActive.store(true, std::memory_order_relaxed);
    ASFW_LOG(SBP2, "SBP2ManagementORB: mgmt agent ACK'd, waiting for status block (timeout=%ums)",
             timeoutMs);

    if (workQueue == nullptr || timeoutQueue == nullptr || timeoutMs == 0) {
        return;
    }

    const uint64_t expectedGeneration =
        state->timerGeneration.fetch_add(1, std::memory_order_acq_rel) + 1ULL;
    const uint64_t delayNs = static_cast<uint64_t>(timeoutMs) * 1'000'000ULL;

    DispatchAfterCompat(timeoutQueue, delayNs, [state, expectedGeneration, workQueue]() {
        DispatchAsyncCompat(workQueue, [state, expectedGeneration]() {
            if (state->timerGeneration.load(std::memory_order_acquire) != expectedGeneration ||
                !state->timerActive.load(std::memory_order_relaxed) ||
                !state->inProgress.load(std::memory_order_relaxed)) {
                return;
            }
            HandleTimeout(state);
        });
    });
}

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
    , owner_(owner)
    , asyncState_(std::make_shared<AsyncState>(&bus_, &addrMgr_)) {}

SBP2ManagementORB::~SBP2ManagementORB() {
    asyncState_->destroyed.store(true, std::memory_order_release);
    asyncState_->inProgress.store(false, std::memory_order_relaxed);
    asyncState_->timerActive.store(false, std::memory_order_relaxed);
    asyncState_->timerGeneration.fetch_add(1, std::memory_order_acq_rel);
    ClearStatusBlockCallback(asyncState_);
    const Async::AsyncHandle pendingHandle = TakeWriteHandle(asyncState_);
    if (pendingHandle) {
        (void)bus_.Cancel(pendingHandle);
    }
    DeallocateResources();
}

// ---------------------------------------------------------------------------
// Resource allocation
// ---------------------------------------------------------------------------

bool SBP2ManagementORB::AllocateResources() noexcept {
    const auto state = asyncState_;
    const auto registerStatusWriteCallback = [this, state]() {
        addrMgr_.SetRemoteWriteCallback(
            statusBlockHandle_,
            [state](uint64_t /*handle*/, uint32_t offset, std::span<const uint8_t> payload) {
                HandleStatusBlockWrite(state, offset, payload);
            });
        state->statusBlockHandle.store(statusBlockHandle_, std::memory_order_release);
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
    if (asyncState_->inProgress.load(std::memory_order_relaxed)) {
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

    asyncState_->destroyed.store(false, std::memory_order_release);
    asyncState_->completionCallback = completionCallback_;
    asyncState_->expectedORBAddressHi.store(orbMeta_.addressHi, std::memory_order_release);
    asyncState_->expectedORBAddressLo.store(orbMeta_.addressLo, std::memory_order_release);
    asyncState_->timerActive.store(false, std::memory_order_relaxed);
    asyncState_->inProgress.store(true, std::memory_order_relaxed);

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

    const Async::AsyncHandle writeHandle = bus_.WriteBlock(
        gen, node, mgmtAddr,
        std::span<const uint8_t>{orbAddressBE_.data(), orbAddressBE_.size()},
        speed,
        [state = asyncState_,
         timeoutMs = timeoutMs_,
         workQueue = workQueue_,
         effectiveTimeoutQueue = (timeoutQueue_ != nullptr ? timeoutQueue_ : workQueue_)](
            Async::AsyncStatus status,
            std::span<const uint8_t> response) {
            HandleWriteComplete(state, status, response, timeoutMs, workQueue, effectiveTimeoutQueue);
        });

    if (!writeHandle) {
        ASFW_LOG(SBP2, "SBP2ManagementORB::Execute: WriteBlock failed");
        asyncState_->inProgress.store(false, std::memory_order_relaxed);
        return false;
    }
    asyncState_->writeHandleValue.store(writeHandle.value, std::memory_order_release);

    ASFW_LOG(SBP2, "SBP2ManagementORB::Execute: wrote management ORB to agent");
    return true;
}

} // namespace ASFW::Protocols::SBP2
