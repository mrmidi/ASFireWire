#include <gtest/gtest.h>

#include "Testing/HostDriverKitStubs.hpp"
#include "Async/Interfaces/IFireWireBus.hpp"
#include "Common/WireFormat.hpp"
#include "Audio/Protocols/DICE/Core/DICETypes.hpp"
#include "Audio/Protocols/DICE/Core/DICETransaction.hpp"
#include "Audio/Protocols/DICE/Focusrite/SPro24DspProtocol.hpp"
#include "Audio/Protocols/DICE/TCAT/DICETcatProtocol.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ASFW::Audio::DICE::TCAT {

class DICETcatProtocolTestPeer {
public:
    static void CacheRuntimeCaps(DICETcatProtocol& protocol,
                                 const GlobalState& global,
                                 const StreamConfig& tx,
                                 const StreamConfig& rx) {
        protocol.CacheRuntimeCaps(global, tx, rx);
    }

    static bool MakeDiceClockConfiguration(const AudioClockConfig& requested,
                                           DiceClockConfiguration& out) {
        return DICETcatProtocol::MakeDiceClockConfiguration(requested, out);
    }
};

} // namespace ASFW::Audio::DICE::TCAT

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::IFireWireBus;
using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::DICE::ClockSource;
using ASFW::Audio::DICE::DecodeDiceNickname;
using ASFW::Audio::DICE::SplitDiceLabels;
using ASFW::Audio::DICE::ExtensionSections;
using ASFW::Audio::DICE::Focusrite::EffectGeneralParams;
using ASFW::Audio::DICE::GeneralSections;
using ASFW::Audio::DICE::DiceClockConfiguration;
using ASFW::Audio::DICE::Focusrite::SPro24DspProtocol;
using ASFW::Audio::DICE::Focusrite::kEffectGeneralOffset;
using ASFW::Audio::DICE::TCAT::DICETcatProtocol;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::LockOp;
using ASFW::FW::NodeId;

constexpr uint32_t kExtensionBaseLo = static_cast<uint32_t>(
    ASFW::Audio::DICE::DICEAbsoluteAddress(ASFW::Audio::DICE::kDICEExtensionOffset) & 0xFFFFFFFFULL);
constexpr uint32_t kDiceBaseLo = static_cast<uint32_t>(
    ASFW::Audio::DICE::DICEAbsoluteAddress(0) & 0xFFFFFFFFULL);
constexpr uint32_t kGlobalBaseLo = static_cast<uint32_t>(
    ASFW::Audio::DICE::DICEAbsoluteAddress(0x28) & 0xFFFFFFFFULL);
constexpr uint32_t kAppSectionQuadletOffset = 0x1FU;
constexpr uint32_t kAppSectionBaseLo = kExtensionBaseLo + (kAppSectionQuadletOffset * 4U);
constexpr uint32_t kGlobalReadBytes = 104U;
constexpr uint32_t kClockSelect48kInternal =
    (ASFW::Audio::DICE::ClockRateIndex::k48000 << ASFW::Audio::DICE::ClockSelect::kRateShift) |
    static_cast<uint32_t>(ClockSource::Internal);
constexpr uint32_t kLocked48kStatus =
    ASFW::Audio::DICE::StatusBits::kSourceLocked |
    (ASFW::Audio::DICE::ClockRateIndex::k48000 << ASFW::Audio::DICE::StatusBits::kNominalRateShift);

void PutBe32(uint8_t* dst, uint32_t value) {
    ASFW::FW::WriteBE32(dst, value);
}

std::array<uint8_t, ExtensionSections::kWireSize> MakeExtensionSectionsWire() {
    std::array<uint8_t, ExtensionSections::kWireSize> bytes{};
    const std::array<uint32_t, 18> quadlets{
        0x13, 0x04,  // caps
        0x17, 0x02,  // command
        0x19, 0x10,  // mixer
        0x1A, 0x10,  // peak
        0x1B, 0x20,  // router
        0x1C, 0x40,  // stream format
        0x1D, 0x80,  // current config
        0x1E, 0x40,  // standalone
        kAppSectionQuadletOffset, 0x100,  // application
    };

    for (size_t index = 0; index < quadlets.size(); ++index) {
        PutBe32(bytes.data() + (index * sizeof(uint32_t)), quadlets[index]);
    }
    return bytes;
}

