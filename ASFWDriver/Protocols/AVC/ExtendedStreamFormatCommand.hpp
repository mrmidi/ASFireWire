//
// ExtendedStreamFormatCommand.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Extended Stream Format Information (Opcode 0xBF)
// Used to query and set stream formats (AM824, IEC60958) and sample rates.
//

#pragma once

#include "AVCDefs.hpp"
#include "AVCAddress.hpp"
#include <vector>
#include <optional>
#include <span>

namespace ASFW::Protocols::AVC {

/// Supported stream format information
struct StreamFormatInfo {
    uint32_t sampleRate;
    // Add other fields like format type (AM824, IEC60958) later if needed
};

class ExtendedStreamFormatCommand {
public:
    enum class CommandType {
        kGetSupported,
        kGetCurrent,
        kSetFormat
    };

    /// Constructor
    /// @param type Command type (GetSupported, GetCurrent, SetFormat)
    /// @param plugAddr Target plug address
    ExtendedStreamFormatCommand(CommandType type, AVCAddress plugAddr);

    /// Build the AV/C command payload
    std::vector<uint8_t> BuildCommand() const;

    /// Parse the AV/C response payload
    /// @param response The full response payload (including opcode/status)
    /// @return true if parsed successfully
    bool ParseResponse(const std::span<const uint8_t>& response);

    /// Get the list of supported formats (valid after ParseResponse for kGetSupported)
    const std::vector<StreamFormatInfo>& GetSupportedFormats() const { return supportedFormats_; }

private:
    CommandType type_;
    AVCAddress plugAddr_;
    std::vector<StreamFormatInfo> supportedFormats_;
};

} // namespace ASFW::Protocols::AVC
