#include <gtest/gtest.h>
#include "ASFWDriver/Protocols/AVC/Music/MusicSubunit.hpp"
#include "ASFWDriver/Protocols/AVC/AVCDefs.hpp"
#include <vector>

using namespace ASFW::Protocols::AVC;
using namespace ASFW::Protocols::AVC::Music;

class MusicSubunitIdentifierParserTests : public ::testing::Test {
protected:
    MusicSubunitIdentifierParserTests() : subunit(AVCSubunitType::kMusic, 0) {}

    MusicSubunit subunit;

    void Parse(const std::vector<uint8_t>& data) {
        subunit.ParseMusicSubunitIdentifier(data.data(), data.size());
    }

    void ParseBlock(const std::vector<uint8_t>& data) {
        subunit.ParseDescriptorBlock(data.data(), data.size());
    }
    
    const MusicSubunitCapabilities& GetCaps() {
        return subunit.GetCapabilities();
    }

    std::vector<uint8_t> CreateBaseDescriptor(size_t specificInfoLen, uint8_t version = 0x10) {
        std::vector<uint8_t> data;
        
        // 1. Descriptor Header (8 bytes)
        // Length (placeholder, bytes 0-1)
        data.push_back(0x00); data.push_back(0x00);
        // Generation ID (0x02)
        data.push_back(0x02);
        // Sizes (ListID, ObjectID, EntryPos) - arbitrary non-zero values
        data.push_back(0x02); data.push_back(0x02); data.push_back(0x02);
        // Num Root Lists (0)
        data.push_back(0x00); data.push_back(0x00);
        
        // 2. Subunit Dependent Info Length (2 bytes)
        // Length of Music Subunit Header (6) + Specific Info Length (specificInfoLen)
        size_t musicHeaderLen = 6;
        size_t subunitDepLen = musicHeaderLen + specificInfoLen;
        data.push_back((subunitDepLen >> 8) & 0xFF);
        data.push_back(subunitDepLen & 0xFF);
        
        // 3. Music Subunit Header (6 bytes)
        // Length (2 bytes) - same as subunitDepLen
        data.push_back((subunitDepLen >> 8) & 0xFF);
        data.push_back(subunitDepLen & 0xFF);
        // Generation ID (0x00? Parser doesn't check this inner one strictly, but let's use 0x01)
        data.push_back(0x01);
        // Music Subunit Version
        data.push_back(version);
        // Specific Info Length (2 bytes)
        data.push_back((specificInfoLen >> 8) & 0xFF);
        data.push_back(specificInfoLen & 0xFF);
        
        // 4. Specific Info (Caller appends this)
        
        // Update total descriptor length (bytes 0-1)
        // Header(8) + SubunitDepLenField(2) + SubunitDepData(subunitDepLen)
        size_t totalLen = 8 + 2 + subunitDepLen;
        data[0] = (totalLen >> 8) & 0xFF;
        data[1] = totalLen & 0xFF;
        
        return data;
    }

};

TEST_F(MusicSubunitIdentifierParserTests, ParseTooShort) {
    std::vector<uint8_t> data = {0x00, 0x01}; // Too short
    Parse(data);
    // Should not crash, caps should be default
    EXPECT_FALSE(GetCaps().hasGeneralCapability);
}

TEST_F(MusicSubunitIdentifierParserTests, ParseBasicHeader) {
    // Create descriptor with 1 byte of specific info (just flags, 0x00)
    auto data = CreateBaseDescriptor(1, 0x10);
    data.push_back(0x00); // Capability flags: None
    
    Parse(data);
    
    EXPECT_EQ(GetCaps().musicSubunitVersion, 0x10);
    EXPECT_FALSE(GetCaps().hasGeneralCapability);
}

TEST_F(MusicSubunitIdentifierParserTests, ParseGeneralCapability) {
    // Specific Info: Flags(1) + GenCap(1+6) = 8 bytes
    auto data = CreateBaseDescriptor(8);
    
    // Capability Flags: General (Bit 0) = 0x01
    data.push_back(0x01);
    
    // General Capability Block
    data.push_back(0x06); // Length (6 bytes data)
    data.push_back(0x02); // Tx Flags (blocking = bit 1)
    data.push_back(0x01); // Rx Flags (non-blocking = bit 0)
    data.push_back(0x00); data.push_back(0x00); data.push_back(0x00); data.push_back(0x0A); // Latency (10)
    
    Parse(data);
    
    EXPECT_TRUE(GetCaps().hasGeneralCapability);
    EXPECT_TRUE(GetCaps().SupportsBlockingTransmit());
    EXPECT_TRUE(GetCaps().SupportsNonBlockingReceive());
    EXPECT_EQ(GetCaps().latencyCapability.value(), 10);
}

TEST_F(MusicSubunitIdentifierParserTests, ParseAudioCapability) {
    // Specific Info: Flags(1) + AudioCap(1+11) = 13 bytes
    auto data = CreateBaseDescriptor(13);
    
    // Capability Flags: Audio (Bit 1) = 0x02
    data.push_back(0x02);
    
    // Audio Capability Block
    data.push_back(0x0B); // Length (11 bytes: 5 header + 6 format)
    data.push_back(0x01); // Num Formats
    data.push_back(0x00); data.push_back(0x08); // Max In (8)
    data.push_back(0x00); data.push_back(0x08); // Max Out (8)
    
    // Format 1
    data.push_back(0x90); // IEC60958-3
    data.push_back(0x40); // 48kHz
    data.push_back(0x00); data.push_back(0x00); data.push_back(0x00); data.push_back(0x00);
    
    Parse(data);
    
    EXPECT_TRUE(GetCaps().hasAudioCapability);
    EXPECT_EQ(GetCaps().maxAudioInputChannels.value(), 8);
    EXPECT_EQ(GetCaps().maxAudioOutputChannels.value(), 8);
    ASSERT_TRUE(GetCaps().availableAudioFormats.has_value());
    ASSERT_EQ(GetCaps().availableAudioFormats.value().size(), 1);
    EXPECT_EQ(GetCaps().availableAudioFormats.value()[0].raw[0], 0x90);
}

TEST_F(MusicSubunitIdentifierParserTests, ParseMidiCapability) {
    // Specific Info: Flags(1) + MidiCap(1+6) = 8 bytes
    auto data = CreateBaseDescriptor(8);
    
    // Capability Flags: MIDI (Bit 2) = 0x04
    data.push_back(0x04);
    
    // MIDI Capability Block
    data.push_back(0x06);       // Length
    data.push_back(0x12);       // Version 1.2 (High=1, Low=2)
    data.push_back(0x00);       // Adapt Ver
    data.push_back(0x00); data.push_back(0x01); // Max In = 1
    data.push_back(0x00); data.push_back(0x01); // Max Out = 1
    
    Parse(data);
    
    EXPECT_TRUE(GetCaps().hasMidiCapability);
    EXPECT_EQ(GetCaps().midiVersionMajor.value(), 1);
    EXPECT_EQ(GetCaps().midiVersionMinor.value(), 2);
    EXPECT_EQ(GetCaps().maxMidiInputPorts.value(), 1);
}