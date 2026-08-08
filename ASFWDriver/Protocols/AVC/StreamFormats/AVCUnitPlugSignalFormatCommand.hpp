//
// AVCUnitPlugSignalFormatCommand.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Unit Plug Signal Format Commands (INPUT/OUTPUT PLUG SIGNAL FORMAT)
// Unit-level commands (opcodes 0x18/0x19) - Oxford/Linux style
//
// Reference: IEC 61883-1, AV/C General Specification
//

#pragma once

#include "../AVCCommand.hpp"
#include "StreamFormatTypes.hpp"
#include <functional>

namespace ASFW::Protocols::AVC::StreamFormats {

//==============================================================================
// UNIT PLUG SIGNAL FORMAT Command (0x18 / 0x19) - Unit Level
//==============================================================================

/// Query/Set INPUT/OUTPUT PLUG SIGNAL FORMAT at Unit level
/// These are Unit-level commands that work on firewire-audio devices
///
/// Opcode 0x18 = OUTPUT PLUG SIGNAL FORMAT
/// Opcode 0x19 = INPUT PLUG SIGNAL FORMAT
///
/// This is the "Oxford/Linux style" approach that works with devices like Apogee Duet
class AVCUnitPlugSignalFormatCommand : public AVC::AVCCommand {
public:
    /// Signal format response
    struct SignalFormat {
        uint8_t plugID{0};        ///< Plug ID
        uint8_t format{0xFF};     ///< Format byte (e.g. 0x90 for AM824)
        uint8_t frequency{0xFF};  ///< Frequency byte (e.g. 0x02 for 48kHz)

        bool IsValid() const { return format != 0xFF && frequency != 0xFF; }
    };

    /// Constructor (Status Query)
    /// @param transport FCP transport for command submission
    /// @param plugID Plug ID (usually 0)
    /// @param isInput true for INPUT (0x19), false for OUTPUT (0x18)
    AVCUnitPlugSignalFormatCommand(AVC::FCPTransport& transport,
                                   uint8_t plugID,
                                   bool isInput)
        : AVCCommand(transport, BuildCdb(plugID, isInput, std::nullopt)) {}

    /// Constructor (Control Set)
    /// @param transport FCP transport for command submission
    /// @param plugID Plug ID (usually 0)
    /// @param isInput true for INPUT (0x19), false for OUTPUT (0x18)
    /// @param rate Sample rate to set
    AVCUnitPlugSignalFormatCommand(AVC::FCPTransport& transport,
                                   uint8_t plugID,
                                   bool isInput,
                                   SampleRate rate)
        : AVCCommand(transport, BuildCdb(plugID, isInput, rate,
                                         AVC::AVCCommandType::kControl)) {}

    /// Tag selecting SPECIFIC INQUIRY rather than CONTROL.
    ///
    /// The operands are identical to a set, but the ctype asks "would you
    /// accept this?" instead of "do this". Discovery must probe with inquiry:
    /// probing with CONTROL would retune the device once per candidate rate,
    /// which is a device-visible side effect during what is meant to be a
    /// read-only capability scan. See AV/C General §, and the reference use in
    /// snd-firewire-ctl-services oxfw/src/lib.rs (specific_inquiry).
    struct InquireRate {
        SampleRate rate{SampleRate::kUnknown};
    };

    /// Constructor (Specific Inquiry — capability probe, no state change)
    AVCUnitPlugSignalFormatCommand(AVC::FCPTransport& transport,
                                   uint8_t plugID,
                                   bool isInput,
                                   InquireRate inquiry)
        : AVCCommand(transport, BuildCdb(plugID, isInput, inquiry.rate,
                                         AVC::AVCCommandType::kInquiry)) {}

    /// Submit command with signal format response
    void Submit(std::function<void(AVC::AVCResult, SignalFormat)> completion) {
        AVCCommand::Submit([completion](AVC::AVCResult result, const AVC::AVCCdb& response) {
            if (AVC::IsSuccess(result) && response.operandLength >= 3) {
                SignalFormat fmt;
                fmt.plugID = response.operands[0];
                fmt.format = response.operands[1];
                fmt.frequency = response.operands[2];
                completion(result, fmt);
            } else {
                completion(result, {0, 0xFF, 0xFF});
            }
        });
    }

    /// Convert frequency byte to SampleRate enum
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
    static AVC::AVCCdb BuildCdb(uint8_t plugID, bool isInput,
                               std::optional<SampleRate> setRate,
                               AVC::AVCCommandType ctype = AVC::AVCCommandType::kStatus) {
        AVC::AVCCdb cdb;

        // A rate-bearing frame is CONTROL or INQUIRY depending on the caller;
        // a rate-less frame is always a STATUS query.
        cdb.ctype = static_cast<uint8_t>(setRate.has_value() ? ctype
                                                             : AVC::AVCCommandType::kStatus);

        cdb.subunit = 0xFF;  // Unit level
        cdb.opcode = isInput ? 0x19 : 0x18; // INPUT/OUTPUT PLUG SIGNAL FORMAT

        cdb.operands[0] = plugID;  // Plug ID
        
        if (setRate.has_value()) {
            // SET: Use AM824 (0x90) and specific frequency
            cdb.operands[1] = 0x90; // AM824
            cdb.operands[2] = SampleRateToFrequency(*setRate);
            cdb.operands[3] = 0xFF; // Padding/Sync
            cdb.operands[4] = 0xFF; // Padding/Sync
        } else {
            // QUERY: Use 0xFF
            cdb.operands[1] = 0xFF; // Format (query)
            cdb.operands[2] = 0xFF; // Frequency (query)
            cdb.operands[3] = 0xFF; // Padding
            cdb.operands[4] = 0xFF; // Padding
        }

        cdb.operandLength = 5;
        return cdb;
    }
    
    static uint8_t SampleRateToFrequency(SampleRate rate) {
        // Standard FDF/SFC codes (IEC 61883-6)
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
