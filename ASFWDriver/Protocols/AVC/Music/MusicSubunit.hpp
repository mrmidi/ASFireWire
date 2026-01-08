//
// MusicSubunit.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Music Subunit implementation (Audio/MIDI interfaces)
//

#pragma once

#include "../Subunit.hpp"
#include "../IAVCCommandSubmitter.hpp"
#include "MusicSubunitCapabilities.hpp"
#include "../Descriptors/AVCInfoBlock.hpp"
#include "../StreamFormats/StreamFormatTypes.hpp"

class MusicSubunitIdentifierParserTests;
class MusicSubunitTests;

namespace ASFW::Protocols::AVC::Music {

class MusicSubunit : public Subunit {
public:
    MusicSubunit(AVCSubunitType type, uint8_t id);
    virtual ~MusicSubunit() = default;

    friend class ::MusicSubunitIdentifierParserTests;
    friend class ::MusicSubunitTests;

    /// Parse capabilities
    void ParseCapabilities(AVCUnit& unit, std::function<void(bool)> completion) override;

    /// Get human-readable name
    std::string GetName() const override { return "Music"; }

    /// Get capabilities
    const MusicSubunitCapabilities& GetCapabilities() const { return capabilities_; }

    /// Query supported formats for all plugs (Phase 4)
    /// Enumerates the supported format list for each plug using STREAM FORMAT SUPPORT (0xC1)
    /// This populates PlugInfo.supportedFormats
    void QuerySupportedFormats(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, std::function<void(bool)> completion);

    /// Query connection topology for all plugs (Phase 4)
    /// Uses SIGNAL SOURCE command (0x1A) to discover plug connections
    /// This populates PlugInfo.connectionInfo for destination plugs
    void QueryConnections(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, std::function<void(bool)> completion);

    /// Set sample rate for all plugs
    /// @param submitter Command submitter
    /// @param sampleRate Sample rate in Hz
    /// @param completion Callback with success/failure
    void SetSampleRate(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, uint32_t sampleRate, std::function<void(bool)> completion);

    /// Set volume for a function block (plug) targeting Audio Subunit (0x01)
    /// @param submitter Command submitter
    /// @param plugId Plug ID (Function Block ID)
    /// @param volume Volume level (0x7FFF = 0dB, etc.)
    /// @param completion Callback
    void SetAudioVolume(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, uint8_t plugId, int16_t volume, std::function<void(bool)> completion);

    /// Set mute for a function block (plug) targeting Audio Subunit (0x01)
    /// @param submitter Command submitter
    /// @param plugId Plug ID (Function Block ID)
    /// @param mute True to mute, false to unmute
    /// @param completion Callback
    void SetAudioMute(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, uint8_t plugId, bool mute, std::function<void(bool)> completion);

    // Use comprehensive PlugInfo from StreamFormats infrastructure
    using PlugInfo = StreamFormats::PlugInfo;

    const std::vector<PlugInfo>& GetPlugs() const { return plugs_; }

    //==========================================================================
    // Status Descriptor Support (Phase 3)
    //==========================================================================

    /// Read dynamic Status Descriptor (type 0x80)
    /// Spec: TA Document 2001007, Section 5.3
    /// @param unit AV/C unit for command submission
    /// @param completion Callback with result (true=success, false=failure)
    void ReadStatusDescriptor(AVCUnit& unit, std::function<void(bool)> completion);

    /// Get dynamic status info blocks (populated by ReadStatusDescriptor)
    const std::vector<::ASFW::Protocols::AVC::Descriptors::AVCInfoBlock>& GetDynamicStatus() const {
        return dynamicStatus_;
    }

    /// Get raw status descriptor data (if available)
    const std::optional<std::vector<uint8_t>>& GetStatusDescriptorData() const {
        return statusDescriptorData_;
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

private:
    MusicSubunitCapabilities capabilities_;
    std::vector<PlugInfo> plugs_;
    std::vector<::ASFW::Protocols::AVC::Descriptors::AVCInfoBlock> dynamicStatus_;  // Phase 3
    std::optional<std::vector<uint8_t>> statusDescriptorData_;
    std::vector<MusicPlugChannel> musicChannels_;

private:

    void ParseSignalFormats(AVCUnit& unit, std::function<void(bool)> completion);
    void QueryPlugFormats(AVCUnit& unit, size_t plugIndex, std::function<void(bool)> completion);
    void ParsePlugNames(AVCUnit& unit, std::function<void(bool)> completion);
    
    /// Parse Music Subunit Identifier Descriptor
    /// Extracts static capabilities (General, Audio, MIDI, SMPTE, Sample Count, Audio SYNC)
    /// Spec: TA Document 2001007, Section 5.2
    /// @param data Raw descriptor data (starts at descriptor_length field)
    /// @param length Total descriptor data byte count
    /// @return Offset where info blocks start (after capability section), or 0 on error
    size_t ParseMusicSubunitIdentifier(const uint8_t* data, size_t length);
    
    /// Helper to parse specific descriptor blocks
    void ParseDescriptorBlock(const uint8_t* data, size_t length);

    /// Helper to log connection info
    void LogConnection(size_t index, const StreamFormats::ConnectionInfo& info);
};

} // namespace ASFW::Protocols::AVC::Music
