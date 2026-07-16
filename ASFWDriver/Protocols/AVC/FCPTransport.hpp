//
// FCPTransport.hpp
// ASFWDriver - AV/C Protocol Layer
//
// FCP (Function Control Protocol) transport layer
// Manages command/response exchange via IEEE 1394 async block writes
//

#pragma once

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/IODispatchQueue.h>
#endif
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include "AVCDefs.hpp"
#include "../Ports/FireWireBusPort.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"

namespace ASFW::Protocols::AVC {

//==============================================================================
// FCP Status Codes
//==============================================================================

/// FCP transport-level status
enum class FCPStatus : uint8_t {
    kOk = 0,                ///< Success
    kTimeout,               ///< Command timed out
    kBusReset,              ///< Bus reset during command
    kTransportError,        ///< Async write/read error
    kInvalidPayload,        ///< Payload size invalid
    kResponseMismatch,      ///< Response doesn't match command
    kBusy,                  ///< Command already pending
};

//==============================================================================
// FCP Frame
//==============================================================================

/// FCP frame (command or response payload)
struct FCPFrame {
    std::array<uint8_t, kAVCFrameMaxSize> data{};
    size_t length{0};

    /// Get payload as read-only span
    std::span<const uint8_t> Payload() const {
        return {data.data(), length};
    }

    /// Get payload as mutable span
    std::span<uint8_t> MutablePayload() {
        return {data.data(), length};
    }

    /// Validate frame size
    bool IsValid() const {
        return length >= kAVCFrameMinSize && length <= kAVCFrameMaxSize;
    }
};

//==============================================================================
// FCP Completion Callback
//==============================================================================

/// Completion callback for FCP command submission
///
/// @param status FCP transport status
/// @param response Response frame (valid only if status == kOk)
using FCPCompletion = std::function<void(FCPStatus status,
                                         const FCPFrame& response)>;

// A command can require response fields beyond the AV/C address/opcode pair.
// It is evaluated only after structural bounds checks and the transport's
// default matcher have accepted the frame.
using FCPResponseMatcher = std::function<bool(std::span<const uint8_t> command,
                                              std::span<const uint8_t> response)>;

enum class FCPRetryClass : uint8_t {
    // Control/state-changing commands are never replayed automatically.
    kNever,
    // Safe read-only/status requests may retry after timeout/write failure and
    // (when enabled by transport config) a bus reset.
    kIdempotent,
};

struct FCPCommandPolicy {
    FCPResponseMatcher responseMatcher{};
    FCPRetryClass retryClass{FCPRetryClass::kNever};
};

enum class FCPQueuePolicy : uint8_t {
    kReject,
    kFifo,
};

//==============================================================================
// FCP Handle
//==============================================================================

/// FCP transaction handle (opaque identifier)
struct FCPHandle {
    uint32_t transactionID{0};

    bool IsValid() const { return transactionID != 0; }
    void Invalidate() { transactionID = 0; }
};

//==============================================================================
// FCP Transport Configuration
//==============================================================================

/// FCP transport configuration
struct FCPTransportConfig {
    /// FCP command CSR address (target receives commands here)
    uint64_t commandAddress{kFCPCommandAddress};

    /// FCP response CSR address (initiator receives responses here)
    uint64_t responseAddress{kFCPResponseAddress};

    /// Initial timeout (milliseconds)
    uint32_t timeoutMs{kFCPTimeoutInitial};

    /// Timeout after interim response (milliseconds)
    uint32_t interimTimeoutMs{kFCPTimeoutAfterInterim};

    /// Maximum retry attempts
    uint8_t maxRetries{kFCPMaxRetries};

    /// Allow bus reset retry (default: false, fail on reset)
    bool allowBusResetRetry{false};

    /// Commands submitted while one is active either queue FIFO or fail Busy.
    FCPQueuePolicy queuePolicy{FCPQueuePolicy::kFifo};
};

//==============================================================================
// FCP Transport
//==============================================================================

class FCPTransport : public std::enable_shared_from_this<FCPTransport> {
public:
    FCPTransport() = default;
    ~FCPTransport();

    bool init(Protocols::Ports::FireWireBusOps* busOps,
              Protocols::Ports::FireWireBusInfo* busInfo,
              Discovery::FWDevice* device,
              Scheduling::ITimerScheduler& timerScheduler,
              const FCPTransportConfig& config = {});