std::array<uint8_t, GeneralSections::kWireSize> MakeGeneralSectionsWire() {
    std::array<uint8_t, GeneralSections::kWireSize> bytes{};
    PutBe32(bytes.data() + 0x00, 0x0000000A);
    PutBe32(bytes.data() + 0x04, 0x0000005F);
    PutBe32(bytes.data() + 0x08, 0x00000069);
    PutBe32(bytes.data() + 0x0C, 0x00000046);
    PutBe32(bytes.data() + 0x10, 0x000000F7);
    PutBe32(bytes.data() + 0x14, 0x00000046);
    return bytes;
}

std::array<uint8_t, kGlobalReadBytes> MakeGlobalStateWire(uint32_t clockSelect,
                                                          uint32_t status,
                                                          uint32_t extStatus,
                                                          uint32_t sampleRate,
                                                          uint32_t notification) {
    std::array<uint8_t, kGlobalReadBytes> bytes{};
    PutBe32(bytes.data() + ASFW::Audio::DICE::GlobalOffset::kNotification, notification);
    PutBe32(bytes.data() + ASFW::Audio::DICE::GlobalOffset::kClockSelect, clockSelect);
    PutBe32(bytes.data() + ASFW::Audio::DICE::GlobalOffset::kStatus, status);
    PutBe32(bytes.data() + ASFW::Audio::DICE::GlobalOffset::kExtStatus, extStatus);
    PutBe32(bytes.data() + ASFW::Audio::DICE::GlobalOffset::kSampleRate, sampleRate);
    PutBe32(bytes.data() + ASFW::Audio::DICE::GlobalOffset::kVersion, 0x01000C00U);
    PutBe32(bytes.data() + ASFW::Audio::DICE::GlobalOffset::kClockCaps, 0x00001E06U);
    return bytes;
}

class CountingFireWireBus final : public IFireWireBus {
public:
    AsyncHandle ReadBlock(Generation generation,
                          NodeId nodeId,
                          FWAddress address,
                          uint32_t length,
                          FwSpeed speed,
                          ASFW::Async::InterfaceCompletionCallback callback) override {
        (void)nodeId;
        (void)speed;
        ++readCount;
        if (generation != generation_) {
            callback(AsyncStatus::kStaleGeneration, {});
            return NextHandle();
        }

        std::vector<uint8_t> payload(length, 0);
        if (address.addressHi == 0xFFFFU && address.addressLo == kDiceBaseLo &&
            length >= GeneralSections::kWireSize) {
            ++generalReadCount;
            const auto bytes = MakeGeneralSectionsWire();
            payload.assign(bytes.begin(), bytes.end());
        } else if (address.addressHi == 0xFFFFU && address.addressLo == kGlobalBaseLo &&
                   length >= kGlobalReadBytes) {
            ++globalReadCount;
            const auto bytes = MakeGlobalStateWire(clockSelect_, status_, extStatus_, sampleRate_, notification_);
            payload.assign(bytes.begin(), bytes.end());
        } else if (address.addressHi == 0xFFFFU && address.addressLo == kExtensionBaseLo &&
            length >= ExtensionSections::kWireSize) {
            ++extensionReadCount;
            const auto bytes = MakeExtensionSectionsWire();
            payload.assign(bytes.begin(), bytes.end());
        } else if (address.addressHi == 0xFFFFU &&
                   address.addressLo == (kAppSectionBaseLo + kEffectGeneralOffset) &&
                   length >= sizeof(uint32_t)) {
            ++appQuadReadCount;
            payload.resize(sizeof(uint32_t));
            PutBe32(payload.data(), 0U);
        }

        callback(AsyncStatus::kSuccess, std::span<const uint8_t>(payload.data(), payload.size()));
        return NextHandle();
    }

    AsyncHandle WriteBlock(Generation generation,
                           NodeId nodeId,
                           FWAddress address,
                           std::span<const uint8_t> data,
                           FwSpeed speed,
                           ASFW::Async::InterfaceCompletionCallback callback) override {
        (void)generation;
        (void)nodeId;
        (void)address;
        (void)data;
        (void)speed;
        ++writeCount;
        callback(AsyncStatus::kSuccess, {});
        return NextHandle();
    }

    AsyncHandle Lock(Generation generation,
                     NodeId nodeId,
                     FWAddress address,
                     LockOp lockOp,
                     std::span<const uint8_t> operand,
                     uint32_t responseLength,
                     FwSpeed speed,
                     ASFW::Async::InterfaceCompletionCallback callback) override {
        (void)generation;
        (void)nodeId;
        (void)address;
        (void)lockOp;
        (void)operand;
        (void)speed;
        ++lockCount;
        std::vector<uint8_t> payload(responseLength, 0);
        callback(AsyncStatus::kSuccess, std::span<const uint8_t>(payload.data(), payload.size()));
        return NextHandle();
    }

