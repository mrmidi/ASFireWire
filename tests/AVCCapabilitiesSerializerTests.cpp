//
//  AVCCapabilitiesSerializerTests.cpp
//  ASFW Tests
//
//  Tests for AVCHandler::SerializeMusicCapabilities logic
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "UserClient/Handlers/AVCHandler.hpp"
#include "Shared/SharedDataModels.hpp"
#include "Protocols/AVC/Music/MusicSubunit.hpp"
#include <DriverKit/IOUserClient.h>
#include <DriverKit/OSData.h>
#include <vector>

using namespace ASFW::UserClient;
using namespace ASFW::Protocols::AVC::Music;
using namespace ASFW::Protocols::AVC::StreamFormats;
using namespace ASFW::Shared;

class AVCCapabilitiesSerializerTests : public ::testing::Test {
protected:
    IOUserClientMethodArguments args{};
    
    void TearDown() override {
        if (args.structureOutput) {
            args.structureOutput->release();
            args.structureOutput = nullptr;
        }
    }
    
    MusicSubunit::PlugInfo CreatePlug(uint8_t id, PlugDirection dir, SampleRate rate, uint8_t chCount = 2, bool compound = false) {
        MusicSubunit::PlugInfo plug;
        plug.plugID = id;
        plug.direction = dir;
        plug.name = "TestPlug";
        
        AudioStreamFormat fmt;
        fmt.sampleRate = rate;
        fmt.totalChannels = chCount;
        fmt.subtype = compound ? AM824Subtype::kCompound : AM824Subtype::kSimple;
        
        if (compound) {
            ChannelFormatInfo chFmt;
            chFmt.formatCode = StreamFormatCode::kMBLA; // MBLA
            chFmt.channelCount = chCount;
            fmt.channelFormats.push_back(chFmt);
        }
        
        plug.currentFormat = fmt;
        
        return plug;
    }
};

// Test 1: Simple AM824 Format (Raw) - Should synthesize 1 Block
TEST_F(AVCCapabilitiesSerializerTests, Serialization_SimpleFormat_CreatesSignalBlock) {
    MusicSubunitCapabilities caps;
    caps.hasAudioCapability = true;
    caps.maxAudioInputChannels = 2;
    
    std::vector<MusicSubunit::PlugInfo> plugs;
    plugs.push_back(CreatePlug(0, PlugDirection::kInput, SampleRate::k48000Hz, 2, false));
    
    std::vector<MusicSubunit::MusicPlugChannel> channels;
    
    kern_return_t ret = AVCHandler::SerializeMusicCapabilities(caps, plugs, channels, &args);
    
    EXPECT_EQ(ret, kIOReturnSuccess);
    ASSERT_NE(args.structureOutput, nullptr);
    
    // Parse result
    const uint8_t* ptr = static_cast<const uint8_t*>(args.structureOutput->getBytesNoCopy());
    auto* wire = reinterpret_cast<const AVCMusicCapabilitiesWire*>(ptr);
    
    EXPECT_EQ(wire->numPlugs, 1);
    
    // Check Plug Info
    size_t offset = sizeof(AVCMusicCapabilitiesWire);
    auto* plugWire = reinterpret_cast<const PlugInfoWire*>(ptr + offset);
    
    EXPECT_EQ(plugWire->numSignalBlocks, 1);
    
    // Check Signal Block
    offset += sizeof(PlugInfoWire);
    auto* blockWire = reinterpret_cast<const SignalBlockWire*>(ptr + offset);
    
    EXPECT_EQ(blockWire->formatCode, 0x06); // MBLA Default for Simple
    EXPECT_EQ(blockWire->channelCount, 2);
}

// Test 1.5: Plug Type Serialization
TEST_F(AVCCapabilitiesSerializerTests, Serialization_PlugType) {
    // Setup test data
    MusicSubunitCapabilities caps;
    caps.hasAudioCapability = true;
    caps.maxAudioInputChannels = 2;
    caps.maxAudioOutputChannels = 2;

    std::vector<MusicSubunit::PlugInfo> plugs;
    
    MusicSubunit::PlugInfo p1;
    p1.plugID = 0;
    p1.direction = PlugDirection::kInput;
    p1.type = MusicPlugType::kAudio; // Explicitly set type
    p1.name = "TestIn";
    // Simple formats usually have 1 "block" of channels associated, handled inside Serialize logic via currentFormat->totalChannels
    // But for this test, let's assume currentFormat is set or we rely on default behavior
    // If currentFormat is null, numSignalBlocks is 0.
    
    MusicSubunit::PlugInfo p2;
    p2.plugID = 1;
    p2.direction = PlugDirection::kOutput;
    p2.type = MusicPlugType::kMIDI; // Explicitly set type
    p2.name = "TestOut";

    plugs.push_back(p1);
    plugs.push_back(p2);
    
    std::vector<MusicSubunit::MusicPlugChannel> channels;
    
    kern_return_t ret = AVCHandler::SerializeMusicCapabilities(caps, plugs, channels, &args);
    
    EXPECT_EQ(ret, kIOReturnSuccess);
    ASSERT_NE(args.structureOutput, nullptr);
    
    // Verify properties
    auto* wire = reinterpret_cast<const AVCMusicCapabilitiesWire*>(args.structureOutput->getBytesNoCopy());
    EXPECT_EQ(wire->numPlugs, 2);

    // Verify Plug 0
    auto* plug0 = reinterpret_cast<const PlugInfoWire*>(static_cast<const uint8_t*>(args.structureOutput->getBytesNoCopy()) + sizeof(AVCMusicCapabilitiesWire));
    EXPECT_EQ(plug0->plugID, 0);
    EXPECT_EQ(plug0->isInput, 1);
    EXPECT_EQ(plug0->type, 0x00); // kAudio
    EXPECT_EQ(std::string(plug0->name), "TestIn");
    
    // Check next plug offset
    size_t plug0Size = sizeof(PlugInfoWire) + (plug0->numSignalBlocks * sizeof(SignalBlockWire));
    auto* plug1 = reinterpret_cast<const PlugInfoWire*>(reinterpret_cast<const uint8_t*>(plug0) + plug0Size);
    
    EXPECT_EQ(plug1->plugID, 1);
    EXPECT_EQ(plug1->isInput, 0);
    EXPECT_EQ(plug1->type, 0x01); // kMIDI
    EXPECT_STREQ(plug1->name, "TestOut");
}

