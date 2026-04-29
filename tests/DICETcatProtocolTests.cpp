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
using ASFW::Audio::DICE::ClockSource;
using ASFW::Audio::DICE::ExtensionSections;
using ASFW::Audio::DICE::Focusrite::EffectGeneralParams;
using ASFW::Audio::DICE::GeneralSections;
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

TEST(DICETcatProtocolTests, ReadDuplexHealthReturnsCurrentGlobalLockState) {
    CountingFireWireBus bus;
    DICETcatProtocol protocol(bus, bus, 2, nullptr);
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);

    ASFW::Audio::DICE::GlobalState global{};
    global.sampleRate = 48000;

    ASFW::Audio::DICE::StreamConfig tx{};
    tx.numStreams = 1;
    tx.streams[0].pcmChannels = 16;

    ASFW::Audio::DICE::StreamConfig rx{};
    rx.numStreams = 1;
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
    EXPECT_EQ(health->appliedClock.clockSelect, kClockSelect48kInternal);
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

    caps = {};
    EXPECT_TRUE(TryGetKnownDICEProfile(0x000595U, 0x000000U, caps));
    EXPECT_EQ(caps.sampleRateHz, 48000U);
    EXPECT_EQ(caps.hostInputPcmChannels, 14U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 2U);
    EXPECT_EQ(caps.deviceToHostAm824Slots, 14U);
    EXPECT_EQ(caps.hostToDeviceAm824Slots, 2U);
}

} // namespace
