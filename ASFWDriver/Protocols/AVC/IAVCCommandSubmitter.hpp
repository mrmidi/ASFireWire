//
// IAVCCommandSubmitter.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Interface for submitting AV/C commands.
// Decouples command logic from the specific transport/unit implementation.
//

#pragma once

#include "AVCDefs.hpp"
#include "AVCCommand.hpp"

namespace ASFW::Protocols::AVC {

class IAVCCommandSubmitter {
public:
    virtual ~IAVCCommandSubmitter() = default;

    /// Submit generic AV/C command
    /// @param cdb Command descriptor block
    /// @param completion Callback with result and response
    virtual void SubmitCommand(const AVCCdb& cdb, AVCCompletion completion) = 0;
};

} // namespace ASFW::Protocols::AVC
