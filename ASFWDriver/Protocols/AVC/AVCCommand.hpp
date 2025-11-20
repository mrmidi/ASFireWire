//
// AVCCommand.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C command abstraction - builds on FCP transport layer
// Provides CDB encode/decode and command execution
//

#pragma once

#include <DriverKit/IOLib.h>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include "AVCDefs.hpp"
#include "FCPTransport.hpp"
#include "../../Logging/Logging.hpp"

// Forward declarations for dispatch types (implementation uses libdispatch)
typedef struct dispatch_semaphore_s* dispatch_semaphore_t;

namespace ASFW::Protocols::AVC {

//==============================================================================
// AV/C Command Descriptor Block (CDB)
//==============================================================================

/// AV/C Command Descriptor Block
///
/// Represents an AV/C command frame with structured access to:
/// - ctype: Command type (CONTROL, STATUS, INQUIRY, NOTIFY)
/// - subunit: Subunit address (unit = 0xFF)
/// - opcode: Command opcode
/// - operands: Command-specific data
///
/// **Wire Format** (IEC 61883 / AV/C spec):
/// ```
/// Byte[0]: ctype (command type / response type)
/// Byte[1]: subunit address (type[7:3] | id[2:0])
/// Byte[2]: opcode
/// Byte[3+]: operands (0-509 bytes)
/// ```
struct AVCCdb {
    uint8_t ctype{0};                        ///< Command type / response type
    uint8_t subunit{kAVCSubunitUnit};        ///< Subunit address (0xFF = unit)
    uint8_t opcode{0};                       ///< Command opcode
    std::array<uint8_t, kAVCOperandMaxLength> operands{}; ///< Operands
    size_t operandLength{0};                 ///< Operand length (0-509)

    /// Encode CDB to FCP frame
    ///
    /// @return FCP frame ready for transmission (3-512 bytes)
    FCPFrame Encode() const {
        FCPFrame frame;
        frame.data[0] = ctype;
        frame.data[1] = subunit;
        frame.data[2] = opcode;

        if (operandLength > 0) {
            std::copy_n(operands.begin(), operandLength,
                        frame.data.begin() + 3);
        }

        frame.length = 3 + operandLength;
        return frame;
    }

    /// Decode FCP frame to CDB
    ///
    /// @param frame FCP response frame
    /// @return Decoded CDB, or nullopt if invalid
    static std::optional<AVCCdb> Decode(const FCPFrame& frame) {
        if (!frame.IsValid()) {
            return std::nullopt;
        }

        AVCCdb cdb;
        cdb.ctype = frame.data[0];
        cdb.subunit = frame.data[1];
        cdb.opcode = frame.data[2];

        if (frame.length > 3) {
            cdb.operandLength = std::min(frame.length - 3,
                                         kAVCOperandMaxLength);
            std::copy_n(frame.data.begin() + 3, cdb.operandLength,
                        cdb.operands.begin());
        } else {
            cdb.operandLength = 0;
        }

        return cdb;
    }

    /// Validate CDB structure
    ///
    /// @return true if CDB is well-formed
    bool IsValid() const {
        return operandLength <= kAVCOperandMaxLength;
    }
};

//==============================================================================
// AV/C Completion Callback
//==============================================================================

/// AV/C command completion callback
///
/// @param result Command result (success, error, timeout, etc.)
/// @param response Response CDB (valid only if IsSuccess(result))
using AVCCompletion = std::function<void(AVCResult result,
                                         const AVCCdb& response)>;

//==============================================================================
// AV/C Command (Base Class)
//==============================================================================

/// Base AV/C Command
///
/// Wraps FCP transport and provides AV/C-specific:
/// - CDB encoding/decoding
/// - Response type mapping (ctype → AVCResult)
/// - FCP status mapping (timeout, bus reset, etc.)
///
/// **Usage** (async):
/// ```cpp
/// AVCCdb cdb;
/// cdb.ctype = static_cast<uint8_t>(AVCCommandType::kStatus);
/// cdb.subunit = kAVCSubunitUnit;
/// cdb.opcode = static_cast<uint8_t>(AVCOpcode::kPlugInfo);
/// cdb.operands[0] = 0xFF;
/// cdb.operandLength = 1;
///
/// auto cmd = std::make_shared<AVCCommand>(transport, cdb);
/// cmd->Submit([](AVCResult result, const AVCCdb& response) {
///     if (IsSuccess(result)) {
///         // Process response operands...
///     }
/// });
/// ```
class AVCCommand {
public:
    /// Constructor
    ///
    /// @param transport FCP transport layer
    /// @param cdb Command descriptor block
    AVCCommand(FCPTransport& transport, const AVCCdb& cdb)
        : transport_(transport), cdb_(cdb) {}

    virtual ~AVCCommand() = default;