    bool Cancel(AsyncHandle handle) override {
        (void)handle;
        return false;
    }

    FwSpeed GetSpeed(NodeId nodeId) const override {
        (void)nodeId;
        return FwSpeed::S400;
    }

    uint32_t HopCount(NodeId nodeA, NodeId nodeB) const override {
        (void)nodeA;
        (void)nodeB;
        return 1;
    }

    Generation GetGeneration() const override { return generation_; }
    NodeId GetLocalNodeID() const override { return localNodeId_; }

    int readCount{0};
    int writeCount{0};
    int lockCount{0};
    int generalReadCount{0};
    int globalReadCount{0};
    int extensionReadCount{0};
    int appQuadReadCount{0};
    uint32_t clockSelect_{kClockSelect48kInternal};
    uint32_t status_{kLocked48kStatus};
    uint32_t extStatus_{0};
    uint32_t sampleRate_{48000};
    uint32_t notification_{0x20};

private:
    AsyncHandle NextHandle() {
        return AsyncHandle{static_cast<uint32_t>(nextHandle_++)};
    }

    Generation generation_{1};
    NodeId localNodeId_{0};
    uint64_t nextHandle_{1};
};

TEST(DICETcatProtocolTests, InitializeIsSideEffectFree) {
    CountingFireWireBus bus;
    DICETcatProtocol protocol(bus, bus, 2, nullptr);

    EXPECT_EQ(protocol.Initialize(), kIOReturnSuccess);

    AudioStreamRuntimeCaps caps{};
    EXPECT_FALSE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(bus.readCount, 0);
    EXPECT_EQ(bus.writeCount, 0);
    EXPECT_EQ(bus.lockCount, 0);
}

TEST(DICETcatProtocolTests, Neutral48kClockRequestMapsToDiceClockSelectInsideAdapter) {
    DiceClockConfiguration mapped{};
    EXPECT_TRUE(ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::MakeDiceClockConfiguration(
        AudioClockConfig{.sampleRateHz = 48000U}, mapped));
    EXPECT_EQ(mapped.sampleRateHz, 48000U);
    EXPECT_EQ(mapped.clockSelect, kClockSelect48kInternal);

    EXPECT_FALSE(ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::MakeDiceClockConfiguration(
        AudioClockConfig{.sampleRateHz = 44100U}, mapped));
}

TEST(DICETcatProtocolTests, RuntimeCapsAggregateTotalConfiguredStreams) {
    CountingFireWireBus bus;
    DICETcatProtocol protocol(bus, bus, 2, nullptr);

    ASFW::Audio::DICE::GlobalState global{};
    global.sampleRate = 48000;

    ASFW::Audio::DICE::StreamConfig tx{};
    tx.numStreams = 2;
    tx.streams[0].isoChannel = 1;
    tx.streams[0].pcmChannels = 10;
    tx.streams[0].midiPorts = 1;
    tx.streams[1].isoChannel = -1;
    tx.streams[1].pcmChannels = 6;

    ASFW::Audio::DICE::StreamConfig rx{};
    rx.numStreams = 2;
    rx.streams[0].isoChannel = 0;
    rx.streams[0].pcmChannels = 8;
    rx.streams[0].midiPorts = 1;
    rx.streams[1].isoChannel = -1;
    rx.streams[1].pcmChannels = 4;

    ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::CacheRuntimeCaps(protocol, global, tx, rx);

    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.sampleRateHz, 48000U);
    EXPECT_EQ(caps.hostInputPcmChannels, 16U);
    EXPECT_EQ(caps.deviceToHostAm824Slots, 17U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 12U);
    EXPECT_EQ(caps.hostToDeviceAm824Slots, 13U);
    EXPECT_EQ(caps.deviceToHostIsoChannel, 1U);
    EXPECT_EQ(caps.hostToDeviceIsoChannel, 0U);
}

