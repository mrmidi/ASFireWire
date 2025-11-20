//
// AVCCommands.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Specific AV/C command implementations
// - PLUG_INFO: Query plug count
// - SUBUNIT_INFO: Enumerate subunits
//

#pragma once

#include "AVCCommand.hpp"
#include <vector>

namespace ASFW::Protocols::AVC {

//==============================================================================
// PLUG_INFO Command (0x02)
//==============================================================================

/// PLUG_INFO command (opcode 0x02)
///
/// Queries the number of input/output plugs on a unit or subunit.
///
/// **AV/C Spec**:
/// - Command: [STATUS, subunit, 0x02, 0xFF]
/// - Response: [IMPLEMENTED/STABLE, subunit, 0x02, numDest, numSrc]
///
/// **Example** (Duet):
/// - Command:  [0x01, 0xFF, 0x02, 0xFF]
/// - Response: [0x0C, 0xFF, 0x02, 0x02, 0x02]
///   → 2 destination (input) plugs, 2 source (output) plugs
class AVCPlugInfoCommand : public AVCCommand {
public:
    /// Plug info response data
    struct PlugInfo {
        uint8_t numDestPlugs{0};  ///< Destination (input) plugs
        uint8_t numSrcPlugs{0};    ///< Source (output) plugs
    };

    /// Constructor
    ///
    /// @param transport FCP transport
    /// @param subunitType Subunit type (0xFF = unit, default)
    /// @param subunitID Subunit ID (0-7, default 0)
    AVCPlugInfoCommand(FCPTransport& transport,
                       uint8_t subunitType = kAVCSubunitUnit,
                       uint8_t subunitID = 0)
        : AVCCommand(transport, BuildCdb(subunitType, subunitID)) {}

    /// Submit and parse response
    ///
    /// @param completion Callback with result and parsed plug info
    void Submit(std::function<void(AVCResult, const PlugInfo&)> completion) {
        AVCCommand::Submit([this, completion](AVCResult result,
                                               const AVCCdb& response) {
            if (IsSuccess(result)) {
                PlugInfo info = ParseResponse(response);
                completion(result, info);
            } else {
                completion(result, {});
            }
        });
    }

private:
    /// Build PLUG_INFO CDB
    ///
    /// @param subunitType Subunit type (0xFF = unit)
    /// @param subunitID Subunit ID (0-7)
    /// @return Command CDB
    static AVCCdb BuildCdb(uint8_t subunitType, uint8_t subunitID) {
        AVCCdb cdb;
        cdb.ctype = static_cast<uint8_t>(AVCCommandType::kStatus);

        if (subunitType == kAVCSubunitUnit) {
            cdb.subunit = kAVCSubunitUnit;
        } else {
            cdb.subunit = MakeSubunitAddress(
                static_cast<AVCSubunitType>(subunitType),
                subunitID
            );
        }

        cdb.opcode = static_cast<uint8_t>(AVCOpcode::kPlugInfo);
        cdb.operands[0] = 0xFF;  // Query all
        cdb.operandLength = 1;

        return cdb;
    }

    /// Parse PLUG_INFO response
    ///
    /// @param response Response CDB
    /// @return Parsed plug info
    PlugInfo ParseResponse(const AVCCdb& response) {
        PlugInfo info;

        if (response.operandLength >= 2) {
            info.numDestPlugs = response.operands[0];
            info.numSrcPlugs = response.operands[1];
        }

        return info;
    }
};

//==============================================================================
// SUBUNIT_INFO Command (0x31)
//==============================================================================

/// SUBUNIT_INFO command (opcode 0x31)
///
/// Enumerates subunits present in the unit.
///
/// **AV/C Spec**:
/// - Command: [STATUS, unit, 0x31, page]
/// - Response: [IMPLEMENTED/STABLE, unit, 0x31, subunit_entries...]
///
/// Each response contains up to 4 subunit entries (1 byte each):
/// - Byte[i] = subunit_type[7:3] | max_subunit_ID[2:0]
/// - 0xFF = no subunit
///
/// **Example**:
/// - Command:  [0x01, 0xFF, 0x31, 0x07]  // Page 0
/// - Response: [0x0C, 0xFF, 0x31, 0xE0, 0xFF, 0xFF, 0xFF]
///   → Music subunit (0x1C) with ID 0, no other subunits
class AVCSubunitInfoCommand : public AVCCommand {
public:
    /// Subunit entry
    struct SubunitEntry {
        uint8_t type{0xFF};        ///< Subunit type (0xFF = no subunit)
        uint8_t maxID{0};          ///< Maximum subunit ID for this type
    };

