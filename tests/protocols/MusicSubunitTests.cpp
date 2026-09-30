//
// MusicSubunitTests.cpp
// ASFW Tests
//
// Tests for MusicSubunit integration (Capabilities Discovery)
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "Protocols/AVC/Music/MusicSubunit.hpp"
#include "Protocols/AVC/IAVCCommandSubmitter.hpp"
#include "Protocols/AVC/AVCDefs.hpp"
#include "Protocols/AVC/Core/IAvcUnit.hpp"

using namespace ASFW;
using namespace ASFW::Protocols::AVC;
using namespace ASFW::Protocols::AVC::Music;
using namespace ASFW::Protocols::AVC::StreamFormats;
using namespace testing;

// Mock IAVCCommandSubmitter & IAvcUnit
class MockAVCCommandSubmitter : public IAVCCommandSubmitter, public ASFW::AVC::IAvcUnit {
public:
    MOCK_METHOD(void, SubmitCommand, (const AVCCdb& cdb, AVCCompletion completion), (override));

    void Submit(const ASFW::AVC::CommandFrame& frame,
                ASFW::FW::Generation,
                ResponseCallback completion) override {
        AVCCdb cdb{};
        cdb.ctype = frame.Bytes()[0];
        cdb.subunit = frame.Bytes()[1];
        cdb.opcode = frame.Bytes()[2];
        cdb.operandLength = static_cast<uint16_t>(frame.Operands().size());
        std::copy(frame.Operands().begin(), frame.Operands().end(), cdb.operands.begin());

        SubmitCommand(cdb, [completion = std::move(completion)](AVCResult res, const AVCCdb& respCdb) {
            if (res != AVCResult::kAccepted && res != AVCResult::kImplementedStable) {
                completion(std::unexpected(ASFW::AVC::AvcError{ASFW::AVC::AvcErrorKind::kRefused}));
                return;
            }
            std::vector<uint8_t> respBytes;
            respBytes.push_back(respCdb.ctype);
            respBytes.push_back(respCdb.subunit);
            respBytes.push_back(respCdb.opcode);
            respBytes.insert(respBytes.end(), respCdb.operands.begin(), respCdb.operands.begin() + respCdb.operandLength);
            auto resp = ASFW::AVC::ParseResponse(respBytes);
            if (!resp) {
                completion(std::unexpected(resp.error()));
                return;
            }
            completion(*resp);
        });
    }

    ASFW::FW::NodeId NodeId() const noexcept override { return ASFW::FW::NodeId(0); }
    ASFW::FW::Generation CurrentGeneration() const noexcept override { return ASFW::FW::Generation(1); }
    uint64_t Guid() const noexcept override { return 0; }
    [[nodiscard]] ASFW::AVC::IAvcUnit* AsAvcUnit() noexcept override { return this; }
};

class MusicSubunitTests : public Test {
protected:
    std::shared_ptr<MusicSubunit> subunit;
    MockAVCCommandSubmitter mockSubmitter;

    void SetUp() override {
        // Create Music Subunit (Audio, ID 0)
        subunit = std::make_shared<MusicSubunit>(AVCSubunitType::kMusic0C, 0);
    }

    // Helper to access private plugs_ member (since fixture is friend)
    void AddPlug(ASFW::Protocols::AVC::Music::MusicSubunit& subunit, uint8_t id, ASFW::Protocols::AVC::StreamFormats::PlugDirection dir) {
        ASFW::Protocols::AVC::StreamFormats::PlugInfo plug;
        plug.plugID = id;
        plug.direction = dir;
        subunit.plugs_.push_back(plug);
    }

    void ParseBlock(ASFW::Protocols::AVC::Music::MusicSubunit& sub, const uint8_t* data, size_t len) {
        sub.ParseDescriptorBlock(data, len);
    }
};

