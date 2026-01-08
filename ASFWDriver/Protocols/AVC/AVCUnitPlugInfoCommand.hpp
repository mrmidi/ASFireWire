//
// AVCUnitPlugInfoCommand.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C PLUG INFO Command (Opcode 0x02)
// Queries the number of Isochronous and External plugs on the Unit
//
// Reference: TA Document 1999008 - AV/C Digital Interface Command Set General Specification
//

#pragma once

#include "IAVCCommandSubmitter.hpp"
#include <functional>

namespace ASFW::Protocols::AVC {

/// Unit Plug Counts structure
struct UnitPlugCounts {
    uint8_t isoInputPlugs{0};
    uint8_t isoOutputPlugs{0};
    uint8_t extInputPlugs{0};
    uint8_t extOutputPlugs{0};

    bool IsValid() const {
        // A valid audio device usually has at least one ISO plug
        return isoInputPlugs > 0 || isoOutputPlugs > 0;
    }
};

/// Command to query Unit Plug Information
class AVCUnitPlugInfoCommand {
public:
    /// Constructor
    /// @param submitter Command submitter
    explicit AVCUnitPlugInfoCommand(IAVCCommandSubmitter& submitter)
        : submitter_(submitter)
        , cdb_(BuildCdb()) {}

    /// Submit command
    void Submit(std::function<void(AVCResult, const UnitPlugCounts&)> completion) {
        submitter_.SubmitCommand(cdb_, [completion](AVCResult result, const AVCCdb& response) {
            if (IsSuccess(result)) {
                completion(result, ParseResponse(response));
            } else {
                completion(result, UnitPlugCounts{});
            }
        });
    }

private:
    IAVCCommandSubmitter& submitter_;
    AVCCdb cdb_;

    static AVCCdb BuildCdb() {
        AVCCdb cdb;
        cdb.ctype = static_cast<uint8_t>(AVCCommandType::kStatus);
        cdb.subunit = 0xFF; // Unit address
        cdb.opcode = 0x02;  // PLUG INFO

        // Operand[0]: Subfunction (0x00 = Plug Info)
        cdb.operands[0] = 0x00; 
        
        // Operand[1-4]: 0xFF (Dummy/Query)
        for (int i = 1; i <= 4; ++i) {
            cdb.operands[i] = 0xFF;
        }

        cdb.operandLength = 5;
        return cdb;
    }

    static UnitPlugCounts ParseResponse(const AVCCdb& response) {
        UnitPlugCounts counts;

        // Response format:
        // [0] = Subfunction (0x00)
        // [1] = Isochronous Input Plugs
        // [2] = Isochronous Output Plugs
        // [3] = External Input Plugs
        // [4] = External Output Plugs

        if (response.operandLength >= 5) {
            counts.isoInputPlugs  = response.operands[1];
            counts.isoOutputPlugs = response.operands[2];
            counts.extInputPlugs  = response.operands[3];
            counts.extOutputPlugs = response.operands[4];
        }

        return counts;
    }
};

} // namespace ASFW::Protocols::AVC
