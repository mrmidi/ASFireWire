//
// AVCCommand.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C command abstraction - builds on FCP transport layer
// Provides CDB encode/decode and command execution
//

#pragma once

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#endif
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include "AVCDefs.hpp"
#include "FCPTransport.hpp"
#include "../../Common/CallbackUtils.hpp"
#include "../../Logging/Logging.hpp"

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

        // IEC 61883-1:2008 §9.3.1 Figure 32: FCP frames require zero padding
        // to align with IEEE 1394 quadlet (4-byte) boundaries for block writes.
        // Unpadded frames violate IEEE 1394 alignment rules and cause transaction failures.
        size_t unpaddedLength = 3 + operandLength;
        size_t paddedLength = (unpaddedLength + 3) & ~3;  // Round up to nearest 4 bytes
        
        // Zero out padding bytes (required by spec)
        if (paddedLength > unpaddedLength) {
            std::fill(frame.data.begin() + unpaddedLength,
                     frame.data.begin() + paddedLength, 0);
        }
        
        frame.length = paddedLength;
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
class AVCCommand : public std::enable_shared_from_this<AVCCommand> {
public:
    /// Constructor
    ///
    /// @param transport FCP transport layer
    /// @param cdb Command descriptor block
    AVCCommand(FCPTransport& transport, const AVCCdb& cdb)
        : transport_(transport), cdb_(cdb) {}

    virtual ~AVCCommand() = default;

    /// Submit command (async) through the unit's transaction engine.
    ///
    /// Completion callback invoked when response received or error occurs.
    ///
    /// @param completion Callback with result and response CDB
    void Submit(AVCCompletion completion) {
        auto completionState = Common::ShareCallback(std::move(completion));
        if (!cdb_.IsValid()) {
            Common::InvokeSharedCallback(completionState, AVCResult::kInvalidResponse, cdb_);
            return;
        }
        auto frame = ASFW::AVC::CommandFrame::Make(
            static_cast<ASFW::AVC::CommandType>(cdb_.ctype & 0x0F),
            ASFW::AVC::SubunitAddress::FromByte(cdb_.subunit),
            static_cast<ASFW::AVC::Opcode>(cdb_.opcode),
            std::span<const uint8_t>(cdb_.operands.data(), cdb_.operandLength));
        if (!frame) {
            Common::InvokeSharedCallback(completionState, AVCResult::kInvalidResponse, cdb_);
            return;
        }

        // Use weak_from_this() to check ownership without throwing bad_weak_ptr
        auto self = weak_from_this().lock();
        if (!self) {
             ASFW_LOG(AVC, "AVCCommand::Submit called without shared ownership - command dropped");
             Common::InvokeSharedCallback(completionState, AVCResult::kTransportError, cdb_);
             return;
        }
        transport_.Submit(*frame, transport_.CurrentGeneration(),
            [self, completionState](ASFW::AVC::Expected<ASFW::AVC::Response> response) {
                self->OnResponse(response, completionState);
            });
    }

    /// Get original CDB
    const AVCCdb& GetCdb() const { return cdb_; }

protected:
    /// Map the engine's result to an AVCResult and the response CDB.
    virtual void OnResponse(const ASFW::AVC::Expected<ASFW::AVC::Response>& response,
                            const std::shared_ptr<AVCCompletion>& completion) {
        if (!response) {
            Common::InvokeSharedCallback(completion, ResultFor(response.error()), cdb_);
            return;
        }
        AVCCdb responseCdb;
        responseCdb.ctype = static_cast<uint8_t>(response->code);
        responseCdb.subunit = response->address.Byte();
        responseCdb.opcode = static_cast<uint8_t>(response->opcode);
        responseCdb.operandLength = std::min(response->operands.size(), kAVCOperandMaxLength);
        std::copy_n(response->operands.begin(), responseCdb.operandLength, responseCdb.operands.begin());
        Common::InvokeSharedCallback(completion, CTypeToResult(responseCdb.ctype), responseCdb);
    }

    [[nodiscard]] static AVCResult ResultFor(const ASFW::AVC::AvcError& error) noexcept {
        switch (error.kind) {
            case ASFW::AVC::AvcErrorKind::kTimeout: return AVCResult::kTimeout;
            case ASFW::AVC::AvcErrorKind::kBusReset: return AVCResult::kBusReset;
            case ASFW::AVC::AvcErrorKind::kBusy: return AVCResult::kBusy;
            case ASFW::AVC::AvcErrorKind::kTransportError:
            case ASFW::AVC::AvcErrorKind::kRefused: return AVCResult::kTransportError;
            default: return AVCResult::kInvalidResponse;
        }
    }

    FCPTransport& transport_;
    AVCCdb cdb_;
};


} // namespace ASFW::Protocols::AVC
