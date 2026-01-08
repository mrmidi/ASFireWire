//
// AVCSignalFormatCommand.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Signal Format Commands (INPUT/OUTPUT SIGNAL FORMAT STATUS)
// Music Subunit specific commands (opcodes 0xA0/0xA1)
//
// Refactored to use StreamFormat types
//

#pragma once

#include "../AVCCommand.hpp"
#include "StreamFormatTypes.hpp"
#include <vector>
#include <functional>

namespace ASFW::Protocols::AVC::StreamFormats {

//==============================================================================
// SIGNAL FORMAT Command (0xA0 / 0xA1) - Music Subunit Specific
//==============================================================================

/// Query INPUT/OUTPUT SIGNAL FORMAT for Music Subunit
/// These are Music Subunit-specific commands, different from general STREAM FORMAT
///
/// Opcode 0xA0 = INPUT SIGNAL FORMAT
/// Opcode 0xA1 = OUTPUT SIGNAL FORMAT
///
/// **WARNING**: These opcodes (0xA0/0xA1) are Music Subunit specific and many
/// devices (including Apogee Duet) do NOT respond to them for sample rate changes.
/// For most FireWire audio devices, use AVCUnitPlugSignalFormatCommand instead,
/// which uses Unit-level opcodes 0x18/0x19 (Oxford/Linux style).
///
/// Reference: TA Document 2001007 - Music Subunit Specification
class AVCSignalFormatCommand : public AVC::AVCCommand {
public:
    /// Simple signal format response
    struct SignalFormat {
        uint8_t format{0xFF};     ///< Format byte (e.g. 0x90 for AM824)
        uint8_t frequency{0xFF};  ///< Frequency byte (e.g. 0x04 for 48kHz)

        bool IsValid() const { return format != 0xFF && frequency != 0xFF; }
    };

    /// Constructor (Status Query)
    /// @param transport FCP transport for command submission
    /// @param subunitAddr Music subunit address
    /// @param isInput true for INPUT (0xA0), false for OUTPUT (0xA1)
    /// @param plugID Plug ID (or 0xFF for subunit-level query)
    AVCSignalFormatCommand(AVC::FCPTransport& transport,
                           uint8_t subunitAddr,
                           bool isInput,
                           uint8_t plugID = 0xFF)
        : AVCCommand(transport, BuildCdb(subunitAddr, isInput, plugID, std::nullopt)) {}

    /// Constructor (Control Set)
    /// @param transport FCP transport for command submission
    /// @param subunitAddr Music subunit address
    /// @param isInput true for INPUT (0xA0), false for OUTPUT (0xA1)
    /// @param rate Sample rate to set
    /// @param plugID Plug ID (or 0xFF for subunit-level query)
    AVCSignalFormatCommand(AVC::FCPTransport& transport,
                           uint8_t subunitAddr,
                           bool isInput,
                           SampleRate rate,
                           uint8_t plugID = 0xFF)
        : AVCCommand(transport, BuildCdb(subunitAddr, isInput, plugID, rate)) {}

    /// Submit command with signal format response
    void Submit(std::function<void(AVC::AVCResult, SignalFormat)> completion) {
        AVCCommand::Submit([completion](AVC::AVCResult result, const AVC::AVCCdb& response) {
            if (AVC::IsSuccess(result) && response.operandLength >= 2) {
                SignalFormat fmt;
                fmt.format = response.operands[0];
                fmt.frequency = response.operands[1];
                completion(result, fmt);
            } else {
                completion(result, {0xFF, 0xFF});
            }
        });
    }

    /// Convert SignalFormat to SampleRate enum
    /// @param freq Frequency byte from response
    /// @return SampleRate enum value
    static SampleRate FrequencyToSampleRate(uint8_t freq) {
        // Standard FDF/SFC codes (IEC 61883-6)
        switch (freq) {
            case 0x00: return SampleRate::k32000Hz;
            case 0x01: return SampleRate::k44100Hz;
            case 0x02: return SampleRate::k48000Hz;
            case 0x03: return SampleRate::k88200Hz;
            case 0x04: return SampleRate::k96000Hz;
            case 0x05: return SampleRate::k176400Hz;
            case 0x06: return SampleRate::k192000Hz;
            default: return SampleRate::kUnknown;
        }
    }

private:
    static AVC::AVCCdb BuildCdb(uint8_t subunitAddr, bool isInput, uint8_t plugID, 
                               std::optional<SampleRate> setRate) {
        AVC::AVCCdb cdb;
        
        if (setRate.has_value()) {
            cdb.ctype = static_cast<uint8_t>(AVC::AVCCommandType::kControl);
        } else {
            cdb.ctype = static_cast<uint8_t>(AVC::AVCCommandType::kStatus);
        }
        
        cdb.subunit = subunitAddr;
        cdb.opcode = isInput ? 0xA0 : 0xA1; // INPUT/OUTPUT SIGNAL FORMAT

        if (setRate.has_value()) {
            // SET: Use AM824 (0x90) and specific frequency
            cdb.operands[0] = 0x90; // AM824
            cdb.operands[1] = SampleRateToFrequency(*setRate);
        } else {
            // QUERY: Use 0xFF
            cdb.operands[0] = 0xFF; // Format (query)
            cdb.operands[1] = 0xFF; // Frequency (query)
        }

        // Some devices may require plugID for plug-specific queries
        // For now, keeping the simple 2-operand form as per common usage
        // If per-plug format is needed, add plugID as operand[2]

        cdb.operandLength = 2;
        return cdb;
    }
    
    static uint8_t SampleRateToFrequency(SampleRate rate) {
        // Standard FDF/SFC codes (IEC 61883-6)
        // 0x00=32k, 0x01=44.1k, 0x02=48k, 0x03=88.2k, 0x04=96k, 0x05=176.4k, 0x06=192k
        switch (rate) {
            case SampleRate::k32000Hz: return 0x00;
            case SampleRate::k44100Hz: return 0x01;
            case SampleRate::k48000Hz: return 0x02;
            case SampleRate::k88200Hz: return 0x03;
            case SampleRate::k96000Hz: return 0x04;
            case SampleRate::k176400Hz: return 0x05;
            case SampleRate::k192000Hz: return 0x06;
            default: return 0xFF;
        }
    }
};

} // namespace ASFW::Protocols::AVC::StreamFormats
