//
// DescriptorAccessor.hpp
// ASFWDriver - AV/C Protocol Layer
//
// High-level API for AV/C Descriptor operations with automatic sequencing,
// chunking, and fallback mechanisms for non-compliant devices.
//
// Specification: TA Document 2002013 - AV/C Descriptor Mechanism 1.2
// Reference: Apple IOFireWireFamily (IOFireWireAVCLib), FWA DescriptorAccessor
//

#pragma once

#include "DescriptorTypes.hpp"
#include "AVCDescriptorCommands.hpp"
#include "../FCPTransport.hpp"
#include <vector>
#include <functional>
#include <memory>

namespace ASFW::Protocols::AVC {

//==============================================================================
// DescriptorAccessor - High-level Descriptor API
//==============================================================================

/// Provides high-level access to AV/C descriptors with automatic:
/// - OPEN → READ → CLOSE sequencing
/// - Chunked reading for large descriptors
/// - Fallback mechanisms for non-compliant devices
/// - read_result_status interpretation with length-based fallback (Apple pattern)
class DescriptorAccessor {
public:
    /// Result type for read operations
    struct ReadDescriptorResult {
        bool success;
        std::vector<uint8_t> data;
        AVCResult avcResult;
    };
    
    /// Completion handler type
    using ReadCompletion = std::function<void(const ReadDescriptorResult&)>;
    using SimpleCompletion = std::function<void(bool success)>;

    //==========================================================================
    // Construction
    //==========================================================================
    
    DescriptorAccessor(FCPTransport& transport, uint8_t subunitAddr);
    ~DescriptorAccessor() = default;

    //==========================================================================
    // Core Descriptor Operations
    //==========================================================================
    
    /// Open descriptor for reading
    /// Spec: Section 7.1 - OPEN DESCRIPTOR command
    void openForRead(const DescriptorSpecifier& specifier, 
                     SimpleCompletion completion);
    
    /// Read entire descriptor with automatic chunking
    /// Implements Apple's dual-strategy approach:
    /// - Primary: read_result_status checking (spec-compliant)
    /// - Fallback: length-based termination (real-world robustness)
    void readComplete(const DescriptorSpecifier& specifier,
                     ReadCompletion completion);
    
    /// Close descriptor
    /// Spec: Section 7.1 - OPEN DESCRIPTOR command (subfunction 0x00)
    void close(const DescriptorSpecifier& specifier,
              SimpleCompletion completion);

    //==========================================================================
    // Convenience Methods (OPEN → READ → CLOSE)
    //==========================================================================
    
    /// Read (Sub)unit Identifier Descriptor
    /// Spec: Section 6.2.1 - Type 0x00
    /// Automatically performs OPEN → READ → CLOSE sequence
    void readUnitIdentifier(ReadCompletion completion);
    
    /// Read Status Descriptor (type 0x80) with proper OPEN→READ→CLOSE sequence
    /// Note: 0x80-0xFF are subunit-type specific (subunit-dependent descriptors)
    /// For Music Subunit, 0x80 is the Status Descriptor containing dynamic info blocks
    /// 
    /// Key difference from readUnitIdentifier: Status descriptors REQUIRE the full
    /// OPEN → READ → CLOSE sequence to work on real hardware (confirmed via packet capture).
    void readStatusDescriptor(uint8_t descriptorType, 
                             ReadCompletion completion);
    
    /// Read descriptor with full OPEN → READ → CLOSE sequence
    /// Required for subunit-dependent descriptors (types 0x80-0xBF)
    /// The Apple driver uses this sequence for all descriptor reads except Unit Identifier
    void readWithOpenCloseSequence(const DescriptorSpecifier& specifier,
                                   ReadCompletion completion);

private:
    //==========================================================================
    // Internal State
    //==========================================================================
    
    FCPTransport& transport_;
    uint8_t subunitAddr_;
    
    //==========================================================================
    // Internal Chunked Read Implementation
    //==========================================================================
    
    struct ReadChunkState {
        DescriptorSpecifier specifier;
        std::vector<uint8_t> accumulatedData;
        uint16_t totalDescriptorLength;
        uint16_t bytesReadSoFar;
        int attemptCount;
        ReadCompletion completion;
    };
    
    /// Read next chunk using READ DESCRIPTOR command
    void readNextChunk(std::shared_ptr<ReadChunkState> state);
    
    /// Handle read chunk response - implements Apple's dual-strategy
    void handleReadChunk(std::shared_ptr<ReadChunkState> state,
                        AVCResult result,
                        const AVCReadDescriptorCommand::ReadResult& readResult);
};

} // namespace ASFW::Protocols::AVC
