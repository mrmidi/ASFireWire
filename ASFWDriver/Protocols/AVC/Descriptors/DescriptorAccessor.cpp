// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DescriptorAccessor.cpp - High-level descriptor access with Apple-validated patterns
//
// Modernized implementation using IAvcUnit and typed Command<Operands>.
//

#include "DescriptorAccessor.hpp"
#include "../../../Common/CallbackUtils.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../../Logging/LogConfig.hpp"

#include <algorithm>

namespace ASFW::Protocols::AVC {

//==============================================================================
// Construction
//==============================================================================

DescriptorAccessor::DescriptorAccessor(ASFW::AVC::IAvcUnit& unit, uint8_t subunitAddr)
    : unit_(unit), subunitAddress_(ASFW::AVC::SubunitAddress::FromByte(subunitAddr)) {
    ASFW_LOG_V3(Discovery, "DescriptorAccessor created for subunit 0x%02x", subunitAddr);
}

DescriptorAccessor::DescriptorAccessor(ASFW::AVC::IAvcUnit& unit, ASFW::AVC::SubunitAddress subunitAddr)
    : unit_(unit), subunitAddress_(subunitAddr) {
    ASFW_LOG_V3(Discovery, "DescriptorAccessor created for subunit 0x%02x", subunitAddr.Byte());
}

//==============================================================================
// Core Operations
//==============================================================================

void DescriptorAccessor::openForRead(const DescriptorSpecifier& specifier,
                                     SimpleCompletion completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    ASFW_LOG_V3(Discovery, "OPEN DESCRIPTOR: subunit=0x%02x, specifier type=0x%02x, size=%zu",
                subunitAddress_.Byte(), static_cast<uint8_t>(specifier.type), specifier.size());

    ASFW::AVC::Cmd::OpenDescriptorCommand cmd;
    cmd.address = subunitAddress_;
    cmd.operands.specifier = ASFW::AVC::Cmd::DescriptorSpecifier::Raw(specifier.buildSpecifier());
    cmd.operands.subfunction = ASFW::AVC::Cmd::OpenDescriptorSubfunction::kReadOpen;

    unit_.Control(cmd, [completionState](ASFW::AVC::Expected<ASFW::AVC::Cmd::OpenDescriptorReply> reply) {
        // ACCEPTED is the answer (FFADO avc_descriptor.cpp:178). The byte after
        // the subfunction is reserved: a Phase 88 echoes the command's FF there.
        const bool success = reply && reply->subfunction == ASFW::AVC::Cmd::OpenDescriptorSubfunction::kReadOpen;
        ASFW_LOG_V3(Discovery, "OPEN DESCRIPTOR result: success=%d", success);
        Common::InvokeSharedCallback(completionState, success);
    });
}

void DescriptorAccessor::close(const DescriptorSpecifier& specifier,
                               SimpleCompletion completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    ASFW::AVC::Cmd::OpenDescriptorCommand cmd;
    cmd.address = subunitAddress_;
    cmd.operands.specifier = ASFW::AVC::Cmd::DescriptorSpecifier::Raw(specifier.buildSpecifier());
    cmd.operands.subfunction = ASFW::AVC::Cmd::OpenDescriptorSubfunction::kClose;

    unit_.Control(cmd, [completionState](ASFW::AVC::Expected<ASFW::AVC::Cmd::OpenDescriptorReply> reply) {
        const bool success = reply && reply->subfunction == ASFW::AVC::Cmd::OpenDescriptorSubfunction::kClose;
        ASFW_LOG_V3(Discovery, "CLOSE DESCRIPTOR result: success=%d", success);
        Common::InvokeSharedCallback(completionState, success);
    });
}

void DescriptorAccessor::readComplete(const DescriptorSpecifier& specifier,
                                      ReadCompletion completion) {
    ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Starting complete read (specifier size=%zu)",
                specifier.size());

