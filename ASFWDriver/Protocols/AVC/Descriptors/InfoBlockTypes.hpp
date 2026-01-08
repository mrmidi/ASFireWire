//
// InfoBlockTypes.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Payload definitions and Reference Paths for AV/C Information Blocks.
// Specification: TA Document 1999045 - AV/C Information Block Types 1.0
//
// Dependencies: DescriptorTypes.hpp (for DescriptorSpecifier)
//

#pragma once

#include "DescriptorTypes.hpp"
#include <cstdint>
#include <vector>

namespace ASFW::Protocols::AVC {

//==============================================================================
// General Information Block Types
// Ref: Info Block Spec 1.0, Table 4.1
//==============================================================================
enum class InfoBlockType : uint16_t {
    kVendorSpecific      = 0x0000, ///< Format defined by specifier ID
    kSizeIndicator       = 0x0001, ///< Size of the object
    kPositionIndicator   = 0x0002, ///< Position of AV stream
    kPositionInfo        = 0x0003, ///< Describes position of data stream
    kTimeStamp_Creation  = 0x0004, ///< Content creation date/time
    kTimeStamp_Mod       = 0x0005, ///< Content modification date/time
    kCharacterCode       = 0x0008, ///< Character code of associated text
    kLanguageCode        = 0x0009, ///< Language code of associated text
    kRawText             = 0x000A, ///< Raw text bytes
    kName                = 0x000B, ///< Name of the entity (Title, Album, etc.)
    kDescription         = 0x000C, ///< Description of the entity
    kImage               = 0x000D, ///< Reference to digital still image
    kImageFormat         = 0x000E, ///< Format of digital still image
    kDescriptorRef       = 0x000F, ///< Encapsulates a descriptor_identifier
    kNumberOfItems       = 0x0010, ///< Item count in context
    kDescriptorCapacity  = 0x0011, ///< Storage characteristics
    
    // Music Subunit Specific (Uses Reserved Range 0x81xx)
    // Ref: Apple IOFireWireFamily - MusicSubunitInfoBlockTypeDescriptions
    // These utilize the reserved 0x81xx range for Music Subunit implementation
    kMusicGeneralStatus  = 0x8100, ///< General Music Subunit Status
    kMusicOutputPlug     = 0x8101, ///< Output Plug Info
    kMusicInputPlug      = 0x8102, ///< Input Plug Info
    kMusicAudioInfo      = 0x8103, ///< Audio Info Block
    kMusicMIDIInfo       = 0x8104, ///< MIDI Info Block
    kMusicSMPTEInfo      = 0x8105, ///< SMPTE Time Code Info
    kMusicSampleCountInfo= 0x8106, ///< Sample Count Info
    kMusicAudioSyncInfo  = 0x8107, ///< Audio SYNC Info
    kMusicRoutingStatus  = 0x8108, ///< Routing Status Info
    kMusicSubunitPlugInfo= 0x8109, ///< Subunit Plug Info (contains plug details + nested name blocks)
    kClusterInfo         = 0x810A, ///< Cluster Info (often contains name)
    kMusicPlugInfo       = 0x810B, ///< Music Plug Info (individual channel names e.g. "Analog Out 1")
    
    // Subunit Specific Ranges (Annex A)
    kDiscSubunitStart    = 0x8000, 
    kBulletinBoardStart  = 0x8900, 
    kCA_SubunitStart     = 0x9000  
};

//==============================================================================
// Character Code Types
// Ref: Info Block Spec 1.0, Table 4.22
//==============================================================================
enum class CharacterCodeType : uint8_t {
    kASCII       = 0x00,
    kISO_8859    = 0x01, // Requires type specific info (1 byte)
    kMS_JIS      = 0x02,
    kITTS        = 0x03,
    kKorean      = 0x04,
    kChinese     = 0x05,
    kISO_646     = 0x06,
    kShiftJIS    = 0x07,
    kJapaneseEUC = 0x08,
    kMDSpecific  = 0x80
};

//==============================================================================
// Language Code Types
// Ref: Info Block Spec 1.0, Table 4.31
//==============================================================================
enum class LanguageCodeType : uint8_t {
    kEBU_Tech_3258 = 0x00, // 1 byte specific info
    kISO_639       = 0x01  // 2 bytes specific info (e.g., "en", "jp")
};

//==============================================================================
// Info Block Reference Path Helper
// Ref: Descriptor Mech 1.2, Section 6.3
// Defined here because it depends on InfoBlockType logic
//==============================================================================

/// Represents the hierarchy path to reach a nested Info Block.
/// Level 0 is ALWAYS a Descriptor (List/Entry).
/// Level 1..n are Info Blocks.
struct InfoBlockReferencePath {
    DescriptorSpecifier rootDescriptor; // Level 0
    
    struct InfoBlockLevel {
        DescriptorSpecifierType type; // kInfoBlockType or kInfoBlockPos
        uint16_t infoBlockType;       // Used if type == 30h
        uint8_t instanceCount;        // Used if type == 30h
        uint8_t position;             // Used if type == 31h
    };
    
    std::vector<InfoBlockLevel> levels;

    /// Create a path starting at a specific Entry or List
    static InfoBlockReferencePath startingAt(const DescriptorSpecifier& root) {
        return InfoBlockReferencePath{root, {}};
    }

    /// Add a level navigating by Type (e.g., "The 0th Name Info Block")
    /// Ref: Descriptor Mech 1.2, Figure 35
    void addLevelByType(InfoBlockType type, uint8_t instance = 0) {
        InfoBlockLevel level;
        level.type = DescriptorSpecifierType::kInfoBlockType;
        level.infoBlockType = static_cast<uint16_t>(type);
        level.instanceCount = instance;
        level.position = 0; // Not used for this type
        levels.push_back(level);
    }

    /// Add a level navigating by Position (e.g., "The 2nd Info Block in the list")
    /// Ref: Descriptor Mech 1.2, Figure 36
    void addLevelByPosition(uint8_t position) {
        InfoBlockLevel level;
        level.type = DescriptorSpecifierType::kInfoBlockPos; // 31h
        level.infoBlockType = 0; // Not used for this type
        level.instanceCount = 0; // Not used for this type
        level.position = position;
        levels.push_back(level);
    }

    /// Build the raw byte sequence for command operands
    /// Ref: Descriptor Mech 1.2, Figure 34
    std::vector<uint8_t> buildPath() const {
        std::vector<uint8_t> data;
        
        // Number of levels = 1 (root) + info block levels
        data.push_back(static_cast<uint8_t>(1 + levels.size()));
        
        // Level[0]: The Root Descriptor Specifier
        auto rootBytes = rootDescriptor.buildSpecifier();
        data.insert(data.end(), rootBytes.begin(), rootBytes.end());
        
        // Level[1..n]: Info Block Specifiers
        for (const auto& level : levels) {
            data.push_back(static_cast<uint8_t>(level.type));
            
            if (level.type == DescriptorSpecifierType::kInfoBlockType) {
                // Type 30h: [Type (2 bytes)] + [Instance (1 byte)]
                data.push_back((level.infoBlockType >> 8) & 0xFF);
                data.push_back(level.infoBlockType & 0xFF);
                data.push_back(level.instanceCount);
            } else {
                // Type 31h: [Position (1 byte)]
                data.push_back(level.position);
            }
        }
        
        return data;
    }
};

} // namespace ASFW::Protocols::AVC