// Test 2: Global Rate Aggregation
TEST_F(AVCCapabilitiesSerializerTests, Serialization_AggregatesGlobalRates) {
    MusicSubunitCapabilities caps;
    
    std::vector<MusicSubunit::PlugInfo> plugs;
    
    // Plug 0: 48kHz current, Supports 44.1, 48
    auto p0 = CreatePlug(0, PlugDirection::kInput, SampleRate::k48000Hz, 2, false);
    p0.supportedFormats.push_back({.sampleRate = SampleRate::k44100Hz});
    p0.supportedFormats.push_back({.sampleRate = SampleRate::k48000Hz});
    plugs.push_back(p0);
    
    // Plug 1: 96kHz current, Supports 96, 48
    auto p1 = CreatePlug(1, PlugDirection::kInput, SampleRate::k96000Hz, 2, false);
    p1.supportedFormats.push_back({.sampleRate = SampleRate::k96000Hz});
    p1.supportedFormats.push_back({.sampleRate = SampleRate::k48000Hz});
    plugs.push_back(p1);
    
    std::vector<MusicSubunit::MusicPlugChannel> channels;
    
    kern_return_t ret = AVCHandler::SerializeMusicCapabilities(caps, plugs, channels, &args);
    
    EXPECT_EQ(ret, kIOReturnSuccess);
    
    const uint8_t* ptr = static_cast<const uint8_t*>(args.structureOutput->getBytesNoCopy());
    auto* wire = reinterpret_cast<const AVCMusicCapabilitiesWire*>(ptr);
    
    // Current Rate should be first valid one found (Plug 0 = 48kHz = 0x04)
    EXPECT_EQ(wire->currentRate, static_cast<uint8_t>(SampleRate::k48000Hz));
    
    // Supported Mask should include 44.1, 48, 96
    // 44.1=3, 48=4, 96=5
    // Mask vals: 1<<3=8, 1<<4=16, 1<<5=32. Sum: 56 (0x38)
    uint32_t expectedMask = (1 << 0x03) | (1 << 0x04) | (1 << 0x05); 
    EXPECT_EQ(wire->supportedRatesMask, expectedMask);
}

// Test 3: Compound Format - Should serialize defined blocks
TEST_F(AVCCapabilitiesSerializerTests, Serialization_CompoundFormat_UsesDefinedBlocks) {
    MusicSubunitCapabilities caps;
    
    std::vector<MusicSubunit::PlugInfo> plugs;
    
    // Plug 0: Compound (8ch MBLA + 2ch IEC60958)
    MusicSubunit::PlugInfo plug;
    plug.plugID = 0;
    plug.direction = PlugDirection::kInput;
    
    AudioStreamFormat fmt;
    fmt.sampleRate = SampleRate::k48000Hz;
    fmt.totalChannels = 10;
    fmt.subtype = AM824Subtype::kCompound; // Set subtype!
    
    ChannelFormatInfo b1;
    b1.formatCode = StreamFormatCode::kMBLA;
    b1.channelCount = 8;
    fmt.channelFormats.push_back(b1);
    
    ChannelFormatInfo b2;
    b2.formatCode = StreamFormatCode::kIEC60958_3;
    b2.channelCount = 2;
    fmt.channelFormats.push_back(b2);
    
    plug.currentFormat = fmt;
    plugs.push_back(plug);
    
    std::vector<MusicSubunit::MusicPlugChannel> channels;
    
    kern_return_t ret = AVCHandler::SerializeMusicCapabilities(caps, plugs, channels, &args);
    
    EXPECT_EQ(ret, kIOReturnSuccess);
    
    const uint8_t* ptr = static_cast<const uint8_t*>(args.structureOutput->getBytesNoCopy());
    auto* wire = reinterpret_cast<const AVCMusicCapabilitiesWire*>(ptr);
    
    EXPECT_EQ(wire->numPlugs, 1);
    
    size_t offset = sizeof(AVCMusicCapabilitiesWire);
    auto* plugWire = reinterpret_cast<const PlugInfoWire*>(ptr + offset);
    
    EXPECT_EQ(plugWire->numSignalBlocks, 2);
    
    offset += sizeof(PlugInfoWire);
    auto* blk1 = reinterpret_cast<const SignalBlockWire*>(ptr + offset);
    EXPECT_EQ(blk1->formatCode, static_cast<uint8_t>(StreamFormatCode::kMBLA));
    EXPECT_EQ(blk1->channelCount, 8);
    
    offset += sizeof(SignalBlockWire);
    auto* blk2 = reinterpret_cast<const SignalBlockWire*>(ptr + offset);
    EXPECT_EQ(blk2->formatCode, static_cast<uint8_t>(StreamFormatCode::kIEC60958_3));
    EXPECT_EQ(blk2->channelCount, 2);
}