// Test: QuerySupportedFormats should send 0xBF command
TEST_F(MusicSubunitTests, QuerySupportedFormats_Sends0xBF) {
    // Expect SubmitCommand to be called with 0xBF
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillRepeatedly(Invoke([](const AVCCdb& cdb, AVCCompletion completion) {
            // Verify Opcode is 0xBF (Stream Format Support)
            EXPECT_EQ(cdb.opcode, 0xBF);
            
            // Verify Subfunction is 0xC1 (Supported)
            EXPECT_EQ(cdb.operands[0], 0xC1);

            // Simulate a response
            AVCCdb response = cdb;
            response.ctype = static_cast<uint8_t>(AVCResponseType::kAccepted); // Accepted
            
            // Add a dummy format to the response so it stops iterating
            // Format: [0]=0x90 (AM824), [1]=0x40 (Compound), [2]=0x02 (48k), [3]=0x00...
            // Offset 7 for subunit plug
            response.operandLength = 7 + 4; 
            response.operands[7] = 0x90;
            response.operands[8] = 0x40;
            response.operands[9] = 0x02;
            response.operands[10] = 0x00;

            completion(AVCResult::kAccepted, response);
        }));

    bool done = false;
    subunit->QuerySupportedFormats(mockSubmitter, [&](bool success) {
        EXPECT_TRUE(success);
        done = true;
    });

    EXPECT_TRUE(done);
}

// Test: SetSampleRate should send 0xBF command with 0xC0 subfunction (Set)
// Note: Set format uses the same opcode (0xBF) but different subfunction/operands
// Actually, to set format, we use 0xC0 (Current) but with WRITE transaction?
// Or is it a CONTROL command?
// Extended Stream Format Spec says:
// To set format: CONTROL command with opcode 0xBF, subfunction 0xC0 (Current)
TEST_F(MusicSubunitTests, SetSampleRate_Sends0xBF_Control) {
    // Expect command submission
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            EXPECT_EQ(cdb.ctype, static_cast<uint8_t>(AVCCommandType::kControl));
            EXPECT_EQ(cdb.opcode, 0xBF); // Output Plug Signal Format
            EXPECT_EQ(cdb.operands[0], 0xC0); // Current
            
            // Verify plug address fields
            // [1]=Direction(1=Output), [2]=Type(1=Subunit), [3]=ID(0), [4]=Label(FF), [5]=Reserved(FF)
            EXPECT_EQ(cdb.operands[1], 0x01); // Output
            EXPECT_EQ(cdb.operands[2], 0x01); // Subunit plug
            EXPECT_EQ(cdb.operands[3], 0x00); // Plug 0
            
            // Verify format in operands (starts at offset 7, after 5-byte plug address and 1-byte support status)
            EXPECT_EQ(cdb.operands[6], 0xFF); // Support status (not used in control)
            EXPECT_EQ(cdb.operands[7], 0x90);
            EXPECT_EQ(cdb.operands[8], 0x40);
            EXPECT_EQ(cdb.operands[9], 0x04); // 48kHz
            
            // Simulate a response (ACCEPTED)
            AVCCdb response = cdb;
            response.ctype = static_cast<uint8_t>(AVCResponseType::kAccepted);
            completion(AVCResult::kAccepted, response);
        }));

    bool done = false;
    
    // Populate plugs_ so SetSampleRate has something to work with
    AddPlug(*subunit, 0, ASFW::Protocols::AVC::StreamFormats::PlugDirection::kOutput);

    subunit->SetSampleRate(mockSubmitter, 48000, [&](bool success) {
        EXPECT_TRUE(success);
        done = true;
    });

    EXPECT_TRUE(done);
}

