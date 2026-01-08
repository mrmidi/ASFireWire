//
// DescriptorTypes.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Shared types, enums, and constants for AV/C Descriptor Mechanism
// Specification: TA Document 2002013 - AV/C Descriptor Mechanism 1.2
//

#pragma once

#include <cstdint>
#include <vector>
#include <stdexcept>

namespace ASFW::Protocols::AVC {

//==============================================================================
// Constants
//==============================================================================

/// Maximum chunk size for reading descriptor data (safe default for FCP payload)
constexpr uint16_t MAX_DESCRIPTOR_CHUNK_SIZE = 128;

//==============================================================================
// Generation ID
// Ref: Table 10 - Generation_ID values
//==============================================================================
enum class GenerationID : uint8_t {
    kAVC_3_0     = 0x00, ///< AV/C General Spec 3.0
    kAVC_3_0_Enh = 0x01, ///< AV/C General Spec 3.0 + Enhancements
    kDescriptor  = 0x02  ///< AV/C Descriptor Mechanism 1.0/1.1/1.2
};

//==============================================================================
// List Descriptor Attributes
// Ref: Table 11 - List descriptor attribute values
//==============================================================================
namespace ListAttributes {
    constexpr uint8_t kHasMoreAttributes   = 0x80; // Bit 7
    constexpr uint8_t kSkip                = 0x40; // Bit 6
    constexpr uint8_t kEntriesHaveObjectID = 0x10; // Bit 4
    constexpr uint8_t kUpToDate            = 0x08; // Bit 3
}

//==============================================================================
// Entry Descriptor Attributes
// Ref: Table 12 - Entry descriptor attribute values
//==============================================================================
namespace EntryAttributes {
    constexpr uint8_t kHasMoreAttributes = 0x80; // Bit 7
    constexpr uint8_t kSkip              = 0x40; // Bit 6
    constexpr uint8_t kHasChildID        = 0x20; // Bit 5
    constexpr uint8_t kUpToDate          = 0x08; // Bit 3
}

//==============================================================================
// Descriptor Specifier Types
// Ref: Table 14 - Descriptor_specifier_type meanings
//==============================================================================
enum class DescriptorSpecifierType : uint8_t {
    kUnitIdentifier     = 0x00,  ///< Reference (Sub)unit identifier descriptor
    kListID             = 0x10,  ///< Reference List descriptor - specified by list_ID
    kListType           = 0x11,  ///< Reference List descriptor - specified by list_type
    kEntryPosition      = 0x20,  ///< Reference Entry descriptor - specified by position
    kEntryObjectID      = 0x21,  ///< Reference Entry descriptor - specified by object_ID
    kEntryType          = 0x22,  ///< Create Entry descriptor - specified by entry_type
    kEntryObjectIDOnly  = 0x23,  ///< Reference Entry descriptor - specified by object_ID only
    kEntrySubunitObject = 0x24,  ///< Ref Entry by subunit specifier + root + type + object_ID
    kEntrySubunitObjOnly= 0x25,  ///< Ref Entry by subunit specifier + object_ID
    kInfoBlockType      = 0x30,  ///< Reference Info block - specified by type/instance
    kInfoBlockPos       = 0x31,  ///< Reference Info block - specified by position
    kSubunitDependent   = 0x80   ///< 0x80-0xBF: Subunit dependent descriptor
};

//==============================================================================
// Read Result Status
// Ref: Table 36 - read_result_status field values
//==============================================================================
enum class ReadResultStatus : uint8_t {
    kComplete           = 0x10,  ///< Complete read: The entire data was returned
    kMoreToRead         = 0x11,  ///< More to read: Only a portion was returned
    kDataLengthTooLarge = 0x12   ///< Data length too large: Less data exists than requested
};

//==============================================================================
// OPEN DESCRIPTOR Subfunctions
// Ref: Table 29 - Values of the subfunction operand
//==============================================================================
enum class OpenDescriptorSubfunction : uint8_t {
    kClose      = 0x00,  ///< Close: Relinquish use of the descriptor
    kReadOpen   = 0x01,  ///< Read open: Open for read-only access
    kWriteOpen  = 0x03   ///< Write open: Open for read or write access
};

//==============================================================================
// WRITE DESCRIPTOR Subfunctions
// Ref: Table 37 & Table 0.41
//==============================================================================
enum class WriteDescriptorSubfunction : uint8_t {
    kChange         = 0x10, ///< Overwrite specific part (not recommended)
    kReplace        = 0x20, ///< Overwrite complete descriptor
    kInsert         = 0x30, ///< Insert entry/descriptor
    kDelete         = 0x40, ///< Delete list/entry
    kPartialReplace = 0x50  ///< Replace/Insert/Delete portion of descriptor
};

//==============================================================================
// WRITE DESCRIPTOR Group Tag
// Ref: Table 38 - Group_tag values
//==============================================================================
enum class WriteGroupTag : uint8_t {
    kImmediate = 0x00, ///< Immediate write
    kFirst     = 0x01, ///< Begin grouped update
    kContinue  = 0x02, ///< Continue grouped update
    kLast      = 0x03  ///< Commit grouped update
};

//==============================================================================
// SEARCH DESCRIPTOR Parameters
// Ref: Tables 54, 55, 56, 58
//==============================================================================
enum class SearchInType : uint8_t {
    kListDescriptors  = 0x10,
    kEntryDescriptors = 0x20,
    kOtherDescriptors = 0x30,
    kListFieldOffset  = 0x50,
    kListTypeField    = 0x52,
    kEntryFieldOffset = 0x60,
    kEntryTypeField   = 0x62,
    kEntryChildListID = 0x64,
    kEntryObjectID    = 0x66,
    kOtherFieldOffset = 0x70
};

enum class SearchStartPointType : uint8_t {
    kAnywhere      = 0x00,
    kCurrentEntry  = 0x02,
    kLastResult    = 0x03,
    kListOffset    = 0x10,
    kListType      = 0x11,
    kEntryOffset   = 0x20,
    kEntryObjectID = 0x21
};

enum class SearchDirection : uint8_t {
    kDontCare      = 0x00,
    kUp            = 0x10,
    kUpByPosition  = 0x12,
    kUpByID        = 0x13,
    kDown          = 0x20,
    kDownByPosition= 0x22,
    kDownByID      = 0x23
};

enum class SearchResponseFormat : uint8_t {
    kDontCare      = 0x00,
    kListID        = 0x10,
    kListType      = 0x11,
    kEntryPosition = 0x20,
    kObjectID      = 0x21
};

//==============================================================================
// OBJECT NUMBER SELECT (ONS)
// Ref: Table 61 & Table 62
//==============================================================================
enum class ONSPlug : uint8_t {
    kDoNotOutput = 0xFE,
    kAnyPlug     = 0xFF
    // 0x00-0x1E are valid plug numbers
};

enum class ONSSubfunction : uint8_t {
    kClear   = 0xC0, ///< Stop output of all selections
    kRemove  = 0xD0, ///< Remove selection
    kAppend  = 0xD1, ///< Add selection to current output
    kReplace = 0xD2, ///< Replace current selection
    kNew     = 0xD3  ///< Output selection if plug is unused
};

//==============================================================================
// Descriptor Specifier Structure
// Ref: Section 6.1 Descriptor specifier
//==============================================================================

/// Represents the variable-length descriptor specifier used in operands.
/// Structure: [Descriptor Specifier Type (1 byte)] + [Type Specific Fields (Variable)]
/// 
/// CRITICAL: This is ONLY the operand payload. Subunit addressing is handled
/// in the FCP frame header (cdb.subunit), NOT in this structure.
struct DescriptorSpecifier {
    DescriptorSpecifierType type;
    std::vector<uint8_t> typeSpecificFields;

