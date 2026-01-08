//
// AVCSignalSourceCommand.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C SIGNAL SOURCE command (opcode 0x1A)
// Query connection topology - which source plug feeds a destination plug
//
// Reference: TA Document 1999008 - AV/C Digital Interface Command Set General Specification
// Reference: FWA/src/FWA/PlugDetailParser.cpp:204-255
//

#pragma once

#include "../IAVCCommandSubmitter.hpp"
#include "StreamFormatTypes.hpp"
#include <functional>

namespace ASFW::Protocols::AVC::StreamFormats {

//==============================================================================
// SIGNAL SOURCE Command (0x1A)
//==============================================================================

/// Query which source plug is connected to a destination plug
/// Used for discovering plug connection topology
///
/// Command format:
/// [ctype=STATUS] [subunit] [opcode=0x1A] [output_status] [conv_data]
/// [plug_type] [dest_plug] [FF FF FF FF FF]
///
/// Response format:
/// [response] [subunit] [opcode=0x1A] [output_status] [conv_data]
/// [source_plug_type] [source_plug] [dest_plug_type] [dest_plug] [...]
class AVCSignalSourceCommand {
public:
    /// Constructor for querying destination plug connection
    /// @param submitter Command submitter
    /// @param subunitAddr Subunit address
    /// @param destPlugNumber Destination plug number to query
    /// @param isSubunitPlug true for subunit plug, false for unit plug
    AVCSignalSourceCommand(IAVCCommandSubmitter& submitter,
                          uint8_t subunitAddr,
                          uint8_t destPlugNumber,
                          bool isSubunitPlug = true)
        : submitter_(submitter)
        , cdb_(BuildCdb(subunitAddr, destPlugNumber, isSubunitPlug)) {}

    /// Submit command with connection info response
    void Submit(std::function<void(AVC::AVCResult, const ConnectionInfo&)> completion) {
        submitter_.SubmitCommand(cdb_, [completion](AVC::AVCResult result, const AVC::AVCCdb& response) {
            if (AVC::IsSuccess(result)) {
                auto connInfo = ParseConnectionInfo(response);
                completion(result, connInfo);
            } else {
                completion(result, ConnectionInfo{});
            }
        });
    }

private:
    IAVCCommandSubmitter& submitter_;
    AVC::AVCCdb cdb_;

    static AVC::AVCCdb BuildCdb(uint8_t subunitAddr, uint8_t destPlugNumber, bool isSubunitPlug) {
        AVC::AVCCdb cdb;
        cdb.ctype = static_cast<uint8_t>(AVC::AVCCommandType::kStatus);
        cdb.subunit = subunitAddr;
        cdb.opcode = 0x1A; // SIGNAL SOURCE

        size_t offset = 0;

        // output_status: 0xFF (query)
        cdb.operands[offset++] = 0xFF;

        // conv_data: 0xFF FF (query)
        cdb.operands[offset++] = 0xFF;
        cdb.operands[offset++] = 0xFF;

        // Destination plug addressing
        if (isSubunitPlug) {
            // Subunit plug
            cdb.operands[offset++] = 0x00; // plug_type = subunit source plug
            cdb.operands[offset++] = destPlugNumber;
        } else {
            // Unit plug (isochronous or external)
            uint8_t plugType = (destPlugNumber < 0x80) ? 0x00 : 0x01;
            cdb.operands[offset++] = plugType;
            cdb.operands[offset++] = destPlugNumber;
        }

        // Query fields (filled with 0xFF)
        for (int i = 0; i < 5; i++) {
            cdb.operands[offset++] = 0xFF;
        }

        cdb.operandLength = offset;
        return cdb;
    }

    static ConnectionInfo ParseConnectionInfo(const AVC::AVCCdb& response) {
        ConnectionInfo info;

        // Response format (after opcode):
        // [0] = output_status
        // [1-2] = conv_data
        // [3] = source_plug_type
        // [4] = source_plug_number
        // [5] = dest_plug_type
        // [6] = dest_plug_number

        if (response.operandLength < 7) {
            // Not enough data
            return info;
        }

        // Parse source plug type
        uint8_t sourcePlugType = response.operands[3];
        uint8_t sourcePlugNumber = response.operands[4];

        // Check for "not connected" state (source = 0xFE)
        if (sourcePlugNumber == 0xFE) {
            info.sourceSubunitType = SourceSubunitType::kNotConnected;
            info.sourcePlugNumber = 0xFF;
            info.sourceSubunitID = 0xFF;
            return info;
        }

        // Parse source subunit type from plug type byte
        // For subunit plugs, the subunit type is encoded in the upper nibble
        if (sourcePlugType == 0x00) {
            // Subunit source plug
            // Need to determine subunit type - typically from subunit address in response
            // For Music Subunit, this is often 0x0C
            // For Audio Subunit, this is 0x01
            // For Unit, this is 0xFF
            info.sourceSubunitType = ParseSubunitType(response.subunit);
            info.sourceSubunitID = response.subunit & 0x07; // Lower 3 bits = ID
            info.sourcePlugNumber = sourcePlugNumber;
        } else {
            // Unit plug (isochronous or external)
            info.sourceSubunitType = SourceSubunitType::kUnit;
            info.sourceSubunitID = 0xFF;
            info.sourcePlugNumber = sourcePlugNumber;
        }

        return info;
    }

    static SourceSubunitType ParseSubunitType(uint8_t subunitAddr) {
        if (subunitAddr == 0xFF) {
            return SourceSubunitType::kUnit;
        }

        // Extract subunit type from upper 5 bits
        uint8_t type = (subunitAddr >> 3) & 0x1F;

        switch (type) {
            case 0x01: return SourceSubunitType::kAudio;
            case 0x0C: return SourceSubunitType::kMusic;
            default: return SourceSubunitType::kUnknown;
        }
    }
};

} // namespace ASFW::Protocols::AVC::StreamFormats
