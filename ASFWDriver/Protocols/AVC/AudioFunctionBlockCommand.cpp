//
// AudioFunctionBlockCommand.cpp
// ASFWDriver - AV/C Protocol Layer
//

#include "AudioFunctionBlockCommand.hpp"

namespace ASFW::Protocols::AVC {

AudioFunctionBlockCommand::AudioFunctionBlockCommand(IAVCCommandSubmitter& submitter,
                                                     uint8_t subunitAddr,
                                                     CommandType type,
                                                     uint8_t functionBlockId,
                                                     ControlSelector selector,
                                                     std::vector<uint8_t> data)
    : submitter_(submitter)
    , cdb_(BuildCdb(subunitAddr, type, functionBlockId, selector, data)) {}

void AudioFunctionBlockCommand::Submit(std::function<void(AVCResult, const std::vector<uint8_t>&)> completion) {
    submitter_.SubmitCommand(cdb_, [completion](AVCResult result, const AVCCdb& response) {
        if (IsSuccess(result)) {
            // Extract control data from response
            // Response format: [Opcode, FuncBlkType, FuncBlkID, CtlAttr, Len, Selector, Data...]
            // Data starts at offset 6 (if Len > 1)
            std::vector<uint8_t> responseData;
            if (response.operandLength > 6) {
                // Copy from offset 6 to end
                for (size_t i = 6; i < response.operandLength; ++i) {
                    responseData.push_back(response.operands[i]);
                }
            }
            completion(result, responseData);
        } else {
            completion(result, {});
        }
    });
}

AVCCdb AudioFunctionBlockCommand::BuildCdb(uint8_t subunitAddr,
                                           CommandType type,
                                           uint8_t functionBlockId,
                                           ControlSelector selector,
                                           const std::vector<uint8_t>& data) {
    AVCCdb cdb;
    cdb.ctype = static_cast<uint8_t>(type == CommandType::kControl ? AVCCommandType::kControl : AVCCommandType::kStatus);
    cdb.subunit = subunitAddr;
    cdb.opcode = 0xB8; // FUNCTION BLOCK

    size_t offset = 0;

    // Function Block Type: Feature (0x81)
    cdb.operands[offset++] = 0x81;

    // Function Block ID
    cdb.operands[offset++] = functionBlockId;

    // Control Attribute
    // 0x10 = Current
    cdb.operands[offset++] = 0x10;

    // Selector Length
    // 1 (Selector) + Data Length
    cdb.operands[offset++] = static_cast<uint8_t>(1 + data.size());

    // Control Selector
    cdb.operands[offset++] = static_cast<uint8_t>(selector);

    // Control Data
    for (uint8_t byte : data) {
        cdb.operands[offset++] = byte;
    }

    cdb.operandLength = offset;
    return cdb;
}

} // namespace ASFW::Protocols::AVC
