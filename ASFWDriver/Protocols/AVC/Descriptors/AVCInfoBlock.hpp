//
// AVCInfoBlock.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Info Block structure for AV/C Descriptor Mechanism
// Spec: TA Document 1999045 - AV/C Information Block Types Specification
//
// Info blocks are hierarchical data structures used in:
// - Identifier Descriptors (static device capabilities)
// - Status Descriptors (dynamic runtime status)
// - Object List Descriptors (device topology)
//
// Structure (per TA 1999045):
//   [0-1] compound_length (16-bit BE) - Total block size including nested blocks
//   [2-3] info_block_type (16-bit BE) - Type identifier
//   [4-5] primary_fields_length (16-bit BE) - Size of primary data only
//   [6...] primary_fields - Type-specific primary data
//   [...] nested_info_blocks - Optional nested blocks (recursive structure)
//

#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <expected>
#include "ParseReader.hpp"

namespace ASFW::Protocols::AVC::Descriptors {

/// AV/C Info Block - Hierarchical data structure from descriptor mechanism
/// Reference: TA Document 1999045, TA Document 2002013
class AVCInfoBlock {
public:
    [[nodiscard]] static Parsed<AVCInfoBlock> Parse(std::span<const uint8_t> bytes,
                                                   size_t& consumed,
                                                   size_t baseOffset = 0, size_t depth = 0);

    //==========================================================================
    // Accessors
    //==========================================================================

    /// Get total size of this block including all nested blocks
    uint16_t GetCompoundLength() const { return compoundLength_; }

    /// Get size of primary data only (excludes nested blocks)
    uint16_t GetPrimaryFieldsLength() const { return primaryFieldsLength_; }

    /// Get info block type identifier
    uint16_t GetType() const { return type_; }

    /// Get primary data (type-specific fields)
    const std::vector<uint8_t>& GetPrimaryData() const { return primaryData_; }

    /// Get nested info blocks (may be empty)
    const std::vector<AVCInfoBlock>& GetNestedBlocks() const { return nestedBlocks_; }

    /// Check if this block has nested blocks
    bool HasNestedBlocks() const { return !nestedBlocks_.empty(); }

    //==========================================================================
    // Navigation Helpers
    //==========================================================================

    /// Find first nested block of specified type
    /// @param type Info block type to search for
    /// @return Found block or std::nullopt if not found
    std::optional<AVCInfoBlock> FindNested(uint16_t type) const;

    /// Find all nested blocks of specified type (non-recursive, immediate children only)
    /// @param type Info block type to search for
    /// @return Vector of matching blocks (empty if none found)
    std::vector<AVCInfoBlock> FindAllNested(uint16_t type) const;

    /// Find first nested block of specified type (recursive search)
    /// Searches immediate children first, then recursively searches their children
    /// @param type Info block type to search for
    /// @return Found block or std::nullopt if not found
    std::optional<AVCInfoBlock> FindNestedRecursive(uint16_t type) const;

    /// Find all nested blocks of specified type (recursive search)
    /// Searches all levels of nesting, maintaining discovery order
    /// @param type Info block type to search for
    /// @return Vector of matching blocks (empty if none found)
    std::vector<AVCInfoBlock> FindAllNestedRecursive(uint16_t type) const;

    //==========================================================================
    // Construction
    //==========================================================================

    /// Default constructor (creates empty block)
    AVCInfoBlock() = default;

    /// Construct with parsed data (used internally by Parse)
    AVCInfoBlock(
        uint16_t compoundLength,
        uint16_t primaryFieldsLength,
        uint16_t type,
        std::vector<uint8_t> primaryData,
        std::vector<AVCInfoBlock> nestedBlocks
    );

private:
    uint16_t compoundLength_{0};        ///< Total size including nested blocks
    uint16_t primaryFieldsLength_{0};   ///< Size of primary data only
    uint16_t type_{0};                  ///< Info block type (TA 1999045 / Music Subunit Table 6.1)
    std::vector<uint8_t> primaryData_;  ///< Type-specific primary data
    std::vector<AVCInfoBlock> nestedBlocks_;  ///< Recursively parsed nested blocks

};

} // namespace ASFW::Protocols::AVC::Descriptors
