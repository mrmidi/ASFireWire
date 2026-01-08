//
// AVCSignalFormatCommand.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Signal Format Commands (INPUT/OUTPUT SIGNAL FORMAT STATUS)
//

#pragma once

#include "AVCCommand.hpp"
#include "StreamFormats/StreamFormatTypes.hpp"
#include <vector>

namespace ASFW::Protocols::AVC {

//==============================================================================
// SIGNAL FORMAT Command (0xA0 / 0xA1)
//==============================================================================

// Opcode 0xA0 = INPUT SIGNAL FORMAT
// Opcode 0xA1 = OUTPUT SIGNAL FORMAT

class AVCSignalFormatCommand : public AVCCommand {
public:
    struct SignalFormat {
        uint8_t format; 
        StreamFormats::SampleRate sampleRate;
    };

    AVCSignalFormatCommand(FCPTransport& transport,
                           uint8_t subunitAddr,
                           bool isInput, // true = INPUT (0xA0), false = OUTPUT (0xA1)
                           uint8_t plugID)
        : AVCCommand(transport, BuildCdb(subunitAddr, isInput, plugID)) {}

    void Submit(std::function<void(AVCResult, SignalFormat)> completion) {
        AVCCommand::Submit([completion](AVCResult result, const AVCCdb& response) {
            if (IsSuccess(result) && response.operandLength >= 2) {
                SignalFormat fmt;
                fmt.format = response.operands[0];
                // Use Music Subunit specific mapping
                fmt.sampleRate = StreamFormats::MusicSubunitCodeToSampleRate(response.operands[1]);
                completion(result, fmt);
            } else {
                completion(result, {0xFF, StreamFormats::SampleRate::kUnknown});
            }
        });
    }

private:
    static AVCCdb BuildCdb(uint8_t subunitAddr, bool isInput, uint8_t plugID) {
        AVCCdb cdb;
        cdb.ctype = static_cast<uint8_t>(AVCCommandType::kStatus);
        cdb.subunit = subunitAddr;
        cdb.opcode = isInput ? 0xA0 : 0xA1;
        cdb.operands[0] = 0xFF; // Format (query)
        cdb.operands[1] = 0xFF; // Frequency (query)
        cdb.operandLength = 2;
        return cdb;
    }
};

//==============================================================================
// OUTPUT PLUG SIGNAL FORMAT Command (0x18)
//==============================================================================

class AVCOutputPlugSignalFormatCommand : public AVCCommand {
public:
    struct SignalFormat {
        uint8_t formatHierarchy; // e.g. 0x90 (AM824)
        uint8_t formatSync;      // e.g. 0x01 (48kHz)
    };

    AVCOutputPlugSignalFormatCommand(FCPTransport& transport, uint8_t plugID = 0)
        : AVCCommand(transport, BuildCdb(plugID)) {}

    void Submit(std::function<void(AVCResult, SignalFormat)> completion) {
        AVCCommand::Submit([completion](AVCResult result, const AVCCdb& response) {
            if (IsSuccess(result) && response.operandLength >= 3) {
                SignalFormat fmt;
                fmt.formatHierarchy = response.operands[1];
                fmt.formatSync = response.operands[2];
                completion(result, fmt);
            } else {
                completion(result, {0xFF, 0xFF});
            }
        });
    }

private:
    static AVCCdb BuildCdb(uint8_t plugID) {
        AVCCdb cdb;
        cdb.ctype = static_cast<uint8_t>(AVCCommandType::kStatus);
        cdb.subunit = kAVCSubunitUnit; // 0xFF
        cdb.opcode = 0x18; // Output Plug Signal Format
        cdb.operands[0] = plugID;
        cdb.operands[1] = 0xFF; // formatHierarchy
        cdb.operands[2] = 0xFF; // formatSync
        cdb.operands[3] = 0xFF; // padding
        cdb.operands[4] = 0xFF; // padding
        cdb.operandLength = 5;
        return cdb;
    }
};

} // namespace ASFW::Protocols::AVC
