//
// AVCDescriptorCommands.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Low-level AV/C Descriptor Command Primitives (OPEN, READ, CLOSE)
// Specification: TA Document 2002013 - AV/C Descriptor Mechanism 1.2
//

#pragma once

#include "DescriptorTypes.hpp"
#include "../AVCCommand.hpp"
#include <vector>

namespace ASFW::Protocols::AVC {

//==============================================================================
// OPEN DESCRIPTOR Command (0x08)
// Ref: Section 7.1 - OPEN DESCRIPTOR command
//==============================================================================

class AVCOpenDescriptorCommand : public AVCCommand {
public:
    AVCOpenDescriptorCommand(FCPTransport& transport,
                             uint8_t subunitAddr,
                             const DescriptorSpecifier& specifier,
                             OpenDescriptorSubfunction subfunction)
        : AVCCommand(transport, BuildCdb(subunitAddr, specifier, subfunction)) {}

    void Submit(std::function<void(AVCResult)> completion) {
        AVCCommand::Submit([completion](AVCResult result, const AVCCdb& response) {
            completion(result);
        });
    }

private:
    static AVCCdb BuildCdb(uint8_t subunitAddr, 
                          const DescriptorSpecifier& specifier,
                          OpenDescriptorSubfunction subfunction) {
        AVCCdb cdb;
        cdb.ctype = static_cast<uint8_t>(AVCCommandType::kControl);
        cdb.subunit = subunitAddr; // FCP Frame Header handles subunit addressing
        cdb.opcode = 0x08; // OPEN DESCRIPTOR
        
        auto spec = specifier.buildSpecifier();
        size_t idx = 0;
        for (const auto& byte : spec) {
            if (idx >= sizeof(cdb.operands)) break;
            cdb.operands[idx++] = byte;
        }
        
        cdb.operands[idx++] = static_cast<uint8_t>(subfunction);
        cdb.operands[idx++] = 0xFF; // Reserved - must be 0xFF for AV/C Control commands
        cdb.operandLength = idx;
        return cdb;
    }
};

//==============================================================================
// READ DESCRIPTOR Command (0x09)
// Ref: Section 7.5 - READ DESCRIPTOR command
//==============================================================================

class AVCReadDescriptorCommand : public AVCCommand {
public:
    struct ReadResult {
        std::vector<uint8_t> data;
        ReadResultStatus status;
        uint16_t dataLength;  // Length reported by device
        uint16_t offset;      // Offset reported by device
    };

    AVCReadDescriptorCommand(FCPTransport& transport,
                             uint8_t subunitAddr,
                             const DescriptorSpecifier& specifier,
                             uint16_t offset,
                             uint16_t length)
        : AVCCommand(transport, BuildCdb(subunitAddr, specifier, offset, length)),
          specifierSize_(specifier.size()) {}

    void Submit(std::function<void(AVCResult, const ReadResult&)> completion) {
        size_t specSize = specifierSize_;
        
        AVCCommand::Submit([completion, specSize](AVCResult result, const AVCCdb& response) {
            ReadResult readResult;
            readResult.status = ReadResultStatus::kComplete; // Default
            readResult.dataLength = 0;
            readResult.offset = 0;
            
            if (IsSuccess(result)) {
                // Response format (operands):
                // [Specifier (N bytes)] + [Status (1)] + [Reserved (1)] + [Length (2)] + [Offset (2)] + [Data...]
                // 
                // CRITICAL: The specifier is variable-length, so we MUST calculate offsets dynamically
                size_t statusIndex = specSize;
                size_t headerSize = specSize + 6; // Status(1) + Rsvd(1) + Len(2) + Offset(2)
                
                if (response.operandLength >= headerSize) {
                    readResult.status = static_cast<ReadResultStatus>(response.operands[statusIndex]);
                    
                    // Data Length is at statusIndex + 2
                    readResult.dataLength = (response.operands[statusIndex + 2] << 8) | 
                                           response.operands[statusIndex + 3];
                    
                    // Offset is at statusIndex + 4
                    readResult.offset = (response.operands[statusIndex + 4] << 8) | 
                                       response.operands[statusIndex + 5];
                    
                    // Data starts at headerSize
                    if (response.operandLength > headerSize) {
                        size_t dataSize = std::min(
                            static_cast<size_t>(readResult.dataLength),
                            response.operandLength - headerSize
                        );
                        readResult.data.reserve(dataSize);
                        for (size_t i = 0; i < dataSize; i++) {
                            readResult.data.push_back(response.operands[headerSize + i]);
                        }
                    }
                }
            }
            
            completion(result, readResult);
        });
    }

private:
    size_t specifierSize_; // Store for response parsing

    static AVCCdb BuildCdb(uint8_t subunitAddr, 
                          const DescriptorSpecifier& specifier,
                          uint16_t offset, 
                          uint16_t length) {
        AVCCdb cdb;
        cdb.ctype = static_cast<uint8_t>(AVCCommandType::kControl);
        cdb.subunit = subunitAddr; // FCP Frame Header handles subunit addressing
        cdb.opcode = 0x09; // READ DESCRIPTOR
        
        auto spec = specifier.buildSpecifier();
        size_t idx = 0;
        for (const auto& byte : spec) {
            if (idx >= sizeof(cdb.operands)) break;
            cdb.operands[idx++] = byte;
        }
        
        cdb.operands[idx++] = 0xFF; // read_result_status = FF (request)
        cdb.operands[idx++] = 0x00; // Reserved
        cdb.operands[idx++] = (length >> 8) & 0xFF;
        cdb.operands[idx++] = length & 0xFF;
        cdb.operands[idx++] = (offset >> 8) & 0xFF;
        cdb.operands[idx++] = offset & 0xFF;
        cdb.operandLength = idx;
        return cdb;
    }
};

//==============================================================================
// CLOSE DESCRIPTOR Command (uses OPEN DESCRIPTOR with subfunction 0x00)
// Ref: Section 7.1 - OPEN DESCRIPTOR command
//==============================================================================

class AVCCloseDescriptorCommand : public AVCCommand {
public:
    AVCCloseDescriptorCommand(FCPTransport& transport,
                              uint8_t subunitAddr,
                              const DescriptorSpecifier& specifier)
        : AVCCommand(transport, BuildCdb(subunitAddr, specifier)) {}

    void Submit(std::function<void(AVCResult)> completion) {
        AVCCommand::Submit([completion](AVCResult result, const AVCCdb& response) {
            completion(result);
        });
    }

private:
    static AVCCdb BuildCdb(uint8_t subunitAddr, const DescriptorSpecifier& specifier) {
        AVCCdb cdb;
        cdb.ctype = static_cast<uint8_t>(AVCCommandType::kControl);
        cdb.subunit = subunitAddr; // FCP Frame Header handles subunit addressing
        cdb.opcode = 0x08; // OPEN DESCRIPTOR (with CLOSE subfunction)
        
        auto spec = specifier.buildSpecifier();
        size_t idx = 0;
        for (const auto& byte : spec) {
            if (idx >= sizeof(cdb.operands)) break;
            cdb.operands[idx++] = byte;
        }
        
        cdb.operands[idx++] = static_cast<uint8_t>(OpenDescriptorSubfunction::kClose);
        cdb.operands[idx++] = 0xFF; // Reserved - must be 0xFF for AV/C Control commands
        cdb.operandLength = idx;
        return cdb;
    }
};

} // namespace ASFW::Protocols::AVC
