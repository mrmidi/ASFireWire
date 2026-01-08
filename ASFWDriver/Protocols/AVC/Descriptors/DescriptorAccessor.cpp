//
// DescriptorAccessor.cpp
// ASFWDriver - AV/C Protocol Layer
//
// Implementation of high-level descriptor access with Apple-validated patterns
//

#include "DescriptorAccessor.hpp"
#include "../AVCDefs.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../../Logging/LogConfig.hpp"
#include <algorithm>

namespace ASFW::Protocols::AVC {

//==============================================================================
// Construction
//==============================================================================

DescriptorAccessor::DescriptorAccessor(FCPTransport& transport, uint8_t subunitAddr)
    : transport_(transport), subunitAddr_(subunitAddr) {
    ASFW_LOG_V3(Discovery, "DescriptorAccessor created for subunit 0x%02x", subunitAddr);
}

//==============================================================================
// Core Operations
//==============================================================================

void DescriptorAccessor::openForRead(const DescriptorSpecifier& specifier,
                                     SimpleCompletion completion) {
    ASFW_LOG_V3(Discovery, "OPEN DESCRIPTOR: subunit=0x%02x, specifier type=0x%02x, size=%zu",
                  subunitAddr_, static_cast<uint8_t>(specifier.type), specifier.size());
    
    auto cmd = std::make_shared<AVCOpenDescriptorCommand>(
        transport_,
        subunitAddr_,
        specifier,
        OpenDescriptorSubfunction::kReadOpen
    );
    
    cmd->Submit([completion](AVCResult result) {
        bool success = IsSuccess(result);
        ASFW_LOG_V3(Discovery, "OPEN DESCRIPTOR result: %d (success=%d)", 
                      static_cast<int>(result), success);
        completion(success);
    });
}

void DescriptorAccessor::close(const DescriptorSpecifier& specifier,
                               SimpleCompletion completion) {
    auto cmd = std::make_shared<AVCCloseDescriptorCommand>(
        transport_,
        subunitAddr_,
        specifier
    );
    
    cmd->Submit([completion](AVCResult result) {
        bool success = IsSuccess(result);
        ASFW_LOG_V3(Discovery, "CLOSE DESCRIPTOR result: %d (success=%d)",
                      static_cast<int>(result), success);
        completion(success);
    });
}

void DescriptorAccessor::readComplete(const DescriptorSpecifier& specifier,
                                      ReadCompletion completion) {
    ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Starting complete read (specifier size=%zu)",
                  specifier.size());
    
    auto state = std::make_shared<ReadChunkState>();
    state->specifier = specifier;
    state->totalDescriptorLength = 0;
    state->bytesReadSoFar = 0;
    state->attemptCount = 0;
    state->completion = completion;
    
    readNextChunk(state);
}

//==============================================================================
// Internal Chunked Read Implementation
//==============================================================================

void DescriptorAccessor::readNextChunk(std::shared_ptr<ReadChunkState> state) {
    if (++state->attemptCount > 50) {
        ASFW_LOG_ERROR(Discovery, "READ DESCRIPTOR: Exceeded max attempts (50)");
        ReadDescriptorResult result;
        result.success = false;
        result.avcResult = AVCResult::kTimeout;
        state->completion(result);
        return;
    }
    
    // Determine chunk size
    uint16_t chunkSize = MAX_DESCRIPTOR_CHUNK_SIZE;
    if (state->totalDescriptorLength > 0) {
        uint16_t remaining = state->totalDescriptorLength - state->bytesReadSoFar;
        chunkSize = std::min(chunkSize, remaining);
    }
    
    
    
    ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Attempt %d, offset=%u, chunk=%u",
                  state->attemptCount, state->bytesReadSoFar, chunkSize);
    
    auto cmd = std::make_shared<AVCReadDescriptorCommand>(
        transport_,
        subunitAddr_,
        state->specifier,
        state->bytesReadSoFar, // offset
        chunkSize              // length
    );
    
    cmd->Submit([this, state](AVCResult result, 
                             const AVCReadDescriptorCommand::ReadResult& readResult) {
        handleReadChunk(state, result, readResult);
    });
}

