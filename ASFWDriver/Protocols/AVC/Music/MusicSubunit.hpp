//
// MusicSubunit.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Music Subunit implementation (Audio/MIDI interfaces)
//

#pragma once

#include "../Subunit.hpp"
#include "MusicSubunitCapabilities.hpp"
#include "../Descriptors/AVCInfoBlock.hpp"
#include "../Descriptors/MusicSubunitDescriptor.hpp"
#include "../StreamFormats/StreamFormatTypes.hpp"
#include <span>
#include "../Discovery/DiscoverySnapshot.hpp"

class MusicSubunitIdentifierParserTests;
class MusicSubunitTests;

namespace ASFW::Protocols::AVC::Music {

class MusicSubunit : public Subunit {
public:
    MusicSubunit(AVCSubunitType type, uint8_t id);
    virtual ~MusicSubunit() = default;

    friend class ::MusicSubunitIdentifierParserTests;
    friend class ::MusicSubunitTests;

    /// Compatibility projection only; discovery owns all transport and facts.
    void LoadSnapshot(const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot& snapshot);

    /// Get human-readable name
    std::string GetName() const override { return "Music"; }

    /// Get capabilities
    const MusicSubunitCapabilities& GetCapabilities() const { return capabilities_; }

    // Use comprehensive PlugInfo from StreamFormats infrastructure
    using PlugInfo = StreamFormats::PlugInfo;

    const std::vector<PlugInfo>& GetPlugs() const { return plugs_; }

    //==========================================================================
    // Status Descriptor Support (Phase 3)
    //==========================================================================

    /// Get dynamic status info blocks (populated by ReadStatusDescriptor)
    const std::vector<::ASFW::Protocols::AVC::Descriptors::AVCInfoBlock>& GetDynamicStatus() const {
        return dynamicStatus_;
    }

    /// Get raw status descriptor data (if available)
    const std::optional<std::vector<uint8_t>>& GetStatusDescriptorData() const {
        return statusDescriptorData_;
    }

    /// Parsed status descriptor model (Phase 4b/4c)
    const std::optional<::ASFW::Protocols::AVC::Descriptors::MusicSubunitStatus>& GetParsedStatus() const noexcept {
        return parsedStatus_;
    }
    
    /// Individual channel info from MusicPlugInfo (0x810B) blocks
    /// These provide per-channel names like "Analog Out 1", "Analog In 2"
    struct MusicPlugChannel {
        uint16_t musicPlugID{0};    ///< Music Plug ID (maps to signal routing)
        uint8_t  portType{0};       ///< MusicPortType (e.g. Speaker=0x00, Line=0x03) or MusicPlugType (Sync=0x80) depending on device behavior
        std::string name;           ///< Channel name (e.g. "Analog Out 1")
    };
    
    /// Get individual music channel names (from MusicPlugInfo blocks)
    const std::vector<MusicPlugChannel>& GetMusicChannels() const { return musicChannels_; }

    /// Returns true if status descriptor parsing produced the minimum
    /// routing/plug data needed for reliable audio device creation.
    bool HasCompleteDescriptorParse() const noexcept;

private:
    MusicSubunitCapabilities capabilities_;
    std::vector<PlugInfo> plugs_;
    std::vector<::ASFW::Protocols::AVC::Descriptors::AVCInfoBlock> dynamicStatus_;  // Phase 3
    std::optional<std::vector<uint8_t>> statusDescriptorData_;
    std::optional<::ASFW::Protocols::AVC::Descriptors::MusicSubunitStatus> parsedStatus_;
    std::vector<MusicPlugChannel> musicChannels_;

    bool statusDescriptorReadOk_{false};
    bool statusDescriptorParsedOk_{false};
    bool statusDescriptorHasRouting_{false};
    bool statusDescriptorHasClusterInfo_{false};
    bool statusDescriptorHasPlugs_{false};
    uint16_t statusDescriptorExpectedPlugCount_{0};

private:
    /// Parse Music Subunit Identifier Descriptor
    /// Extracts static capabilities (General, Audio, MIDI, SMPTE, Sample Count, Audio SYNC)
    /// Spec: TA Document 2001007, Section 5.2
    /// @param data Raw descriptor data (starts at descriptor_length field)
    /// @param length Total descriptor data byte count
    /// @return Offset where info blocks start (after capability section), or 0 on error
    size_t ParseMusicSubunitIdentifier(const uint8_t* data, size_t length);
    
    /// Parse status descriptor block via MusicSubunitDescriptorParser
    void ParseDescriptorBlock(const uint8_t* data, size_t length);
    void ApplyMusicChannelNamesToPlugs();
    void UpdateCapabilitiesFromPlugs();
    [[nodiscard]] static uint16_t ChannelCountForFormat(const StreamFormats::AudioStreamFormat& format) noexcept;
};

} // namespace ASFW::Protocols::AVC::Music
