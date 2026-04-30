#pragma once

// SBP-2 Normal Command ORB.
// Represents a single SCSI command submitted to the device after login.
//
// Ref: SBP-2 §5.1.1 (Normal Command ORB format)

#include "AddressSpaceManager.hpp"
#include "SBP2PageTable.hpp"
#include "SBP2WireFormats.hpp"
#include "../../Async/AsyncTypes.hpp"
#include "../../Common/FWCommon.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#endif

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <span>

namespace ASFW::Protocols::SBP2 {

class SBP2CommandORB {
public:
    enum Flags : uint32_t {
        kNotify         = (1 << 0),
        kDataFromTarget = (1 << 1),
        kImmediate      = (1 << 2),
        kNormalORB      = (1 << 5),
        kReservedORB    = (1 << 6),
        kVendorORB      = (1 << 7),
        kDummyORB       = (1 << 8),
    };

    using CompletionCallback = std::function<void(int transportStatus, uint8_t sbpStatus)>;

    SBP2CommandORB(AddressSpaceManager& addrMgr, void* owner,
                   uint32_t maxCommandBlockSize);
    ~SBP2CommandORB();

    SBP2CommandORB(const SBP2CommandORB&) = delete;
    SBP2CommandORB& operator=(const SBP2CommandORB&) = delete;

    // Configuration (call before submit)
    void SetCommandBlock(std::span<const uint8_t> cdb) noexcept;
    void SetFlags(uint32_t flags) noexcept { flags_ = flags; }
    void SetMaxPayloadSize(uint16_t bytes) noexcept { maxPayloadSize_ = bytes; }
    void SetTimeout(uint32_t ms) noexcept { timeoutDuration_ = ms; }
    void SetCompletionCallback(CompletionCallback cb) noexcept { completionCallback_ = std::move(cb); }

    // Bind page table result from SBP2PageTable::Build.
    void SetDataDescriptor(const SBP2PageTable::Result& ptResult) noexcept {
        dataDescriptor_ = ptResult;
    }

    // Internal: called by the session layer before submission.
    void PrepareForExecution(uint16_t localNodeID, FW::FwSpeed speed,
                             uint16_t maxPayloadLog) noexcept;

    // Internal: ORB address for fetch agent / chaining.
    [[nodiscard]] Async::FWAddress GetORBAddress() const noexcept;

    // Internal: set the next ORB pointer (big-endian values).
    void SetNextORBAddress(uint32_t hi, uint32_t lo) noexcept;

    // Set rq_fmt=3 (NOP dummy) so device skips this ORB if already fetched.
    void SetToDummy() noexcept;

    // Internal: timer management.
    void StartTimer(IODispatchQueue* queue) noexcept;
    void CancelTimer() noexcept;

    // State tracking.
    [[nodiscard]] bool IsAppended() const noexcept { return isAppended_; }
    void SetAppended(bool state) noexcept { isAppended_ = state; }

    [[nodiscard]] uint32_t GetFetchAgentWriteRetries() const noexcept { return fetchAgentWriteRetries_; }
    void SetFetchAgentWriteRetries(uint32_t retries) noexcept { fetchAgentWriteRetries_ = retries; }

    [[nodiscard]] uint32_t GetFlags() const noexcept { return flags_; }
    [[nodiscard]] CompletionCallback& GetCompletionCallback() noexcept { return completionCallback_; }

private:
    bool AllocateResources() noexcept;
    void DeallocateResources() noexcept;
    void WriteORBToAddressSpace() noexcept;

    AddressSpaceManager& addrMgr_;
    void* owner_;
    uint32_t maxCommandBlockSize_;

    uint32_t flags_{0};
    uint16_t maxPayloadSize_{0};
    uint32_t timeoutDuration_{0};
    CompletionCallback completionCallback_;

    // ORB buffer — local copy, written to address space before submission.
    uint64_t orbHandle_{0};
    AddressSpaceManager::AddressRangeMeta orbMeta_;
    std::vector<uint8_t> orbStorage_;

    // Page table binding.
    SBP2PageTable::Result dataDescriptor_{};

    // State.
    bool isAppended_{false};
    std::atomic<bool> inProgress_{false};
    uint32_t fetchAgentWriteRetries_{20};

    // Timer.
    IODispatchQueue* timerQueue_{nullptr};
    std::atomic<uint64_t> timerGeneration_{0};
    std::shared_ptr<int> lifetimeToken_{std::make_shared<int>(0)};
};

} // namespace ASFW::Protocols::SBP2
