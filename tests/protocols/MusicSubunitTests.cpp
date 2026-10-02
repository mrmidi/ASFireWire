//
// MusicSubunitTests.cpp
// ASFW Tests
//
// Tests for MusicSubunit integration (Capabilities Discovery)
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <array>
#include <functional>
#include <optional>
#include "Protocols/AVC/Music/MusicSubunit.hpp"
#include "Protocols/AVC/AVCDefs.hpp"
#include "Protocols/AVC/Core/IAvcUnit.hpp"

using namespace ASFW;
using namespace ASFW::Protocols::AVC;
using namespace ASFW::Protocols::AVC::Music;
using namespace ASFW::Protocols::AVC::StreamFormats;
using namespace testing;

// The frame a test sees and answers: ctype/response code, address, opcode, operands.
struct TestFrame {
    uint8_t ctype{0};
    uint8_t subunit{0xFF};
    uint8_t opcode{0};
    std::array<uint8_t, kAVCOperandMaxLength> operands{};
    size_t operandLength{0};
};
using TestRespond = std::function<void(AVCResult, const TestFrame&)>;

// An IAvcUnit whose every command is answered by the mocked SubmitCommand.
class MockAVCCommandSubmitter : public ASFW::AVC::IAvcUnit {
public:
    MOCK_METHOD(void, SubmitCommand, (const TestFrame& cdb, TestRespond completion));

    void Submit(const ASFW::AVC::CommandFrame& frame,
                ASFW::FW::Generation,
                ResponseCallback completion) override {
        TestFrame cdb{};
        cdb.ctype = frame.Bytes()[0];
        cdb.subunit = frame.Bytes()[1];
        cdb.opcode = frame.Bytes()[2];
        cdb.operandLength = static_cast<uint16_t>(frame.Operands().size());
        std::copy(frame.Operands().begin(), frame.Operands().end(), cdb.operands.begin());

        SubmitCommand(cdb, [completion = std::move(completion)](AVCResult res, const TestFrame& respCdb) {
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

// --- Lifecycle: a continuation outliving its unit or its subunit ---------------


TEST(LiveRefTests, YieldsNullOnceTheTargetIsGoneAndCopiesHaveTheirOwnLifetime) {
    struct Target {
        ASFW::Common::LifetimeAnchor anchor;
        [[nodiscard]] std::weak_ptr<const void> LifetimeToken() const noexcept { return anchor.Token(); }
    };
    auto first = std::make_unique<Target>();
    const ASFW::Common::LiveRef<Target> ref(*first);
    EXPECT_EQ(ref.Get(), first.get());

    auto copy = std::make_unique<Target>(*first);
    const ASFW::Common::LiveRef<Target> copyRef(*copy);
    first.reset();
    EXPECT_EQ(ref.Get(), nullptr);
    EXPECT_FALSE(ref);
    EXPECT_EQ(copyRef.Get(), copy.get()) << "a copy does not share the original's lifetime";
}
