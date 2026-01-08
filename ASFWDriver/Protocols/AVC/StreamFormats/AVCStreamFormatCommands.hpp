//
// AVCStreamFormatCommands.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Stream Format Commands (opcode 0xBF/0x2F with subfunctions)
// Refactored to use StreamFormatParser for response parsing
//
// Reference: TA Document 2001002 - AV/C Stream Format Information Specification
// Reference: FWA/src/FWA/PlugDetailParser.cpp:105-202
//

#pragma once

#include "../AVCCommand.hpp"
#include "../IAVCCommandSubmitter.hpp"
#include "StreamFormatTypes.hpp"
#include "StreamFormatParser.hpp"
#include <vector>
#include <optional>
#include <functional>

namespace ASFW::Protocols::AVC::StreamFormats {

//==============================================================================
// Stream Format Command Constants
//==============================================================================

/// Stream format subfunctions
constexpr uint8_t kStreamFormatSubfunc_Current = 0xC0;
constexpr uint8_t kStreamFormatSubfunc_Supported = 0xC1;

/// Stream format opcodes (try 0xBF first, fallback to 0x2F)
constexpr uint8_t kStreamFormatOpcode_Primary = 0xBF;
constexpr uint8_t kStreamFormatOpcode_Alternate = 0x2F;

//==============================================================================
// Stream Format Query Command
//==============================================================================

/// Query current or supported stream formats for a plug
/// Handles both STREAM FORMAT SUPPORT (0xBF) and alternate opcode (0x2F)
/// Query current or supported stream formats for a plug
/// Handles both STREAM FORMAT SUPPORT (0xBF) and alternate opcode (0x2F)
class AVCStreamFormatCommand {
public:
    //==========================================================================
    // Constructors
    //==========================================================================

    /// Constructor for querying current format
    /// @param submitter Command submitter
    /// @param subunitAddr Subunit address (0xFF for unit plugs)
    /// @param plugNum Plug number
    /// @param isInput true for input/destination plug, false for output/source plug
    /// @param useAlternateOpcode true to use 0x2F instead of 0xBF
    AVCStreamFormatCommand(IAVCCommandSubmitter& submitter,
                          uint8_t subunitAddr,
                          uint8_t plugNum,
                          bool isInput,
                          bool useAlternateOpcode = false)
        : submitter_(submitter)
        , cdb_(BuildCdb(subunitAddr, plugNum, isInput,
                       kStreamFormatSubfunc_Current, 0xFF,
                       useAlternateOpcode))
        , isListQuery_(false) {}

    /// Constructor for querying supported formats
    /// @param submitter Command submitter
    /// @param subunitAddr Subunit address (0xFF for unit plugs)
    /// @param plugNum Plug number
    /// @param isInput true for input/destination plug, false for output/source plug
    /// @param listIndex Index in supported format list (0-based)
    /// @param useAlternateOpcode true to use 0x2F instead of 0xBF
    AVCStreamFormatCommand(IAVCCommandSubmitter& submitter,
                          uint8_t subunitAddr,
                          uint8_t plugNum,
                          bool isInput,
                          uint8_t listIndex,
                          bool useAlternateOpcode = false)
        : submitter_(submitter)
        , cdb_(BuildCdb(subunitAddr, plugNum, isInput,
                       kStreamFormatSubfunc_Supported, listIndex,
                       useAlternateOpcode))
        , isListQuery_(true) {}

    /// Constructor for setting format
    /// @param submitter Command submitter
    /// @param subunitAddr Subunit address (0xFF for unit plugs)
    /// @param plugNum Plug number
    /// @param isInput true for input/destination plug, false for output/source plug
    /// @param format Format to set
    /// @param useAlternateOpcode true to use 0x2F instead of 0xBF
    AVCStreamFormatCommand(IAVCCommandSubmitter& submitter,
                          uint8_t subunitAddr,
                          uint8_t plugNum,
                          bool isInput,
                          const AudioStreamFormat& format,
                          bool useAlternateOpcode = false)
        : submitter_(submitter)
        , cdb_(BuildCdb(subunitAddr, plugNum, isInput,
                       kStreamFormatSubfunc_Current, 0xFF,
                       useAlternateOpcode, &format))
        , isListQuery_(false) {}