    auto state = std::make_shared<ReadChunkState>();
    state->specifier = ASFW::AVC::Cmd::DescriptorSpecifier::Raw(specifier.buildSpecifier());
    state->totalDescriptorLength = 0;
    state->bytesReadSoFar = 0;
    state->attemptCount = 0;
    state->completion = std::move(completion);

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
        const uint16_t remaining = state->totalDescriptorLength - state->bytesReadSoFar;
        chunkSize = std::min(chunkSize, remaining);
    }

    ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Attempt %d, offset=%u, chunk=%u",
                state->attemptCount, state->bytesReadSoFar, chunkSize);

    ASFW::AVC::Cmd::ReadDescriptorCommand cmd;
    cmd.address = subunitAddress_;
    cmd.operands.specifier = state->specifier;
    cmd.operands.offset = state->bytesReadSoFar;
    cmd.operands.length = chunkSize;

    unit_.Control(cmd, [this, state](ASFW::AVC::Expected<ASFW::AVC::Cmd::ReadDescriptorReply> reply) {
        handleReadChunk(state, reply);
    });
}

void DescriptorAccessor::handleReadChunk(
    std::shared_ptr<ReadChunkState> state,
    ASFW::AVC::Expected<ASFW::AVC::Cmd::ReadDescriptorReply> reply) {
    if (!reply) {
        ASFW_LOG_ERROR(Discovery, "READ DESCRIPTOR: Command failed with error %d",
                       static_cast<int>(reply.error().kind));
        ReadDescriptorResult finalResult;
        finalResult.success = false;
        finalResult.avcResult = (reply.error().kind == ASFW::AVC::AvcErrorKind::kTimeout)
            ? AVCResult::kTimeout : AVCResult::kRejected;
        state->completion(finalResult);
        return;
    }

    const auto& readResult = *reply;

    if (readResult.reportedOffset != state->bytesReadSoFar ||
        readResult.data.empty()) {
        ReadDescriptorResult finalResult;
        finalResult.success = false;
        finalResult.avcResult = AVCResult::kInvalidResponse;
        state->completion(finalResult);
        return;
    }

    // First chunk? Extract total length from descriptor header
    // Per TA 2002013 Table 7: descriptor_length is the byte count of following fields,
    // so the entire descriptor on wire is descriptor_length + 2 bytes.
    if (state->bytesReadSoFar == 0 && readResult.data.size() >= 2) {
        const uint16_t bodyLength = (static_cast<uint16_t>(readResult.data[0]) << 8) | readResult.data[1];
        const size_t declaredTotal = static_cast<size_t>(bodyLength) + 2;
        if (declaredTotal > 4096) {
            ReadDescriptorResult finalResult;
            finalResult.success = false;
            finalResult.avcResult = AVCResult::kInvalidResponse;
            state->completion(finalResult);
            return;
        }
        state->totalDescriptorLength = static_cast<uint16_t>(declaredTotal);
        ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Total length = %u bytes (body=%u + header=2)",
                    state->totalDescriptorLength, bodyLength);

    }

    // Append data from this chunk
    state->accumulatedData.insert(
        state->accumulatedData.end(),
        readResult.data.begin(),
        readResult.data.end()
    );
    state->bytesReadSoFar += static_cast<uint16_t>(readResult.data.size());

    ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Accumulated %u/%u bytes, status=0x%02x",
                state->bytesReadSoFar, state->totalDescriptorLength,
                static_cast<uint8_t>(readResult.status));

    //==========================================================================
    // Dual-Strategy Termination (Spec + Robust Length Check)
    // Reference: Apple IOFireWireFamily pattern
    //==========================================================================

    bool shouldContinue = false;

    // Strategy 1: Spec-compliant read_result_status checking
    if (readResult.status == ASFW::AVC::Cmd::ReadResultStatus::kMoreToRead) {
        shouldContinue = true;
    } else if (readResult.status == ASFW::AVC::Cmd::ReadResultStatus::kComplete ||
               readResult.status == ASFW::AVC::Cmd::ReadResultStatus::kDataLengthTooLarge) {
        shouldContinue = false;
        ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Spec says complete (status=0x%02x)",
                    static_cast<uint8_t>(readResult.status));
    }

    // Strategy 2: Length-based fallback (TA 2002013 Table 7)
    if (state->totalDescriptorLength > 0) {
        if (state->bytesReadSoFar < state->totalDescriptorLength) {
            shouldContinue = true;
        } else {
            shouldContinue = false;
            ASFW_LOG_V3(Discovery, "READ DESCRIPTOR: Length-based complete (%u bytes, target=%u)",
                        state->bytesReadSoFar, state->totalDescriptorLength);
        }
    }

    if (state->totalDescriptorLength == 0 ||
        state->bytesReadSoFar > state->totalDescriptorLength ||
        (readResult.status == ASFW::AVC::Cmd::ReadResultStatus::kMoreToRead &&
         state->bytesReadSoFar >= state->totalDescriptorLength)) {
        ReadDescriptorResult finalResult;
        finalResult.success = false;
        finalResult.avcResult = AVCResult::kInvalidResponse;
        state->completion(finalResult);
        return;
    }

    if (shouldContinue) {
        readNextChunk(state);
    } else {
        ASFW_LOG_V2(Discovery, "READ DESCRIPTOR: Complete - read %u bytes total",
                    state->bytesReadSoFar);

        ReadDescriptorResult finalResult;
        finalResult.success = state->bytesReadSoFar == state->totalDescriptorLength;
        finalResult.data = std::move(state->accumulatedData);
        finalResult.avcResult = finalResult.success ? AVCResult::kAccepted : AVCResult::kInvalidResponse;
        state->completion(finalResult);
    }
}

