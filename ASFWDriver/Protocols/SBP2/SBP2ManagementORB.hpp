#pragma once

// SBP-2 Management ORB — task abort, logical unit reset, target reset.
// Written to the management agent address (same as login/reconnect/logout).
// Has its own per-ORB status FIFO address space.
//
// Ref: SBP-2 §6 (Task Management)

#include "AddressSpaceManager.hpp"
#include "SBP2WireFormats.hpp"
#include "../../Async/AsyncTypes.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#endif

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <span>

namespace ASFW::Async {
class IFireWireBus;
class IFireWireBusInfo;
}

namespace ASFW::Protocols::SBP2 {

class SBP2ManagementORB {
public:
    using CompletionCallback = std::function<void(int status)>;

    enum class Function : uint16_t {
        QueryLogins      = 1,
        AbortTask        = 0xB,
        AbortTaskSet     = 0xC,
        LogicalUnitReset = 0xE,
        TargetReset      = 0xF,
    };

    SBP2ManagementORB(Async::IFireWireBus& bus,
                      Async::IFireWireBusInfo& busInfo,
                      AddressSpaceManager& addrMgr, void* owner);
    ~SBP2ManagementORB();

    SBP2ManagementORB(const SBP2ManagementORB&) = delete;
    SBP2ManagementORB& operator=(const SBP2ManagementORB&) = delete;

    // Configuration (call before Execute)
    void SetFunction(Function fn) noexcept { function_ = fn; }
    void SetLoginID(uint16_t loginID) noexcept { loginID_ = loginID; }
    void SetTargetORBAddress(uint32_t hi, uint32_t lo) noexcept {
        targetORBAddressHi_ = hi;
        targetORBAddressLo_ = lo;
    }
    void SetManagementAgentOffset(uint32_t offset) noexcept { managementAgentOffset_ = offset; }
    void SetTimeout(uint32_t ms) noexcept { timeoutMs_ = ms; }
    void SetCompletionCallback(CompletionCallback cb) noexcept { completionCallback_ = std::move(cb); }

    // Set node targeting before Execute.
    void SetTargetNode(uint16_t generation, uint16_t nodeID) noexcept {
        generation_ = generation;
        nodeID_ = nodeID;
    }

    void SetWorkQueue(IODispatchQueue* queue) noexcept { workQueue_ = queue; }

    // Lifecycle
    [[nodiscard]] bool Execute() noexcept;

    [[nodiscard]] Function GetFunction() const noexcept { return function_; }
    [[nodiscard]] bool InProgress() const noexcept { return inProgress_.load(std::memory_order_relaxed); }

private:
    bool AllocateResources() noexcept;
    void DeallocateResources() noexcept;
    void BuildManagementORB() noexcept;

    void OnWriteComplete(Async::AsyncStatus status, std::span<const uint8_t> response) noexcept;
    void OnStatusBlockWrite(uint32_t offset, std::span<const uint8_t> payload) noexcept;
    void OnTimeout() noexcept;
    void Complete(int status) noexcept;

    // Dependencies
    Async::IFireWireBus& bus_;
    Async::IFireWireBusInfo& busInfo_;
    AddressSpaceManager& addrMgr_;
    void* owner_;

    // Configuration
    Function function_{Function::AbortTaskSet};
    uint16_t loginID_{0};
    uint32_t managementAgentOffset_{0};
    uint32_t timeoutMs_{2000};
    CompletionCallback completionCallback_;

    // Target ORB (AbortTask only)
    uint32_t targetORBAddressHi_{0};
    uint32_t targetORBAddressLo_{0};

    // ORB buffer + address space
    uint64_t orbHandle_{0};
    AddressSpaceManager::AddressRangeMeta orbMeta_{};
    Wire::TaskManagementORB orbBuffer_{};

    // Per-ORB status block address space
    uint64_t statusBlockHandle_{0};
    AddressSpaceManager::AddressRangeMeta statusBlockMeta_{};

    // Management agent write payload (8-byte BE ORB address)
    std::array<uint8_t, 8> orbAddressBE_{};
    Async::AsyncHandle writeHandle_{};

    // State
    std::atomic<bool> inProgress_{false};
    std::atomic<bool> timerActive_{false};

    // Node targeting
    uint16_t generation_{0};
    uint16_t nodeID_{0xFFFF};

    // Timer infrastructure
    IODispatchQueue* workQueue_{nullptr};
    std::atomic<uint64_t> timerGeneration_{0};
    std::shared_ptr<int> lifetimeToken_{std::make_shared<int>(0)};
};

} // namespace ASFW::Protocols::SBP2