TEST(DICETcatProtocolTests, ChannelLabelsFlattenAcrossStreamsInChannelOrder) {
    CountingFireWireBus bus;
    DICETcatProtocol protocol(bus, bus, 2, nullptr);

    // No labels before caps are cached.
    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    EXPECT_FALSE(protocol.GetChannelLabels(inNames, outNames));

    ASFW::Audio::DICE::GlobalState global{};
    global.sampleRate = 48000;

    // Host input == device TX; two streams, names concatenated in stream order.
    ASFW::Audio::DICE::StreamConfig tx{};
    tx.numStreams = 2;
    strlcpy(tx.streams[0].labels, "Mic 1\\Mic 2\\\\", sizeof(tx.streams[0].labels));
    strlcpy(tx.streams[1].labels, "Line 3\\Line 4\\\\", sizeof(tx.streams[1].labels));

    // Host output == device RX.
    ASFW::Audio::DICE::StreamConfig rx{};
    rx.numStreams = 1;
    strlcpy(rx.streams[0].labels, "Main L\\Main R\\\\", sizeof(rx.streams[0].labels));

    ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::CacheRuntimeCaps(protocol, global, tx, rx);

    ASSERT_TRUE(protocol.GetChannelLabels(inNames, outNames));
    ASSERT_EQ(inNames.size(), 4u);
    EXPECT_EQ(inNames[0], "Mic 1");
    EXPECT_EQ(inNames[1], "Mic 2");
    EXPECT_EQ(inNames[2], "Line 3");
    EXPECT_EQ(inNames[3], "Line 4");
    ASSERT_EQ(outNames.size(), 2u);
    EXPECT_EQ(outNames[0], "Main L");
    EXPECT_EQ(outNames[1], "Main R");
}

TEST(DICETcatProtocolTests, ReadDuplexHealthReturnsCurrentGlobalLockState) {
    CountingFireWireBus bus;
    DICETcatProtocol protocol(bus, bus, 2, nullptr);
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);

    ASFW::Audio::DICE::GlobalState global{};
    global.sampleRate = 48000;

    ASFW::Audio::DICE::StreamConfig tx{};
    tx.numStreams = 1;
    tx.streams[0].isoChannel = 1;
    tx.streams[0].pcmChannels = 16;

    ASFW::Audio::DICE::StreamConfig rx{};
    rx.numStreams = 1;
    rx.streams[0].isoChannel = 0;
    rx.streams[0].pcmChannels = 8;

    ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::CacheRuntimeCaps(protocol, global, tx, rx);

    bus.notification_ = ASFW::Audio::DICE::Notify::kLockChange;
    bus.status_ = kLocked48kStatus;
    bus.extStatus_ = 0x40U;

    std::optional<ASFW::Audio::DICE::DiceDuplexHealthResult> health;
    IOReturn healthStatus = kIOReturnError;
    protocol.ReadDuplexHealth([&](IOReturn status, ASFW::Audio::DICE::DiceDuplexHealthResult result) {
        healthStatus = status;
        health = result;
    });

    ASSERT_EQ(healthStatus, kIOReturnSuccess);
    ASSERT_TRUE(health.has_value());
    EXPECT_EQ(health->appliedClock.sampleRateHz, 48000U);
    EXPECT_TRUE(health->sourceLocked);
    EXPECT_EQ(health->nominalRateHz, 48000U);
    EXPECT_EQ(health->notification, ASFW::Audio::DICE::Notify::kLockChange);
    EXPECT_EQ(health->status, kLocked48kStatus);
    EXPECT_EQ(health->extStatus, 0x40U);
    EXPECT_EQ(health->runtimeCaps.sampleRateHz, 48000U);
    EXPECT_EQ(health->runtimeCaps.hostInputPcmChannels, 16U);
    EXPECT_EQ(health->runtimeCaps.hostOutputPcmChannels, 8U);
    EXPECT_EQ(bus.generalReadCount, 1);
    EXPECT_EQ(bus.globalReadCount, 1);
}

TEST(SPro24DspProtocolTests, VendorCallLoadsExtensionsLazily) {
    CountingFireWireBus bus;
    SPro24DspProtocol protocol(bus, bus, 2, nullptr);
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    EXPECT_EQ(bus.extensionReadCount, 0);

    std::optional<IOReturn> callbackStatus;
    protocol.GetEffectParams([&](IOReturn status, EffectGeneralParams /*params*/) {
        callbackStatus = status;
    });

    ASSERT_TRUE(callbackStatus.has_value());
    EXPECT_EQ(*callbackStatus, kIOReturnSuccess);
    EXPECT_EQ(bus.extensionReadCount, 1);
    EXPECT_EQ(bus.appQuadReadCount, 1);
}

// ---------------------------------------------------------------------------
// DICE nickname decode (little-endian within each big-endian wire quadlet).
// cross-validated with FFADO dice_avdevice.cpp:696.
// ---------------------------------------------------------------------------