void DescriptorAccessor::handleReadChunk(std::shared_ptr<ReadChunkState> state,
                                        AVCResult result,
                                        const AVCReadDescriptorCommand::ReadResult& readResult) {
    if (!IsSuccess(result)) {
        ASFW_LOG_ERROR(Discovery, "READ DESCRIPTOR: Command failed with result %d",
                      static_cast<int>(result));
        ReadDescriptorResult finalResult;
        finalResult.success = false;
        finalResult.avcResult = result;
        state->completion(finalResult);
        return;
    }
    
    // First chunk? Extract total length from descriptor header
    if (state->bytesReadSoFar == 0 && readResult.data.size() >= 2) {
        state->totalDescriptorLength = (readResult.data[0] << 8) | readResult.data[1];
        ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Total length = %u bytes",
                      state->totalDescriptorLength);
        
        // Sanity check
        if (state->totalDescriptorLength > 4096) {
            ASFW_LOG_ERROR(Discovery, "READ DESCRIPTOR: Suspicious length %u, aborting",
                          state->totalDescriptorLength);
            ReadDescriptorResult finalResult;
            finalResult.success = false;
            finalResult.avcResult = AVCResult::kInvalidResponse;
            state->completion(finalResult);
            return;
        }
    }
    
    // Append data from this chunk
    if (!readResult.data.empty()) {
        state->accumulatedData.insert(
            state->accumulatedData.end(),
            readResult.data.begin(),
            readResult.data.end()
        );
        state->bytesReadSoFar += readResult.data.size();

        
        ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Accumulated %u/%u bytes, status=0x%02x",
                      state->bytesReadSoFar, state->totalDescriptorLength,
                      static_cast<uint8_t>(readResult.status));
    }
    
    //==========================================================================
    // Dual-Strategy Termination (Spec + Apple Workaround)
    //==========================================================================
    // Reference: Apple IOFireWireFamily comment - "Some devices don't report 
    // read_result_status correctly, so use a length check instead"
    //==========================================================================
    
    bool shouldContinue = false;
    
    // Strategy 1: Spec-compliant read_result_status checking
    if (readResult.status == ReadResultStatus::kMoreToRead) {
        shouldContinue = true;
    } else if (readResult.status == ReadResultStatus::kComplete ||
               readResult.status == ReadResultStatus::kDataLengthTooLarge) {
        shouldContinue = false;
        ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Spec says complete (status=0x%02x)",
                      static_cast<uint8_t>(readResult.status));
    }
    
    // Strategy 2: Length-based fallback (Apple's robust approach)
    // Override spec status if we have valid length info
    if (state->totalDescriptorLength > 0) {
        // APOGEE QUIRK: Apogee devices (Duet, Ensemble) report descriptor lengths
        // that are smaller than the actual nested block sizes. The last MusicPlugInfo
        // blocks get truncated. Read an extra buffer beyond the declared length to
        // capture the complete data.
        // Vendor ID 0xDB0300 = Apogee Electronics (from RE of AppleFWAudioDevice)
        constexpr uint16_t kApogeeExtraBytes = 64;  // Safety margin for oversized descriptors
        uint16_t targetLength = state->totalDescriptorLength + kApogeeExtraBytes;
        
        if (state->bytesReadSoFar < targetLength) {
            shouldContinue = true;
        } else {
            shouldContinue = false;
            ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Length-based complete (%u bytes, target=%u)",
                          state->bytesReadSoFar, targetLength);
        }
    }
    
    // Additional safety: No data received
    if (readResult.data.empty() && readResult.status == ReadResultStatus::kMoreToRead) {
        ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Device claims more data but sent empty chunk");
        shouldContinue = false;
    }
    
    if (shouldContinue) {
        // Continue reading
        readNextChunk(state);
    } else {
        // Complete
        ASFW_LOG_V2(Discovery, "READ DESCRIPTOR: Complete - read %u bytes total",
                     state->bytesReadSoFar);
        
        ReadDescriptorResult finalResult;
        finalResult.success = true;
        finalResult.data = std::move(state->accumulatedData);
        finalResult.avcResult = result;
        state->completion(finalResult);
    }
}