//==============================================================================
// Convenience Methods
//==============================================================================

void DescriptorAccessor::readUnitIdentifier(ReadCompletion completion) {
    auto specifier = DescriptorSpecifier::forUnitIdentifier();
    ASFW_LOG_V3(Discovery, "Reading Unit Identifier Descriptor");
    // FFADO performs OPEN, READ, and CLOSE for descriptor loads as well
    // (libavc/descriptors/avc_descriptor.cpp:165,184,274).
    readWithOpenCloseSequence(specifier, std::move(completion));
}

void DescriptorAccessor::readStatusDescriptor(uint8_t descriptorType,
                                              ReadCompletion completion) {
    DescriptorSpecifier specifier;
    specifier.type = static_cast<DescriptorSpecifierType>(descriptorType);
    specifier.typeSpecificFields = {};

    ASFW_LOG_V3(Discovery, "Reading Status Descriptor (type=0x%02x) with OPEN→READ→CLOSE",
                descriptorType);

    readWithOpenCloseSequence(specifier, std::move(completion));
}

//==============================================================================
// OPEN → READ → CLOSE Sequence (Required for subunit-dependent descriptors)
//==============================================================================

void DescriptorAccessor::readWithOpenCloseSequence(const DescriptorSpecifier& specifier,
                                                   ReadCompletion completion) {
    auto specifierCopy = std::make_shared<DescriptorSpecifier>(specifier);
    auto completionPtr = std::make_shared<ReadCompletion>(std::move(completion));

    ASFW_LOG_V3(Discovery, "OPEN→READ→CLOSE: Starting sequence (specifier type=0x%02x)",
                static_cast<uint8_t>(specifier.type));

    openForRead(*specifierCopy, [this, specifierCopy, completionPtr](bool openSuccess) {
        if (!openSuccess) {
            ASFW_LOG_V2(Discovery, "OPEN→READ→CLOSE: OPEN unavailable");
            ReadDescriptorResult result;
            result.success = false;
            result.avcResult = AVCResult::kRejected;
            (*completionPtr)(result);
            return;
        }

        ASFW_LOG_V3(Discovery, "OPEN→READ→CLOSE: OPEN succeeded, starting READ");

        readComplete(*specifierCopy, [this, specifierCopy, completionPtr](
            const ReadDescriptorResult& readResult
        ) {
            ASFW_LOG_V3(Discovery, "OPEN→READ→CLOSE: READ %{public}s (%zu bytes)",
                        readResult.success ? "succeeded" : "failed",
                        readResult.data.size());

            auto savedResult = std::make_shared<ReadDescriptorResult>(readResult);

            close(*specifierCopy, [completionPtr, savedResult](bool closeSuccess) {
                if (!closeSuccess) {
                    ASFW_LOG_V2(Discovery, "OPEN→READ→CLOSE: CLOSE failed (continuing anyway)");
                }

                ASFW_LOG_V3(Discovery, "OPEN→READ→CLOSE: Sequence complete");
                (*completionPtr)(*savedResult);
            });
        });
    });
}

} // namespace ASFW::Protocols::AVC
