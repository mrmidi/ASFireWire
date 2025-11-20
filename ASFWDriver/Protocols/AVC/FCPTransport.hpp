//
// FCPTransport.hpp
// ASFWDriver - AV/C Protocol Layer
//
// FCP (Function Control Protocol) transport layer
// Manages command/response exchange via IEEE 1394 async block writes
//

#pragma once

#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/OSSharedPtr.h>
#include <array>
#include <functional>
#include <memory>
#include <span>
#include "AVCDefs.hpp"
#include "../../Async/AsyncSubsystem.hpp"
#include "../../Discovery/FWDevice.hpp"

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
};

//==============================================================================
// FCP Transport
//==============================================================================

/// FCP Transport - manages command/response exchange via async writes
///
/// CONCURRENCY MODEL (v1):
/// - Enforces SINGLE outstanding command per transport instance
/// - Response correlation is reliable (no transaction ID on wire)
/// - Second command submission returns kBusy until first completes
///
/// INTERIM RESPONSE HANDLING:
/// - When device sends interim (ctype 0x0F), automatically extends timeout
/// - Interim response is NOT visible to caller - only final response delivered
///
/// BUS RESET POLICY:
/// - Default: fail pending command on bus reset
/// - Optional: retry if allowBusResetRetry=true (STATUS queries only)
///
/// THREAD SAFETY:
/// - All methods are thread-safe (protected by IOLock)
/// - Completion callback invoked OUTSIDE lock (never hold lock during callback)
/// - Timeout handler runs on dedicated dispatch queue
class FCPTransport {
public:
    /// Constructor
    ///
    /// @param async Reference to async subsystem
    /// @param device Target device (for nodeID + generation)
    /// @param config Transport configuration
    FCPTransport(Async::AsyncSubsystem& async,
                 Discovery::FWDevice& device,
                 const FCPTransportConfig& config = {});

    ~FCPTransport();

    // Non-copyable, non-movable
    FCPTransport(const FCPTransport&) = delete;
    FCPTransport& operator=(const FCPTransport&) = delete;

    /// Submit FCP command (async block write to target's FCP_COMMAND_ADDRESS)
    ///
    /// CONCURRENCY: v1 allows ONLY ONE outstanding command at a time.
    /// If a command is already pending, this returns kBusy immediately.
    ///
    /// @param command Command payload (3-512 bytes, validated)
    /// @param completion Callback with response or error
    /// @return Handle for tracking/cancellation (invalid if rejected)
    [[nodiscard]] FCPHandle SubmitCommand(const FCPFrame& command,
                                          FCPCompletion completion);

    /// Cancel outstanding command
    ///
    /// @param handle Command handle to cancel
    /// @return true if command was found and cancelled
    bool CancelCommand(FCPHandle handle);

    /// Called by RxPath when FCP response arrives
    /// (block write to our response CSR from target device)
    ///
    /// @param srcNodeID Source node ID
    /// @param generation Generation number
    /// @param payload Response payload
    void OnFCPResponse(uint16_t srcNodeID,
                       uint32_t generation,
                       std::span<const uint8_t> payload);

    /// Called by BusResetCoordinator when bus reset occurs
    ///
    /// @param newGeneration New generation number
    void OnBusReset(uint32_t newGeneration);

    /// Get current configuration
    const FCPTransportConfig& GetConfig() const { return config_; }

    /// Get async subsystem reference (for PCRSpace)
    Async::AsyncSubsystem& GetAsyncSubsystem() { return async_; }

private:
    //==========================================================================
    // Outstanding Command State
    //==========================================================================

    struct OutstandingCommand {
        FCPFrame command;               ///< Original command frame
        FCPCompletion completion;       ///< User completion callback
        uint32_t generation;            ///< Generation when submitted
        uint8_t retriesLeft;            ///< Retries remaining
        bool allowBusResetRetry;        ///< Allow retry after bus reset
        bool gotInterim{false};         ///< Received interim response

        Async::AsyncHandle asyncHandle; ///< Handle for async write
        uint64_t timeoutToken{0};       ///< Token for timeout validation
    };

    //==========================================================================
    // Internal Methods
    //==========================================================================

    /// Async write completion callback
    void OnAsyncWriteComplete(Async::AsyncHandle handle,
                              Async::AsyncStatus status,
                              std::span<const uint8_t> response);

    /// Submit async write through AsyncSubsystem
    Async::AsyncHandle SubmitWriteCommand(const FCPFrame& frame);

    /// Timeout handler (called on timeout queue)
    void OnCommandTimeout();

    /// Retry command after failure
    void RetryCommand();

    /// Response validation
    bool ValidateResponse(std::span<const uint8_t> response) const;

    /// Complete command and invoke user completion
    void CompleteCommand(FCPStatus status, const FCPFrame& response);

    /// Schedule timeout
    void ScheduleTimeout(uint32_t timeoutMs);

    /// Cancel timeout
    void CancelTimeout();

    //==========================================================================
    // Member Variables
    //==========================================================================

    Async::AsyncSubsystem& async_;
    Discovery::FWDevice& device_;
    FCPTransportConfig config_;

    /// Lock protecting pending_ (DriverKit IOLock, not os_unfair_lock)
    IOLock* lock_{nullptr};

    /// Dedicated DriverKit dispatch queue for timeout handling
    OSSharedPtr<IODispatchQueue> timeoutQueue_;

    /// Timer source for timeouts (replaces IOSleep)
    OSSharedPtr<IOTimerDispatchSource> timer_;

    /// Unique token generator for timeout validation
    uint64_t nextTimeoutToken_{0};

    /// Set to true while tearing down to prevent new timers
    bool shuttingDown_{false};

    /// Single outstanding command (v1 concurrency model)
    std::unique_ptr<OutstandingCommand> pending_;

    /// Transaction ID (dummy, always 1 since only one command at a time)
    static constexpr uint32_t kTransactionID = 1;
};

} // namespace ASFW::Protocols::AVC