    /// Subunit info response data
    struct SubunitInfo {
        std::vector<SubunitEntry> subunits;
    };

    /// Constructor
    ///
    /// @param transport FCP transport
    /// @param page Page number (0 = first page, usually sufficient)
    AVCSubunitInfoCommand(FCPTransport& transport, uint8_t page = 0)
        : AVCCommand(transport, BuildCdb(page)) {}

    /// Submit and parse response
    ///
    /// @param completion Callback with result and parsed subunit info
    void Submit(std::function<void(AVCResult, const SubunitInfo&)> completion) {
        AVCCommand::Submit([this, completion](AVCResult result,
                                               const AVCCdb& response) {
            if (IsSuccess(result)) {
                SubunitInfo info = ParseResponse(response);
                completion(result, info);
            } else {
                completion(result, {});
            }
        });
    }

private:
    /// Build SUBUNIT_INFO CDB
    ///
    /// @param page Page number
    /// @return Command CDB
    static AVCCdb BuildCdb(uint8_t page) {
        AVCCdb cdb;
        cdb.ctype = static_cast<uint8_t>(AVCCommandType::kStatus);
        cdb.subunit = kAVCSubunitUnit;
        cdb.opcode = static_cast<uint8_t>(AVCOpcode::kSubunitInfo);
        cdb.operands[0] = (page << 4) | 0x07;  // Page | extension code
        cdb.operands[1] = 0xFF;
        cdb.operands[2] = 0xFF;
        cdb.operands[3] = 0xFF;
        cdb.operands[4] = 0xFF;
        cdb.operandLength = 5;

        return cdb;
    }

    /// Parse SUBUNIT_INFO response
    ///
    /// @param response Response CDB
    /// @return Parsed subunit info
    SubunitInfo ParseResponse(const AVCCdb& response) {
        SubunitInfo info;

        // Response format: [page, entry0, entry1, entry2, entry3]
        // Each entry: subunit_type[7:3] | max_ID[2:0]

        if (response.operandLength < 2) {
            return info;
        }

        // Parse subunit entries (up to 4 per page)
        for (size_t i = 1; i < response.operandLength && i < 5; i++) {
            uint8_t entry = response.operands[i];

            if (entry == 0xFF) {
                // No subunit
                continue;
            }

            SubunitEntry subunit;
            subunit.type = (entry >> 3) & 0x1F;
            subunit.maxID = entry & 0x07;

            info.subunits.push_back(subunit);
        }

        return info;
    }
};

//==============================================================================
// Helper: Get Subunit Type Name
//==============================================================================

/// Get human-readable subunit type name
///
/// @param type Subunit type code
/// @return Type name string
inline const char* GetSubunitTypeName(uint8_t type) {
    switch (static_cast<AVCSubunitType>(type)) {
        case AVCSubunitType::kVideoMonitor:
            return "Video Monitor";
        case AVCSubunitType::kAudio:
            return "Audio";
        case AVCSubunitType::kTapeRecorder:
            return "Tape Recorder";
        case AVCSubunitType::kTuner:
            return "Tuner";
        case AVCSubunitType::kCA:
            return "CA";
        case AVCSubunitType::kCamera:
            return "Camera";
        case AVCSubunitType::kPanel:
            return "Panel";
        case AVCSubunitType::kBulletinBoard:
            return "Bulletin Board";
        case AVCSubunitType::kCameraStorage:
            return "Camera Storage";
        case AVCSubunitType::kMusic:
            return "Music";
        case AVCSubunitType::kUnit:
            return "Unit";
        default:
            return "Unknown";
    }
}

} // namespace ASFW::Protocols::AVC