    //==========================================================================
    // Command Submission
    //==========================================================================

    /// Submit command with parsed format response
    /// Uses StreamFormatParser for robust parsing
    void Submit(std::function<void(AVC::AVCResult, const std::optional<AudioStreamFormat>&)> completion) {
        submitter_.SubmitCommand(cdb_, [this, completion](AVC::AVCResult result, const AVC::AVCCdb& response) {
            if (AVC::IsSuccess(result)) {
                auto format = ParseFormatResponse(response);
                completion(result, format);
            } else {
                completion(result, std::nullopt);
            }
        });
    }

private:
    IAVCCommandSubmitter& submitter_;
    AVC::AVCCdb cdb_;
    bool isListQuery_; ///< true if querying supported formats list

    //==========================================================================
    // CDB Building
    //==========================================================================

    static AVC::AVCCdb BuildCdb(uint8_t subunitAddr, uint8_t plugNum, bool isInput,
                                uint8_t subfunction, uint8_t listIndex, bool useAlternateOpcode,
                                const AudioStreamFormat* formatToSet = nullptr) {
        AVC::AVCCdb cdb;
        // If setting format, use CONTROL, otherwise STATUS
        cdb.ctype = formatToSet ? static_cast<uint8_t>(AVC::AVCCommandType::kControl) 
                                : static_cast<uint8_t>(AVC::AVCCommandType::kStatus);
        cdb.subunit = subunitAddr;
        cdb.opcode = useAlternateOpcode ? kStreamFormatOpcode_Alternate : kStreamFormatOpcode_Primary;

        size_t offset = 0;
        cdb.operands[offset++] = subfunction; // 0xC0 or 0xC1
        cdb.operands[offset++] = isInput ? 0x00 : 0x01; // plug_direction

        if (subunitAddr == 0xFF) {
            // Unit plugs (isochronous or external)
            uint8_t plugType = (plugNum < 0x80) ? 0x00 : 0x01; // 0=Iso, 1=External
            cdb.operands[offset++] = plugType;
            cdb.operands[offset++] = plugType;
            cdb.operands[offset++] = plugNum;
            cdb.operands[offset++] = 0xFF; // format_info_label
            if (subfunction == kStreamFormatSubfunc_Supported) {
                cdb.operands[offset++] = 0xFF; // reserved
                cdb.operands[offset++] = listIndex;
            }
        } else {
            // Subunit plugs
            // Per TA 2001002 and FWA reference: subunit plug format command layout:
            //   operands[0]: subfunction (0xC0/0xC1)
            //   operands[1]: plug_direction
            //   operands[2]: plug_type (0x01 = subunit plug)
            //   operands[3]: subunit_plug_ID
            //   operands[4]: format_info_label (0xFF)
            //   operands[5]: reserved (0xFF)
            //   operands[6]: reserved (0xFF) - for C1 only
            //   operands[7]: list_index - for C1 only
            cdb.operands[offset++] = 0x01; // plug_type = subunit plug
            cdb.operands[offset++] = plugNum;
            cdb.operands[offset++] = 0xFF; // format_info_label
            cdb.operands[offset++] = 0xFF; // reserved
            if (subfunction == kStreamFormatSubfunc_Supported) {
                cdb.operands[offset++] = 0xFF; // reserved (was missing!)
                cdb.operands[offset++] = listIndex;
            }
        }

        // Append format data if setting
        if (formatToSet) {
            // Serialize format
            // Assumes AM824 for now as that's what we use
            // TODO: Make this generic based on format type
            
            // AM824 Compound Format
            cdb.operands[offset++] = 0x90; // AM824
            cdb.operands[offset++] = 0x40; // Compound
            cdb.operands[offset++] = static_cast<uint8_t>(formatToSet->sampleRate); // Rate code
            cdb.operands[offset++] = 0x00; // Sync (internal)
            cdb.operands[offset++] = static_cast<uint8_t>(formatToSet->channelFormats.size()); // Num channels
            
            // Channel formats?
            // For SetSampleRate, we might just send the header?
            // Or we need to send the full format?
            // The spec says "The format_information_field shall contain the new format."
            // If we are just changing sample rate, we should probably preserve other fields,
            // but here we are constructing a new format.
            // For now, we just send the header as implemented above.
        }

        cdb.operandLength = offset;
        return cdb;
    }

