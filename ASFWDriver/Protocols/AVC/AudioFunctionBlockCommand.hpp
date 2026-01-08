//
// AudioFunctionBlockCommand.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Audio Function Block Command (Opcode 0xB8)
// Used to control audio features like Volume, Mute, and Sample Rate.
//

#pragma once

#include "AVCDefs.hpp"
#include "IAVCCommandSubmitter.hpp"
#include <vector>
#include <optional>
#include <functional>

namespace ASFW::Protocols::AVC {

class AudioFunctionBlockCommand {
public:
    enum class CommandType {
        kControl,
        kStatus
    };

    enum class ControlSelector : uint8_t {
        kMute = 0x01,
        kVolume = 0x02,
        kLRBalance = 0x03,
        kDelay = 0x0A,
        kSamplingFrequency = 0xC0,
        kCurrentStatus = 0x10
    };
    
    /// Constructor
    /// @param submitter Command submitter
    /// @param subunitAddr Subunit address (usually Audio 0x01 or Music 0x0C)
    /// @param type Command type (Control, Status)
    /// @param functionBlockId The ID of the function block (often Plug ID)
    /// @param selector The control selector (e.g., Volume, SampleRate)
    /// @param data Additional control data (e.g., the sample rate value)
    AudioFunctionBlockCommand(IAVCCommandSubmitter& submitter,
                              uint8_t subunitAddr,
                              CommandType type,
                              uint8_t functionBlockId,
                              ControlSelector selector,
                              std::vector<uint8_t> data = {});

    /// Submit command
    /// @param completion Callback with result and optional response data
    void Submit(std::function<void(AVCResult, const std::vector<uint8_t>&)> completion);

private:
    IAVCCommandSubmitter& submitter_;
    AVCCdb cdb_;

    static AVCCdb BuildCdb(uint8_t subunitAddr,
                           CommandType type,
                           uint8_t functionBlockId,
                           ControlSelector selector,
                           const std::vector<uint8_t>& data);
};

} // namespace ASFW::Protocols::AVC