// Test: QueryConnections should send 0x1A command for Input plugs
TEST_F(MusicSubunitTests, QueryConnections_Sends0x1A_Status) {
    // Add an Input plug (Destination)
    AddPlug(*subunit, 0, ASFW::Protocols::AVC::StreamFormats::PlugDirection::kInput);
    
    // Add an Output plug (Source) - should NOT be queried
    AddPlug(*subunit, 1, ASFW::Protocols::AVC::StreamFormats::PlugDirection::kOutput);

    // Expect command submission for Input plug only
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            EXPECT_EQ(cdb.ctype, static_cast<uint8_t>(AVCCommandType::kStatus));
            EXPECT_EQ(cdb.opcode, 0x1A); // SIGNAL SOURCE
            
            // Verify operands
            // [0]=0xFF (Output Status), [1]=0xFF, [2]=0xFF (Conv Data)
            // [3]=0x00 (Subunit Plug), [4]=0x00 (Plug ID 0)
            EXPECT_EQ(cdb.operands[0], 0xFF);
            EXPECT_EQ(cdb.operands[3], 0x00);
            EXPECT_EQ(cdb.operands[4], 0x00);
            
            // Simulate response: Connected to Unit Plug 0 (Iso)
            AVCCdb response = cdb;
            response.ctype = static_cast<uint8_t>(AVCResponseType::kImplementedStable); // Stable/Implemented
            
            // Response format:
            // [0]=OutputStatus, [1-2]=ConvData
            // [3]=SourcePlugType (0x01=Unit), [4]=SourcePlugID (0x00)
            // [5]=DestPlugType (0x00), [6]=DestPlugID (0x00)
            response.operandLength = 7;
            response.operands[3] = 0x01; // Unit plug
            response.operands[4] = 0x00; // Plug 0
            response.operands[5] = 0x00; // Subunit plug
            response.operands[6] = 0x00; // Plug 0
            
            completion(AVCResult::kAccepted, response);
        }));

    bool done = false;
    subunit->QueryConnections(mockSubmitter, [&](bool success) {
        EXPECT_TRUE(success);
        done = true;
    });

    EXPECT_TRUE(done);
}
// Test: QueryConnections should retry with Unit address if Subunit returns kNotImplemented
TEST_F(MusicSubunitTests, QueryConnections_RetryWithUnit) {
    // Add an Input plug
    AddPlug(*subunit, 0, ASFW::Protocols::AVC::StreamFormats::PlugDirection::kInput);

    // Expect TWO command submissions
    // 1. To Subunit (returns kNotImplemented)
    // 2. To Unit (returns kAccepted)

    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            // First call: To Subunit
            EXPECT_EQ(cdb.subunit, 0x60); // Music Subunit (0x0C << 3) | 0
            EXPECT_EQ(cdb.opcode, 0x1A);
            
            // Return Not Implemented
            completion(AVCResult::kNotImplemented, cdb);
        }))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            // Second call: To Unit
            EXPECT_EQ(cdb.subunit, 0xFF); // Unit Address (0xFF)
            EXPECT_EQ(cdb.opcode, 0x1A);
            
            // Verify operands (asking about Subunit Plug 0)
            // [3]=0x00 (Subunit Plug), [4]=0x00 (Plug ID 0)
            EXPECT_EQ(cdb.operands[3], 0x00);
            EXPECT_EQ(cdb.operands[4], 0x00);

            // Simulate response: Connected to Unit Plug 0
            AVCCdb response = cdb;
            response.ctype = static_cast<uint8_t>(AVCResponseType::kImplementedStable);
            response.operandLength = 7;
            response.operands[3] = 0x01; // Unit plug
            response.operands[4] = 0x00; // Plug 0
            
            completion(AVCResult::kAccepted, response);
        }));

    bool done = false;
    subunit->QueryConnections(mockSubmitter, [&](bool success) {
        EXPECT_TRUE(success);
        done = true;
    });

    EXPECT_TRUE(done);
    
    // Verify plug info updated
    auto plugs = subunit->GetPlugs();
    ASSERT_EQ(plugs.size(), 1);
    EXPECT_TRUE(plugs[0].connectionInfo.has_value());
    EXPECT_EQ(plugs[0].connectionInfo->sourceSubunitType, ASFW::Protocols::AVC::StreamFormats::SourceSubunitType::kUnit);
    EXPECT_EQ(plugs[0].connectionInfo->sourcePlugNumber, 0);
}

// Test: SetAudioVolume should send 0xB8 command to Audio Subunit (0x08)
TEST_F(MusicSubunitTests, SetAudioVolume_SendsCorrectCDB) {
    uint8_t plugId = 0x01;
    int16_t volume = 0x7FFF; // 0dB
    
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            EXPECT_EQ(cdb.ctype, static_cast<uint8_t>(AVCCommandType::kControl));
            // Target Audio Subunit 0 (0x01 << 3 | 0 = 0x08)
            EXPECT_EQ(cdb.subunit, 0x08); 
            EXPECT_EQ(cdb.opcode, 0xB8); // FUNCTION BLOCK
            
            // [0]=0x81 (Feature), [1]=PlugID, [2]=0x10 (Current), [3]=Len, [4]=Channel, [5]=Selector
            EXPECT_EQ(cdb.operands[0], 0x81);
            EXPECT_EQ(cdb.operands[1], plugId);
            EXPECT_EQ(cdb.operands[4], 0x00); // Channel
            EXPECT_EQ(cdb.operands[5], 0x02); // Volume Selector
            EXPECT_EQ(cdb.operands[6], 0x02); // Data length
            EXPECT_EQ(cdb.operands[7], 0x7F);
            EXPECT_EQ(cdb.operands[8], 0xFF);
            
            completion(AVCResult::kAccepted, cdb);
        }));
        
    bool done = false;
    subunit->SetAudioVolume(mockSubmitter, plugId, volume, [&](bool success) {
        EXPECT_TRUE(success);
        done = true;
    });
    
    EXPECT_TRUE(done);
}