//==============================================================================
// Convenience Methods
//==============================================================================

void DescriptorAccessor::readUnitIdentifier(ReadCompletion completion) {
    auto specifier = DescriptorSpecifier::forUnitIdentifier();
    
    ASFW_LOG_V3(Discovery, "Reading Unit Identifier Descriptor");
    
    // Simple approach: Direct read without explicit OPEN/CLOSE
    // Many devices work without the full sequence for Identifier descriptors
    readComplete(specifier, completion);
}

void DescriptorAccessor::readStatusDescriptor(uint8_t descriptorType,
                                              ReadCompletion completion) {
    // Status descriptors use types 0x80-0xFF (subunit-dependent descriptors)
    // For Music Subunit, 0x80 is the Music Subunit Identifier/Status Descriptor
    // 
    // CRITICAL: Unlike Unit Identifier, status descriptors REQUIRE the full
    // OPEN → READ → CLOSE sequence! This was confirmed via packet capture
    // of the Apple driver working with the Apogee Duet.
    
    DescriptorSpecifier specifier;
    specifier.type = static_cast<DescriptorSpecifierType>(descriptorType);
    specifier.typeSpecificFields = {}; // Status descriptors typically have no extra fields
    
    ASFW_LOG_V3(Discovery, "Reading Status Descriptor (type=0x%02x) with OPEN→READ→CLOSE", 
                  descriptorType);
    
    // Use full sequence for subunit-dependent descriptors
    readWithOpenCloseSequence(specifier, completion);
}

//==============================================================================
// OPEN → READ → CLOSE Sequence (Required for subunit-dependent descriptors)
//==============================================================================

void DescriptorAccessor::readWithOpenCloseSequence(const DescriptorSpecifier& specifier,
                                                    ReadCompletion completion) {
    // Capture specifier for use in closures
    auto specifierCopy = std::make_shared<DescriptorSpecifier>(specifier);
    auto completionPtr = std::make_shared<ReadCompletion>(std::move(completion));
    
    ASFW_LOG_V3(Discovery, "OPEN→READ→CLOSE: Starting sequence (specifier type=0x%02x)",
                  static_cast<uint8_t>(specifier.type));
    
    //==========================================================================
    // Step 1: OPEN DESCRIPTOR (subfunction 0x01 = Read Open)
    //==========================================================================
    openForRead(*specifierCopy, [this, specifierCopy, completionPtr](bool openSuccess) {
        if (!openSuccess) {
            ASFW_LOG_ERROR(Discovery, "OPEN→READ→CLOSE: OPEN failed");
            ReadDescriptorResult result;
            result.success = false;
            result.avcResult = AVCResult::kRejected;
            (*completionPtr)(result);
            return;
        }
        
        ASFW_LOG_V3(Discovery, "OPEN→READ→CLOSE: OPEN succeeded, starting READ");
        
        //======================================================================
        // Step 2: READ DESCRIPTOR (with chunking and 0x11 handling)
        //======================================================================
        readComplete(*specifierCopy, [this, specifierCopy, completionPtr](
            const ReadDescriptorResult& readResult
        ) {
            ASFW_LOG_V3(Discovery, "OPEN→READ→CLOSE: READ %{public}s (%zu bytes)",
                          readResult.success ? "succeeded" : "failed",
                          readResult.data.size());
            
            // Save read result for after CLOSE
            auto savedResult = std::make_shared<ReadDescriptorResult>(readResult);
            
            //==================================================================
            // Step 3: CLOSE DESCRIPTOR (subfunction 0x00)
            //==================================================================
            close(*specifierCopy, [completionPtr, savedResult](bool closeSuccess) {
                if (!closeSuccess) {
                    ASFW_LOG_V2(Discovery, "OPEN→READ→CLOSE: CLOSE failed (continuing anyway)");
                    // Don't fail the overall operation - we have the data
                }
                
                ASFW_LOG_V3(Discovery, "OPEN→READ→CLOSE: Sequence complete");
                
                // Return the read result
                (*completionPtr)(*savedResult);
            });
        });
    });
}

} // namespace ASFW::Protocols::AVC