    /// Build the raw byte sequence for the command operand
    std::vector<uint8_t> buildSpecifier() const {
        std::vector<uint8_t> spec;
        spec.push_back(static_cast<uint8_t>(type));
        spec.insert(spec.end(), typeSpecificFields.begin(), typeSpecificFields.end());
        return spec;
    }

    /// Returns the total length of the specifier in bytes
    size_t size() const {
        return 1 + typeSpecificFields.size();
    }

    //==========================================================================
    // Factory Methods for Standard Specifiers (Section 6.2)
    //==========================================================================

    /// 6.2.1 (Sub)unit identifier descriptor specifier
    /// Structure: [00]
    static DescriptorSpecifier forUnitIdentifier() {
        return DescriptorSpecifier{DescriptorSpecifierType::kUnitIdentifier, {}};
    }

    /// 6.2.2 List descriptor specified by list ID
    /// Structure: [10] + [list ID (variable)]
    /// Note: length of list ID is defined in Unit Identifier (size_of_list_ID)
    static DescriptorSpecifier forListID(const std::vector<uint8_t>& listID) {
        return DescriptorSpecifier{DescriptorSpecifierType::kListID, listID};
    }

    /// 6.2.3 List descriptor specified by list_type
    /// Structure: [11] + [list_type (1 byte)]
    static DescriptorSpecifier forListType(uint8_t listType) {
        return DescriptorSpecifier{DescriptorSpecifierType::kListType, {listType}};
    }

    /// 6.2.4 Entry descriptor specified by position
    /// Structure: [20] + [list ID (variable)] + [entry position (variable)]
    /// Note: sizes defined in Unit Identifier
    static DescriptorSpecifier forEntryPosition(const std::vector<uint8_t>& listID, 
                                                const std::vector<uint8_t>& position) {
        std::vector<uint8_t> data = listID;
        data.insert(data.end(), position.begin(), position.end());
        return DescriptorSpecifier{DescriptorSpecifierType::kEntryPosition, data};
    }

    /// 6.2.5 Entry descriptor specified by object_ID
    /// Structure: [21] + [root list ID] + [list type] + [object ID]
    static DescriptorSpecifier forEntryObjectID(const std::vector<uint8_t>& rootListID,
                                                uint8_t listType,
                                                const std::vector<uint8_t>& objectID) {
        std::vector<uint8_t> data = rootListID;
        data.push_back(listType);
        data.insert(data.end(), objectID.begin(), objectID.end());
        return DescriptorSpecifier{DescriptorSpecifierType::kEntryObjectID, data};
    }
    
    /// 6.2.7 Entry descriptor specified only by object_ID
    /// Structure: [23] + [object ID]
    static DescriptorSpecifier forEntryObjectIDOnly(const std::vector<uint8_t>& objectID) {
        return DescriptorSpecifier{DescriptorSpecifierType::kEntryObjectIDOnly, objectID};
    }
};

} // namespace ASFW::Protocols::AVC