// Encode a string the way a DICE device stores it: the first character of each
// quadlet sits in the least-significant byte, and the quadlet is transmitted
// big-endian on the wire — so the wire bytes are the characters reversed within
// each 4-byte group. `out` is the global-section payload; nickname starts at
// GlobalOffset::kNickname (0x0C).
std::vector<uint8_t> MakeNicknamePayload(const std::string& name) {
    std::vector<uint8_t> payload(0x0C + 64, 0);
    for (size_t q = 0; q * 4 < name.size() && q < 16; ++q) {
        uint8_t chars[4] = {0, 0, 0, 0};
        for (size_t b = 0; b < 4; ++b) {
            const size_t idx = q * 4 + b;
            chars[b] = (idx < name.size()) ? static_cast<uint8_t>(name[idx]) : 0;
        }
        const size_t base = 0x0C + q * 4;
        payload[base + 0] = chars[3];  // MSB on the wire = last char of group
        payload[base + 1] = chars[2];
        payload[base + 2] = chars[1];
        payload[base + 3] = chars[0];  // LSB on the wire = first char of group
    }
    return payload;
}

TEST(DiceNicknameTests, DecodesLittleEndianStringNotByteReversed) {
    // The Midas Venice regression: "Veni" must not decode as "ineV".
    const auto payload = MakeNicknamePayload("Venice F32");
    char out[64]{};
    DecodeDiceNickname(payload.data(), payload.size(), out);
    EXPECT_STREQ(out, "Venice F32");
}

TEST(DiceNicknameTests, ShortNameWithinFirstQuadletTerminates) {
    const auto payload = MakeNicknamePayload("Hi");
    char out[64]{};
    DecodeDiceNickname(payload.data(), payload.size(), out);
    EXPECT_STREQ(out, "Hi");
}

TEST(DiceNicknameTests, StopsAtPayloadBoundaryWithoutOverrun) {
    // Only one full quadlet of nickname present after the 0x0C offset.
    std::vector<uint8_t> payload(0x0C + 4, 0);
    payload[0x0C + 0] = 'i';  // wire bytes for "Veni" -> first 4 chars only
    payload[0x0C + 1] = 'n';
    payload[0x0C + 2] = 'e';
    payload[0x0C + 3] = 'V';
    char out[64]{};
    DecodeDiceNickname(payload.data(), payload.size(), out);
    EXPECT_STREQ(out, "Veni");
}

TEST(DiceNicknameTests, EmptyNicknameYieldsEmptyString) {
    const std::vector<uint8_t> payload(0x0C + 64, 0);
    char out[64]{};
    DecodeDiceNickname(payload.data(), payload.size(), out);
    EXPECT_STREQ(out, "");
}

// ---------------------------------------------------------------------------
// DICE channel-label splitting (FFADO splitNameString).
// ---------------------------------------------------------------------------

TEST(DiceLabelTests, SplitsSingleBackslashSeparatedNames) {
    const auto names = SplitDiceLabels("Mic 1\\Mic 2\\Line 3\\\\");
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "Mic 1");
    EXPECT_EQ(names[1], "Mic 2");
    EXPECT_EQ(names[2], "Line 3");
}

TEST(DiceLabelTests, StopsAtDoubleBackslashTerminator) {
    // Padding after the "\\\\" terminator must be ignored.
    const auto names = SplitDiceLabels("A\\B\\\\garbage\\more");
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "A");
    EXPECT_EQ(names[1], "B");
}

TEST(DiceLabelTests, PreservesLeadingEmptyTokenForChannelAlignment) {
    // A leading separator yields an empty first token (channel 0 unnamed).
    // (Two consecutive separators would form the "\\\\" terminator, so an
    // interior empty token cannot occur.)
    const auto names = SplitDiceLabels("\\A\\B\\\\");
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "");
    EXPECT_EQ(names[1], "A");
    EXPECT_EQ(names[2], "B");
}

TEST(DiceLabelTests, NullAndEmptyYieldNoNames) {
    EXPECT_TRUE(SplitDiceLabels(nullptr).empty());
    EXPECT_TRUE(SplitDiceLabels("").empty());
    EXPECT_TRUE(SplitDiceLabels("\\\\").empty());
}

TEST(DiceLabelTests, SingleNameWithoutTerminator) {
    const auto names = SplitDiceLabels("Solo");
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "Solo");
}

} // namespace
