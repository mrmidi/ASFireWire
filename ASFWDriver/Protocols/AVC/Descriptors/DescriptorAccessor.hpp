// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DescriptorAccessor.hpp - High-level API for AV/C Descriptor operations
// with automatic sequencing, chunking, and fallback mechanisms for non-compliant devices.
//
// Specification: TA Document 2002013 - AV/C Descriptor Mechanism 1.2
// Reference: Apple IOFireWireFamily (IOFireWireAVCLib), FWA DescriptorAccessor
//

#pragma once

#include "DescriptorTypes.hpp"
#include "../Commands/DescriptorCommands.hpp"
#include "../Core/IAvcUnit.hpp"
#include "../AVCDefs.hpp"

#include <functional>
#include <memory>
#include <vector>

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
        bool success{false};
        std::vector<uint8_t> data;
        AVCResult avcResult{AVCResult::kRejected};
    };

    /// Completion handler types
    using ReadCompletion = std::function<void(const ReadDescriptorResult&)>;
    using SimpleCompletion = std::function<void(bool success)>;

    //==========================================================================
    // Construction
    //==========================================================================

    DescriptorAccessor(ASFW::AVC::IAvcUnit& unit, uint8_t subunitAddr = 0xFF);
    DescriptorAccessor(ASFW::AVC::IAvcUnit& unit, ASFW::AVC::SubunitAddress subunitAddr);
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
    /// Performs OPEN → READ → CLOSE for the identifier descriptor.
    void readUnitIdentifier(ReadCompletion completion);

    /// Read Status Descriptor (type 0x80) with proper OPEN→READ→CLOSE sequence
    void readStatusDescriptor(uint8_t descriptorType,
                             ReadCompletion completion);

    /// Read descriptor with full OPEN → READ → CLOSE sequence
    /// Required for subunit-dependent descriptors (types 0x80-0xBF)
    void readWithOpenCloseSequence(const DescriptorSpecifier& specifier,
                                   ReadCompletion completion);

private:
    ASFW::AVC::IAvcUnit& unit_;
    ASFW::AVC::SubunitAddress subunitAddress_;

    struct ReadChunkState {
        ASFW::AVC::Cmd::DescriptorSpecifier specifier;
        std::vector<uint8_t> accumulatedData;
        uint16_t totalDescriptorLength{0};
        uint16_t bytesReadSoFar{0};
        int attemptCount{0};
        ReadCompletion completion;
    };

    void readNextChunk(std::shared_ptr<ReadChunkState> state);
    void handleReadChunk(std::shared_ptr<ReadChunkState> state,
                         ASFW::AVC::Expected<ASFW::AVC::Cmd::ReadDescriptorReply> reply);
};

} // namespace ASFW::Protocols::AVC
