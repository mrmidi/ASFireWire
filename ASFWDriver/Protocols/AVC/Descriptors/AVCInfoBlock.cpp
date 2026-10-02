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


//==============================================================================
// AVCInfoBlock - Construction
//==============================================================================

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
AVCInfoBlock::AVCInfoBlock(
    uint16_t compoundLength, // NOLINT(bugprone-easily-swappable-parameters)
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

Parsed<AVCInfoBlock> AVCInfoBlock::Parse(std::span<const uint8_t> bytes,
                                             size_t& consumed, size_t baseOffset, size_t depth) {
    consumed = 0;
    if (depth >= kMaxInfoBlockDepth)
        return std::unexpected(ParseError{baseOffset, ParseErrorKind::BudgetExceeded});
    // Music Subunit Table 6.1: length excludes its own two bytes; type precedes
    // primary length. FFADO avc_descriptor_music.cpp:164-167 shares this header.
    ParseReader outer(bytes, baseOffset);
    auto fields = outer.Section(); if (!fields) return std::unexpected(fields.error());
    auto type = fields->BE16(); if (!type) return std::unexpected(type.error());
    auto primaryLength = fields->BE16(); if (!primaryLength) return std::unexpected(primaryLength.error());
    auto primary = fields->Take(*primaryLength); if (!primary) return std::unexpected(primary.error());
    std::vector<AVCInfoBlock> children;
    while (fields->Remaining()) {
        const auto childBase = fields->Offset();
        auto remaining = fields->Take(fields->Remaining());
        size_t used = 0;
        auto child = Parse(*remaining, used, childBase, depth + 1);
        if (!child) return std::unexpected(child.error());
        children.push_back(std::move(*child));
        // Rebuild a cursor on the unconsumed suffix, preserving absolute offset.
        *fields = ParseReader(remaining->subspan(used), childBase + used);
    }
    consumed = outer.Offset() - baseOffset;
    return AVCInfoBlock(static_cast<uint16_t>(consumed - 2), *primaryLength, *type,
                        std::vector<uint8_t>(primary->begin(), primary->end()), std::move(children));
}

std::expected<AVCInfoBlock, AVCResult> AVCInfoBlock::Parse(
    const uint8_t* bytes, size_t length, size_t& consumed) {
    auto result = Parse(std::span<const uint8_t>(bytes, length), consumed);
    if (!result) return std::unexpected(AVCResult::kInvalidResponse);
    return std::move(*result);
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
