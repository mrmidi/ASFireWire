//
// AVCStreamFormatCommand.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Stream Format Commands (opcode 0xBF/0x2F with subfunctions)
// Used to query current and supported stream formats for plugs
//

#pragma once

#include "AVCCommand.hpp"
#include <vector>
#include <optional>

namespace ASFW::Protocols::AVC {

//==============================================================================
// Stream Format Command (0xBF or 0x2F)
//==============================================================================

/// Stream format subfunctions
constexpr uint8_t kStreamFormatSubfunc_Current = 0xC0;
constexpr uint8_t kStreamFormatSubfunc_Supported = 0xC1;

/// Stream format opcodes (try 0xBF first, fallback to 0x2F)
constexpr uint8_t kStreamFormatOpcode_Primary = 0xBF;
constexpr uint8_t kStreamFormatOpcode_Alternate = 0x2F;

/// Parsed stream format information
struct StreamFormat {
    uint8_t formatType{0};      ///< 0x90 = AM824, etc.
    uint8_t formatSubtype{0};   ///< 0x00 = simple, 0x40 = compound
    uint8_t sampleRate{0};      ///< Sample rate code
    bool syncMode{false};       ///< Synchronization mode
    uint8_t numChannels{0};     ///< Number of channels
    std::vector<uint8_t> rawData; ///< Raw format block for detailed parsing
    
    bool IsValid() const { return formatType != 0; }
};

/// Stream Format Command
class AVCStreamFormatCommand : public AVCCommand {
public:
    /// Constructor for querying current format
    AVCStreamFormatCommand(FCPTransport& transport,
                          uint8_t subunitAddr,
                          uint8_t plugNum,
                          bool isInput,
                          bool useAlternateOpcode = false)
        : AVCCommand(transport, BuildCdb(subunitAddr, plugNum, isInput, 
                                        kStreamFormatSubfunc_Current, 0xFF,
                                        useAlternateOpcode)) {}
    
    /// Constructor for querying supported formats
    AVCStreamFormatCommand(FCPTransport& transport,
                          uint8_t subunitAddr,
                          uint8_t plugNum,
                          bool isInput,
                          uint8_t listIndex,
                          bool useAlternateOpcode = false)
        : AVCCommand(transport, BuildCdb(subunitAddr, plugNum, isInput,
                                        kStreamFormatSubfunc_Supported, listIndex,
                                        useAlternateOpcode)) {}
    
    /// Submit command with parsed format response
    void Submit(std::function<void(AVCResult, const std::optional<StreamFormat>&)> completion) {
        AVCCommand::Submit([completion](AVCResult result, const AVCCdb& response) {
            if (IsSuccess(result)) {
                auto format = ParseFormat(response);
                completion(result, format);
            } else {
                completion(result, std::nullopt);
            }
        });
    }

private:
    static AVCCdb BuildCdb(uint8_t subunitAddr, uint8_t plugNum, bool isInput,
                          uint8_t subfunction, uint8_t listIndex, bool useAlternateOpcode) {
        AVCCdb cdb;
        cdb.ctype = static_cast<uint8_t>(AVCCommandType::kStatus);
        cdb.subunit = subunitAddr;
        cdb.opcode = useAlternateOpcode ? kStreamFormatOpcode_Alternate : kStreamFormatOpcode_Primary;
        
        size_t offset = 0;
        cdb.operands[offset++] = subfunction; // 0xC0 or 0xC1
        cdb.operands[offset++] = isInput ? 0x00 : 0x01; // plug_direction
        
        if (subunitAddr == 0xFF) {
            // Unit plugs
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
            cdb.operands[offset++] = 0x01; // plug_type = subunit plug
            cdb.operands[offset++] = plugNum;
            cdb.operands[offset++] = 0xFF; // format_info_label
            cdb.operands[offset++] = 0xFF; // reserved
            if (subfunction == kStreamFormatSubfunc_Supported) {
                cdb.operands[offset++] = listIndex;
            }
        }
        
        cdb.operandLength = offset;
        return cdb;
    }
    
    static std::optional<StreamFormat> ParseFormat(const AVCCdb& response) {
        // Format block starts after header
        // For 0xC0 (current): offset 7 (unit) or 6 (subunit)
        // For 0xC1 (supported): offset 8 (unit) or 7 (subunit)
        
        if (response.operandLength < 3) {
            return std::nullopt;
        }
        
        // Determine format block offset based on subfunction
        uint8_t subfunction = response.operands[0];
        size_t formatOffset = 0;
        
        if (subfunction == kStreamFormatSubfunc_Current) {
            formatOffset = (response.subunit == 0xFF) ? 7 : 6;
        } else if (subfunction == kStreamFormatSubfunc_Supported) {
            formatOffset = (response.subunit == 0xFF) ? 8 : 7;
        } else {
            return std::nullopt;
        }
        
        if (response.operandLength <= formatOffset) {
            return std::nullopt;
        }
        
        StreamFormat fmt;
        fmt.formatType = response.operands[formatOffset];
        
        if (formatOffset + 1 < response.operandLength) {
            fmt.formatSubtype = response.operands[formatOffset + 1];
        }
        
        // Parse AM824 format (0x90)
        if (fmt.formatType == 0x90) {
            if (fmt.formatSubtype == 0x40 && response.operandLength >= formatOffset + 5) {
                // Compound AM824
                fmt.sampleRate = response.operands[formatOffset + 2];
                fmt.syncMode = (response.operands[formatOffset + 3] & 0x04) != 0;
                fmt.numChannels = response.operands[formatOffset + 4];
            } else if (fmt.formatSubtype == 0x00 && response.operandLength >= formatOffset + 6) {
                // Simple AM824 (6-byte format)
                fmt.sampleRate = (response.operands[formatOffset + 4] & 0xF0) >> 4;
                fmt.numChannels = 2; // Typically stereo for simple format
            } else if (fmt.formatSubtype == 0x00 && response.operandLength >= formatOffset + 3) {
                // 3-byte AM824 format
                fmt.sampleRate = 0xFF; // Don't care
                fmt.numChannels = 2;
            }
        }
        
        // Store raw data for detailed parsing if needed
        size_t rawDataLen = response.operandLength - formatOffset;
        fmt.rawData.assign(response.operands.begin() + formatOffset,
                          response.operands.begin() + formatOffset + rawDataLen);
        
        return fmt;
    }
};

} // namespace ASFW::Protocols::AVC