    FCPTransport(const FCPTransport&) = delete;
    FCPTransport& operator=(const FCPTransport&) = delete;

    [[nodiscard]] FCPHandle SubmitCommand(const FCPFrame& command,
                                          FCPCompletion completion);
    [[nodiscard]] FCPHandle SubmitCommand(const FCPFrame& command,
                                          FCPCompletion completion,
                                          FCPCommandPolicy policy);

    /// Stop retries and complete any outstanding command without more bus I/O.
    void Shutdown();

    bool CancelCommand(FCPHandle handle);

    void OnFCPResponse(uint16_t srcNodeID,
                       uint32_t generation,
                       std::span<const uint8_t> payload);

    void OnBusReset(uint32_t newGeneration);

    const FCPTransportConfig& GetConfig() const { return config_; }

    // The FWDevice object survives a bus reset and is updated with its new
    // node ID before observers are resumed. CMP must use this live identity,
    // never a node ID cached by a protocol adapter before the reset.
    [[nodiscard]] uint16_t GetTargetNodeID() const noexcept {
        return device_ ? device_->GetNodeID() : 0xFFFFU;
    }

private:
    /// Immutable routing epoch for one FCP block-write attempt.  A response
    /// may match only after this exact attempt has completed successfully.
    /// Linux pairs destination identity with generation at request issue
    /// (core-transaction.c:285-303, 363-372); Apple's AVC stack retains the
    /// equivalent fWriteNodeID/fWriteGen on its write command
    /// (IOFireWireAVCCommand.cpp:481-491).  We reproduce that behavior, not
    /// their implementation.
    struct FCPWriteAttempt {
        uint64_t id{0};
        uint16_t targetNodeID{0xFFFFU};
        uint32_t generation{0};
    };

    struct OutstandingCommand {
        FCPFrame command;
        FCPCompletion completion;
        FCPCommandPolicy policy;
        uint32_t transactionID{0};
        uint8_t retriesLeft;
        bool allowBusResetRetry;
        /// The currently submitted async write. Cleared before cancellation
        /// so its late completion cannot affect a replacement attempt.
        std::optional<FCPWriteAttempt> activeWriteAttempt;
        /// Set only by the success completion of `activeWriteAttempt`. Response
        /// matching reads this value and never mutable device/bus state.
        std::optional<FCPWriteAttempt> successfulWriteAttempt;
        bool gotInterim{false};

        Async::AsyncHandle asyncHandle;
        Scheduling::TimerToken timeoutToken{Scheduling::kInvalidTimerToken};
        uint64_t timeoutEpoch{0};
    };

    void OnAsyncWriteComplete(FCPWriteAttempt writeAttempt,
                              Async::AsyncStatus status,
                              std::span<const uint8_t> response);

    Async::AsyncHandle SubmitWriteCommand(const FCPFrame& frame,
                                          FCPWriteAttempt writeAttempt);

    [[nodiscard]] bool StartPendingWrite();
    void StartNextQueuedCommand();

    void OnCommandTimeout();

    void RetryCommand();

    bool ValidateResponse(std::span<const uint8_t> response) const;

    void CompleteCommand(FCPStatus status, const FCPFrame& response);

    void ScheduleTimeout(uint32_t timeoutMs);

    void CancelTimeout();

    Protocols::Ports::FireWireBusOps* busOps_{nullptr};
    Protocols::Ports::FireWireBusInfo* busInfo_{nullptr};
    Discovery::FWDevice* device_{nullptr};
    Scheduling::ITimerScheduler* timerScheduler_{nullptr};
    FCPTransportConfig config_;

    IOLock* lock_{nullptr};

    uint64_t nextTimeoutEpoch_{0};
    uint64_t nextWriteAttempt_{0};

    // Protected by lock_. Timeout and async-completion blocks capture shared
    // ownership, so destruction cannot race their callback target.
    bool shuttingDown_{false};

    std::unique_ptr<OutstandingCommand> pending_;
    std::deque<std::unique_ptr<OutstandingCommand>> queued_;
    uint32_t nextTransactionID_{0};
};

} // namespace ASFW::Protocols::AVC
