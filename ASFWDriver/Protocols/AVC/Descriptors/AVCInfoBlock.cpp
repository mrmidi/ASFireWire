//
// AVCInfoBlock.cpp
// ASFWDriver - AV/C Protocol Layer
//
// Implementation of AV/C Info Block parsing
//

#include "AVCInfoBlock.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../../Logging/LogConfig.hpp"
#include <algorithm>

namespace ASFW::Protocols::AVC::Descriptors {

/// Helper to read big-endian uint16_t
static inline uint16_t ReadBE16(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

//==============================================================================
// AVCInfoBlock - Construction
//==============================================================================

AVCInfoBlock::AVCInfoBlock(
    uint16_t compoundLength,
    uint16_t primaryFieldsLength,
    uint16_t type,
    std::vector<uint8_t> primaryData,
    std::vector<AVCInfoBlock> nestedBlocks
)
    : compoundLength_(compoundLength)
    , primaryFieldsLength_(primaryFieldsLength)
    , type_(type)
    , primaryData_(std::move(primaryData))
    , nestedBlocks_(std::move(nestedBlocks))
{
}

//==============================================================================
// AVCInfoBlock - Parsing
//==============================================================================

std::expected<AVCInfoBlock, AVCResult> AVCInfoBlock::Parse(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed
) {
    bytesConsumed = 0;

    // Minimum: 6 bytes header (compound_length + type + primary_fields_length)
    if (length < 6) {
        ASFW_LOG_ERROR(Discovery, "Info block too short (%zu bytes, need >=6)", length);
        return std::unexpected(AVCResult::kInvalidResponse);
    }

    // Parse header per TA 1999045 Table 4.1
    // [0-1] compound_length
    // [2-3] info_block_type
    // [4-5] primary_fields_length
    uint16_t compoundLength = ReadBE16(data);
    uint16_t type = ReadBE16(data + 2);
    uint16_t primaryFieldsLength = ReadBE16(data + 4);

    ASFW_LOG_V3(Discovery, "Parsing info block: type=0x%04x, compound_len=%u, primary_len=%u",
                  type, compoundLength, primaryFieldsLength);

    // NOTE: The "Apogee Header Quirk" was removed. Analysis confirmed that the
    // Apogee Duet returns a spec-compliant descriptor with GMSSA (0x8100) placed
    // before RoutingStatus (0x8108). Per TA 1999045 and TA 2002013, Info Block
    // ordering is not mandated. The old workaround misfired during nested parsing
    // when 0x000A (Name Info Block type) appeared adjacent to 0x8100, causing
    // severe parser misalignment and cascading failures (truncated blocks,
    // garbage type values like 0x0100, and FCP timeouts).

    // Validate compound length with ROBUST handling
    // compound_length excludes itself (2 bytes), so total size is +2
    size_t claimedTotalSize = static_cast<size_t>(compoundLength) + 2;
    size_t effectiveLength = length;
    bool truncated = false;

    // compound_length includes Type(2) + PrimLen(2) + Fields...
    // So minimum valid compound_length is 4.
    if (compoundLength < 4) {
        ASFW_LOG_ERROR(Discovery, "Invalid compound_length %u (must be >=4)", compoundLength);
        return std::unexpected(AVCResult::kInvalidResponse);
    }

    // Check for overflow/truncation
    if (claimedTotalSize > length) {
        ASFW_LOG_V3(Discovery,
            "Info block truncated: claimed %zu bytes (len=%u), available %zu bytes. Parsing what is available.",
            claimedTotalSize, compoundLength, length);
        truncated = true;
        effectiveLength = length;
    } else {
        effectiveLength = claimedTotalSize;
    }

    // Validate primary fields length
    // Max possible primary length is (effectiveLength - 6)
    // Header is 6 bytes (Len+Type+PrimLen)
    size_t maxPrimary = (effectiveLength >= 6u) ? (effectiveLength - 6u) : 0u;
    
    if (primaryFieldsLength > maxPrimary) {
        ASFW_LOG_V3(Discovery,
            "Primary fields truncated: claimed %u bytes, available %zu bytes.",
            primaryFieldsLength, maxPrimary);
        primaryFieldsLength = static_cast<uint16_t>(maxPrimary);
    }

    // Extract primary data (skip 6-byte header)
    std::vector<uint8_t> primaryData;
    if (primaryFieldsLength > 0) {
        primaryData.assign(data + 6, data + 6 + primaryFieldsLength);
    }

    // Parse nested info blocks (if any)
    std::vector<AVCInfoBlock> nestedBlocks;
    size_t nestedDataOffset = 6 + primaryFieldsLength;
    
    if (nestedDataOffset < effectiveLength) {
        size_t nestedDataLength = effectiveLength - nestedDataOffset;
        
        ASFW_LOG_V3(Discovery, "Parsing nested blocks (%zu bytes)", nestedDataLength);

        size_t nestedBytesConsumed = 0;
        auto nestedResult = ParseNestedBlocks(
            data + nestedDataOffset,
            nestedDataLength,
            nestedBytesConsumed
        );

        if (nestedResult) {
            nestedBlocks = std::move(*nestedResult);
        } else {
            // Log error but don't fail the whole block - return what we parsed
            ASFW_LOG_V3(Discovery, "Failed to parse some nested blocks (error %d)", 
                           static_cast<int>(nestedResult.error()));
        }
    }

    // Bytes consumed is the effective length used from the buffer
    bytesConsumed = effectiveLength;

    return AVCInfoBlock(
        compoundLength,
        primaryFieldsLength,
        type,
        std::move(primaryData),
        std::move(nestedBlocks)
    );
}

std::expected<std::vector<AVCInfoBlock>, AVCResult> AVCInfoBlock::ParseNestedBlocks(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed
) {
    std::vector<AVCInfoBlock> blocks;
    bytesConsumed = 0;

    while (bytesConsumed < length) {
        size_t remaining = length - bytesConsumed;

        // Need at least 6 bytes for next block header
        if (remaining < 6) {
            // Not enough for a header, stop parsing nested blocks
            break; 
        }

        // Peek at size to handle truncation logic
        uint16_t nextCompoundLen = (data[bytesConsumed] << 8) | data[bytesConsumed + 1];
        size_t nextTotalSize = static_cast<size_t>(nextCompoundLen) + 2;
        
        // FWA FALLBACK: Check for invalid block sizes (padding/garbage)
        // ASFW requires 6 bytes for header (len+type+primLen), so anything less is invalid.
        if (nextTotalSize < 6 || nextCompoundLen == 0xFFFF) {
             ASFW_LOG_V3(Discovery, "Invalid nested block size at offset %zu (size=%zu). Scanning... (skipping 4 bytes)", 
                            bytesConsumed, nextTotalSize);
             bytesConsumed += 4;
             continue;
        }
        
        // Check if next block fits
        bool blockTruncated = false;
        size_t bytesToParse = nextTotalSize;
        
        if (nextTotalSize > remaining) {
            ASFW_LOG_V3(Discovery, 
                "Nested block at offset %zu truncated: claimed %zu (len=%u), remaining %zu. Parsing partial.",
                bytesConsumed, nextTotalSize, nextCompoundLen, remaining);
            blockTruncated = true;
            bytesToParse = remaining;
        }

        size_t blockBytesConsumed = 0;
        auto blockResult = Parse(data + bytesConsumed, bytesToParse, blockBytesConsumed);

        if (!blockResult) {
            ASFW_LOG_V3(Discovery, "Failed to parse nested block at offset %zu. Scanning... (skipping 4 bytes)", bytesConsumed);
            // Don't break! FWA fallback: skip header and try to find next valid block.
            bytesConsumed += 4;
            continue;
        }

        blocks.push_back(std::move(*blockResult));
        bytesConsumed += blockBytesConsumed;

        if (blockTruncated) {
            // If this block was truncated, we can't trust alignment for subsequent blocks
            break;
        }
    }

    return blocks;
}

//==============================================================================
// AVCInfoBlock - Navigation Helpers
//==============================================================================

std::optional<AVCInfoBlock> AVCInfoBlock::FindNested(uint16_t type) const {
    auto it = std::find_if(nestedBlocks_.begin(), nestedBlocks_.end(),
                          [type](const AVCInfoBlock& block) {
                              return block.GetType() == type;
                          });

    if (it != nestedBlocks_.end()) {
        return *it;
    }

    return std::nullopt;
}

std::vector<AVCInfoBlock> AVCInfoBlock::FindAllNested(uint16_t type) const {
    std::vector<AVCInfoBlock> matches;

    for (const auto& block : nestedBlocks_) {
        if (block.GetType() == type) {
            matches.push_back(block);
        }
    }

    return matches;
}

std::optional<AVCInfoBlock> AVCInfoBlock::FindNestedRecursive(uint16_t type) const {
    // Check immediate children first
    auto immediate = FindNested(type);
    if (immediate) {
        return immediate;
    }

    // Recursively search children's children
    for (const auto& child : nestedBlocks_) {
        auto recursive = child.FindNestedRecursive(type);
        if (recursive) {
            return recursive;
        }
    }

    return std::nullopt;
}

std::vector<AVCInfoBlock> AVCInfoBlock::FindAllNestedRecursive(uint16_t type) const {
    std::vector<AVCInfoBlock> matches;

    // Check immediate children
    for (const auto& block : nestedBlocks_) {
        if (block.GetType() == type) {
            matches.push_back(block);
        }
        // Also search recursively in each child
        auto childMatches = block.FindAllNestedRecursive(type);
        matches.insert(matches.end(), childMatches.begin(), childMatches.end());
    }

    return matches;
}

} // namespace ASFW::Protocols::AVC::Descriptors
