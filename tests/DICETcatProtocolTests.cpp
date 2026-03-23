#include <gtest/gtest.h>

#include "Testing/HostDriverKitStubs.hpp"
#include "Async/Interfaces/IFireWireBus.hpp"
#include "Common/WireFormat.hpp"
#include "Protocols/Audio/DICE/Core/DICETypes.hpp"
#include "Protocols/Audio/DICE/Focusrite/SPro24DspProtocol.hpp"
#include "Protocols/Audio/DICE/TCAT/DICEKnownProfiles.hpp"
#include "Protocols/Audio/DICE/TCAT/DICETcatProtocol.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
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
};

} // namespace ASFW::Audio::DICE::TCAT

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::IFireWireBus;
using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::DICE::ExtensionSections;
using ASFW::Audio::DICE::Focusrite::EffectGeneralParams;
using ASFW::Audio::DICE::Focusrite::SPro24DspProtocol;
using ASFW::Audio::DICE::Focusrite::kEffectGeneralOffset;
using ASFW::Audio::DICE::TCAT::DICETcatProtocol;
using ASFW::Audio::DICE::TCAT::TryGetKnownDICEProfile;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::LockOp;
using ASFW::FW::NodeId;

constexpr uint32_t kExtensionBaseLo = static_cast<uint32_t>(
    ASFW::Audio::DICE::DICEAbsoluteAddress(ASFW::Audio::DICE::kDICEExtensionOffset) & 0xFFFFFFFFULL);
constexpr uint32_t kAppSectionQuadletOffset = 0x1FU;
constexpr uint32_t kAppSectionBaseLo = kExtensionBaseLo + (kAppSectionQuadletOffset * 4U);

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
        if (address.addressHi == 0xFFFFU && address.addressLo == kExtensionBaseLo &&
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
    int extensionReadCount{0};
    int appQuadReadCount{0};

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

TEST(DICETcatProtocolTests, RuntimeCapsAggregateAllConfiguredStreams) {
    CountingFireWireBus bus;
    DICETcatProtocol protocol(bus, bus, 2, nullptr);

    ASFW::Audio::DICE::GlobalState global{};
    global.sampleRate = 48000;

    ASFW::Audio::DICE::StreamConfig tx{};
    tx.numStreams = 2;
    tx.streams[0].pcmChannels = 10;
    tx.streams[0].midiPorts = 1;
    tx.streams[1].pcmChannels = 6;

    ASFW::Audio::DICE::StreamConfig rx{};
    rx.numStreams = 2;
    rx.streams[0].pcmChannels = 8;
    rx.streams[0].midiPorts = 1;
    rx.streams[1].pcmChannels = 4;

    ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::CacheRuntimeCaps(protocol, global, tx, rx);

    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.sampleRateHz, 48000U);
    EXPECT_EQ(caps.hostInputPcmChannels, 16U);
    EXPECT_EQ(caps.deviceToHostAm824Slots, 17U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 12U);
    EXPECT_EQ(caps.hostToDeviceAm824Slots, 13U);
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

TEST(DICEKnownProfilesTests, ReturnsKnownFocusriteProfiles) {
    AudioStreamRuntimeCaps caps{};
    EXPECT_TRUE(TryGetKnownDICEProfile(0x00130eU, 0x000009U, caps));
    EXPECT_EQ(caps.sampleRateHz, 48000U);
    EXPECT_EQ(caps.hostInputPcmChannels, 8U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 12U);
    EXPECT_EQ(caps.deviceToHostAm824Slots, 9U);
    EXPECT_EQ(caps.hostToDeviceAm824Slots, 13U);

    caps = {};
    EXPECT_TRUE(TryGetKnownDICEProfile(0x00130eU, 0x000007U, caps));
    EXPECT_EQ(caps.sampleRateHz, 48000U);
    EXPECT_EQ(caps.hostInputPcmChannels, 16U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 8U);
    EXPECT_EQ(caps.deviceToHostAm824Slots, 17U);
    EXPECT_EQ(caps.hostToDeviceAm824Slots, 9U);

    caps = {};
    EXPECT_TRUE(TryGetKnownDICEProfile(0x00130eU, 0x000008U, caps));
    EXPECT_EQ(caps.sampleRateHz, 48000U);
    EXPECT_EQ(caps.hostInputPcmChannels, 16U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 8U);
    EXPECT_EQ(caps.deviceToHostAm824Slots, 17U);
    EXPECT_EQ(caps.hostToDeviceAm824Slots, 9U);
}

} // namespace
