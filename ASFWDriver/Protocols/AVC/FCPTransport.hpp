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
#include <DriverKit/OSObject.h>
#include <DriverKit/IODispatchQueue.h>
#endif
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

class FCPTransport : public OSObject {
private:
    using super = OSObject;
public:
    FCPTransport() = default;
    virtual ~FCPTransport() = default;

    void* operator new(size_t size) { return IOMallocZero(size); }
    void operator delete(void* ptr, size_t size) { IOFree(ptr, size); }

    virtual bool init() override { return super::init(); }

    virtual bool init(Async::AsyncSubsystem* async,
                      Discovery::FWDevice* device,
                      const FCPTransportConfig& config = {});

    virtual void free() override;

    FCPTransport(const FCPTransport&) = delete;
    FCPTransport& operator=(const FCPTransport&) = delete;

    [[nodiscard]] FCPHandle SubmitCommand(const FCPFrame& command,
                                          FCPCompletion completion);

    bool CancelCommand(FCPHandle handle);

    void OnFCPResponse(uint16_t srcNodeID,
                       uint32_t generation,
                       std::span<const uint8_t> payload);

    void OnBusReset(uint32_t newGeneration);

    const FCPTransportConfig& GetConfig() const { return config_; }

    Async::AsyncSubsystem& GetAsyncSubsystem() { return *async_; }

private:
    struct OutstandingCommand {
        FCPFrame command;
        FCPCompletion completion;
        uint32_t generation;
        uint8_t retriesLeft;
        bool allowBusResetRetry;
        bool gotInterim{false};

        Async::AsyncHandle asyncHandle;
        uint64_t timeoutToken{0};
    };

    void OnAsyncWriteComplete(Async::AsyncHandle handle,
                              Async::AsyncStatus status,
                              uint8_t responseCode,
                              std::span<const uint8_t> response);

    Async::AsyncHandle SubmitWriteCommand(const FCPFrame& frame);

    void OnCommandTimeout();

    void RetryCommand();

    bool ValidateResponse(std::span<const uint8_t> response) const;

    void CompleteCommand(FCPStatus status, const FCPFrame& response);

    void ScheduleTimeout(uint32_t timeoutMs);

    void CancelTimeout();

    Async::AsyncSubsystem* async_;
    Discovery::FWDevice* device_;
    FCPTransportConfig config_;

    IOLock* lock_{nullptr};

    OSSharedPtr<IODispatchQueue> timeoutQueue_;

    uint64_t nextTimeoutToken_{0};

    bool shuttingDown_{false};

    std::unique_ptr<OutstandingCommand> pending_;

    static constexpr uint32_t kTransactionID = 1;
};

} // namespace ASFW::Protocols::AVC
