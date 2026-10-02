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
#include <numeric>

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
    return outer.Section().and_then([&](ParseReader fields) -> Parsed<AVCInfoBlock> {
        uint16_t type{}, primaryLength{};
        return fields.Fields(type, primaryLength)
            .and_then([&] { return fields.Take(primaryLength); })
            .and_then([&](std::span<const uint8_t> primary) -> Parsed<AVCInfoBlock> {
                std::vector<AVCInfoBlock> children;
                while (fields.Remaining()) {
                    const auto childBase = fields.Offset();
                    auto remaining = fields.Take(fields.Remaining());
                    if (!remaining) return std::unexpected(remaining.error());
                    size_t used = 0;
                    auto child = Parse(*remaining, used, childBase, depth + 1);
                    if (!child) return std::unexpected(child.error());
                    children.push_back(std::move(*child));
                    // A cursor on the unconsumed suffix, keeping the absolute offset.
                    fields = ParseReader(remaining->subspan(used), std::add_sat(childBase, used));
                }
                consumed = outer.Offset() - baseOffset;
                return AVCInfoBlock(static_cast<uint16_t>(consumed - 2), primaryLength, type,
                                    std::vector<uint8_t>(primary.begin(), primary.end()), std::move(children));
            });
    });
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
    // Immediate children first, then each child's subtree in order. Depth is
    // bounded by kMaxInfoBlockDepth at parse time.
    const auto search = [type](this const auto& self, const AVCInfoBlock& block) -> std::optional<AVCInfoBlock> {
        if (auto immediate = block.FindNested(type)) return immediate;
        for (const auto& child : block.nestedBlocks_)
            if (auto found = self(child)) return found;
        return std::nullopt;
    };
    return search(*this);
}

std::vector<AVCInfoBlock> AVCInfoBlock::FindAllNestedRecursive(uint16_t type) const {
    std::vector<AVCInfoBlock> matches;
    // Pre-order: a match, then the matches inside it.
    const auto collect = [type, &matches](this const auto& self, const AVCInfoBlock& block) -> void {
        for (const auto& child : block.nestedBlocks_) {
            if (child.GetType() == type) matches.push_back(child);
            self(child);
        }
    };
    collect(*this);
    return matches;
}

} // namespace ASFW::Protocols::AVC::Descriptors