    //==========================================================================
    // Response Parsing
    //==========================================================================

    std::optional<AudioStreamFormat> ParseFormatResponse(const AVC::AVCCdb& response) const {
        // Format block starts after command header
        // For 0xC0 (current): offset 7 (unit) or 6 (subunit)
        // For 0xC1 (supported): offset 8 (unit) or 7 (subunit)

        if (response.operandLength < 3) {
            return std::nullopt;
        }

        // Determine format block offset based on subfunction
        // Per TA 2001002 and FWA reference (PlugDetailParser.cpp:260-269):
        // - C0 (current format): format starts at wire byte 10 = operands[7]
        // - C1 (supported format): format starts at wire byte 11 = operands[8]
        // Note: FWA uses the SAME offset for both unit and subunit plugs, despite
        // different operand structures in the command. The response structure is consistent.
        uint8_t subfunction = response.operands[0];
        size_t formatOffset = 0;

        if (subfunction == kStreamFormatSubfunc_Current) {
            formatOffset = 7;  // Wire byte 10 = operands[7]
        } else if (subfunction == kStreamFormatSubfunc_Supported) {
            formatOffset = 8;  // Wire byte 11 = operands[8]
        } else {
            return std::nullopt;
        }

        if (response.operandLength <= formatOffset) {
            return std::nullopt;
        }

        // Use StreamFormatParser for robust parsing
        size_t formatLength = response.operandLength - formatOffset;
        return StreamFormatParser::Parse(response.operands.data() + formatOffset, formatLength);
    }
};

//==============================================================================
// Helper Function for Querying Supported Formats List
//==============================================================================

/// Query all supported formats for a plug by iterating list indices
/// @param submitter Command submitter
/// @param subunitAddr Subunit address
/// @param plugNum Plug number
/// @param isInput true for input plug
/// @param maxIterations Maximum list indices to try (default 16 per spec)
/// @param completion Callback with vector of all supported formats
inline void QueryAllSupportedFormats(
    IAVCCommandSubmitter& submitter,
    uint8_t subunitAddr,
    uint8_t plugNum,
    bool isInput,
    std::function<void(std::vector<AudioStreamFormat>)> completion,
    uint8_t maxIterations = 16
) {
    auto formats = std::make_shared<std::vector<AudioStreamFormat>>();
    auto iteration = std::make_shared<uint8_t>(0);

    // Recursive lambda for iteration
    // Use shared_ptr to allow capturing itself
    auto queryNext = std::make_shared<std::function<void()>>();
    
    *queryNext = [&submitter, subunitAddr, plugNum, isInput, maxIterations, formats, iteration, completion, queryNext]() {
        if (*iteration >= maxIterations) {
            completion(*formats);
            return;
        }

        auto cmd = std::make_shared<AVCStreamFormatCommand>(
            submitter, subunitAddr, plugNum, isInput, *iteration
        );

        cmd->Submit([formats, iteration, completion, queryNext](
            AVC::AVCResult result,
            const std::optional<AudioStreamFormat>& format
        ) {
            if (AVC::IsSuccess(result) && format) {
                formats->push_back(*format);
                (*iteration)++;
                (*queryNext)(); // Query next format
            } else {
                // No more formats or error - done
                completion(*formats);
            }
        });
    };

    (*queryNext)();
}

} // namespace ASFW::Protocols::AVC::StreamFormats