    /// Submit command (async)
    ///
    /// Encodes CDB to FCP frame and submits to transport.
    /// Completion callback invoked when response received or error occurs.
    ///
    /// @param completion Callback with result and response CDB
    void Submit(AVCCompletion completion) {
        if (!cdb_.IsValid()) {
            completion(AVCResult::kInvalidResponse, cdb_);
            return;
        }

        FCPFrame frame = cdb_.Encode();

        fcpHandle_ = transport_.SubmitCommand(frame,
            [this, completion](FCPStatus fcpStatus, const FCPFrame& response) {
                OnFCPComplete(fcpStatus, response, completion);
            });
    }

    /// Cancel command
    ///
    /// Attempts to cancel outstanding FCP command.
    /// Completion callback will be invoked with kTransportError if successful.
    void Cancel() {
        if (fcpHandle_.IsValid()) {
            transport_.CancelCommand(fcpHandle_);
            fcpHandle_.Invalidate();
        }
    }

    /// Get original CDB
    const AVCCdb& GetCdb() const { return cdb_; }

protected:
    /// FCP completion handler (virtual for extensibility)
    ///
    /// Maps FCP status → AVCResult and decodes response CDB.
    ///
    /// @param fcpStatus FCP transport status
    /// @param response FCP response frame
    /// @param completion User completion callback
    virtual void OnFCPComplete(FCPStatus fcpStatus,
                               const FCPFrame& response,
                               AVCCompletion completion) {
        // Handle FCP-level errors
        if (fcpStatus != FCPStatus::kOk) {
            AVCResult result = MapFCPStatus(fcpStatus);
            completion(result, cdb_);
            return;
        }

        // Decode AV/C response
        auto responseCdb = AVCCdb::Decode(response);
        if (!responseCdb) {
            completion(AVCResult::kInvalidResponse, cdb_);
            return;
        }

        // Map ctype to result
        AVCResult result = CTypeToResult(responseCdb->ctype);
        completion(result, *responseCdb);
    }

    /// Map FCP status to AV/C result
    ///
    /// @param status FCP transport status
    /// @return Corresponding AVCResult
    AVCResult MapFCPStatus(FCPStatus status) {
        switch (status) {
            case FCPStatus::kOk:
                return AVCResult::kAccepted;  // Should not reach here
            case FCPStatus::kTimeout:
                return AVCResult::kTimeout;
            case FCPStatus::kBusReset:
                return AVCResult::kBusReset;
            case FCPStatus::kBusy:
                return AVCResult::kBusy;
            default:
                return AVCResult::kTransportError;
        }
    }

    FCPTransport& transport_;
    AVCCdb cdb_;
    FCPHandle fcpHandle_;
};

//==============================================================================
// AV/C Command (Synchronous Variant)
//==============================================================================

/// Synchronous AV/C command
///
/// Blocks calling thread until response received or timeout expires.
/// Uses dispatch_semaphore for blocking.
///
/// **Usage**:
/// ```cpp
/// AVCCdb cdb;
/// cdb.ctype = static_cast<uint8_t>(AVCCommandType::kStatus);
/// cdb.subunit = kAVCSubunitUnit;
/// cdb.opcode = static_cast<uint8_t>(AVCOpcode::kPlugInfo);
/// cdb.operands[0] = 0xFF;
/// cdb.operandLength = 1;
///
/// AVCCommandSync cmd(transport, cdb);
/// AVCCdb response;
/// AVCResult result = cmd.SubmitAndWait(response, 5000);  // 5s timeout
///
/// if (IsSuccess(result)) {
///     uint8_t numDestPlugs = response.operands[0];
///     uint8_t numSrcPlugs = response.operands[1];
/// }
/// ```
///
/// **Thread Safety**:
/// - Safe to call from UserClient ExternalMethod handlers
/// - Do NOT call from FCP completion queue or timeout queue (will deadlock)
/// - Completion callback runs on FCP timeout queue (different from caller)
class AVCCommandSync : public AVCCommand {
public:
    using AVCCommand::AVCCommand;

    /// Submit and wait for response (blocking)
    ///
    /// Blocks calling thread until:
    /// - Response received (returns result from ctype)
    /// - Timeout expires (returns kTimeout)
    ///
    /// @param outResponse Output response CDB (valid if IsSuccess(result))
    /// @param timeoutMs Maximum wait time (milliseconds)
    /// @return Command result
    ///
    /// TODO: Implement using DriverKit-compatible synchronization
    /// (IOLock + condition variable or callback-based waiting mechanism)
    /// DriverKit doesn't support dispatch_semaphore_t from libdispatch
    AVCResult SubmitAndWait(AVCCdb& outResponse,
                            uint32_t timeoutMs = 10000) {
        // TEMPORARILY STUBBED - libdispatch not available in DriverKit
        (void)outResponse;
        (void)timeoutMs;
        ASFW_LOG_ERROR(Async,
                       "AVCCommand::SubmitAndWait() not yet implemented for DriverKit");
        return AVCResult::kTransportError;
    }
};

} // namespace ASFW::Protocols::AVC