// Test: ParseDescriptorBlock with captured Apogee Duet fixture
TEST_F(MusicSubunitTests, ParseDescriptorBlock_DuetFixtureIntegration) {
    const std::string duetHex =
        "01ce000a810000060101ffffffff01c08108000403030005002e8109000800900200000100020020810a000b060302000000ff000101ff000f000a000b416e616c6f67204f757400002d810900080190020500010002001f810a000b060302000200ff000301ff000e000a000a416e616c6f6720496e0000248109000802900203000100010016810a0007400901000400ff0009000a000553796e6300002d810900080090020000010002001f810a000b060302000200ff000301ff000e000a000a416e616c6f6720496e00002e8109000801900205000100020020810a000b060302000000ff000101ff000f000a000b416e616c6f67204f75740000248109000802900203000100010016810a0007400901000400ff0009000a000553796e63000025810b000e00000000f000ff00fff101ff00ff0011000a000d416e616c6f67204f75742031000025810b000e00000100f000ff01fff101ff01ff0011000a000d416e616c6f67204f75742032000024810b000e00000200f001ff00fff100ff00ff0010000a000c416e616c6f6720496e2031000024810b000e00000300f001ff01fff100ff01ff0010000a000c416e616c6f6720496e2032000012810b000e80000400f002ff00fff102ff00ff";

    std::vector<uint8_t> bytes;
    bytes.reserve(duetHex.size() / 2);
    for (size_t i = 0; i < duetHex.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoul(duetHex.substr(i, 2), nullptr, 16)));
    }
    ASSERT_EQ(bytes.size(), 464u);

    // Call ParseDescriptorBlock on MusicSubunit
    ParseBlock(*subunit, bytes.data(), bytes.size());

    // Verify completion status
    EXPECT_TRUE(subunit->HasCompleteDescriptorParse());

    // Verify plugs
    const auto& plugs = subunit->GetPlugs();
    ASSERT_EQ(plugs.size(), 6u);
    EXPECT_EQ(plugs[0].plugID, 0);
    EXPECT_EQ(plugs[0].direction, StreamFormats::PlugDirection::kInput);
    EXPECT_EQ(plugs[0].name, "Analog Out");

    // Verify channel details within plug format
    ASSERT_TRUE(plugs[0].currentFormat.has_value());
    ASSERT_FALSE(plugs[0].currentFormat->channelFormats.empty());
    const auto& chFormats = plugs[0].currentFormat->channelFormats[0];
    ASSERT_GE(chFormats.channels.size(), 2u);
    EXPECT_EQ(chFormats.channels[0].musicPlugID, 0);
    EXPECT_EQ(chFormats.channels[0].name, "Analog Out 1");
    EXPECT_EQ(chFormats.channels[1].musicPlugID, 1);
    EXPECT_EQ(chFormats.channels[1].name, "Analog Out 2");

    // Verify music channels
    const auto& channels = subunit->GetMusicChannels();
    ASSERT_EQ(channels.size(), 5u);
    EXPECT_EQ(channels[0].musicPlugID, 0);
    EXPECT_EQ(channels[0].name, "Analog Out 1");
    EXPECT_EQ(channels[1].musicPlugID, 1);
    EXPECT_EQ(channels[1].name, "Analog Out 2");
    EXPECT_EQ(channels[2].musicPlugID, 2);
    EXPECT_EQ(channels[2].name, "Analog In 1");
    EXPECT_EQ(channels[3].musicPlugID, 3);
    EXPECT_EQ(channels[3].name, "Analog In 2");

    // Verify parsed status access
    const auto& status = subunit->GetParsedStatus();
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->declaredLength, 462);
    EXPECT_TRUE(status->capabilities.hasGeneralCapability);
}

