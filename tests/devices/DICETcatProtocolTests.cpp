#include <gtest/gtest.h>

#include "Testing/HostDriverKitStubs.hpp"
#include "Async/Interfaces/IFireWireBus.hpp"
#include "Common/WireFormat.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Audio/Protocols/DICE/Core/DICETypes.hpp"
#include "Audio/Protocols/DICE/Core/DICETransaction.hpp"
#include "Audio/Protocols/DICE/Focusrite/SPro24DspProtocol.hpp"
#include "Audio/Protocols/DICE/TCAT/DICETcatProtocol.hpp"

#include <array>
#include <atomic>
#include <thread>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ASFW::Audio::DICE::TCAT {

class DICETcatProtocolTestPeer {
public:
    static bool CacheRuntimeCaps(DICETcatProtocol& protocol,
                                 const GlobalState& global,
                                 const StreamConfig& tx,
                                 const StreamConfig& rx) {
        return protocol.CacheRuntimeCaps(global, tx, rx);
    }

    static bool UpdateOperationalCaps(DICETcatProtocol& protocol,
                                      const AudioStreamRuntimeCaps& caps) {
        return protocol.UpdateOperationalCaps(caps);
    }

    static bool HasDuplexState(const DICETcatProtocol& protocol) {
        return protocol.duplexCtrl_ &&
            (protocol.duplexCtrl_->IsPrepared() || protocol.duplexCtrl_->IsArmed() ||
             protocol.duplexCtrl_->IsRunning() || protocol.duplexCtrl_->IsOwnerClaimed());
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
using ASFW::Audio::DICE::TCAT::DICETcatRuntimePolicy;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::LockOp;
using ASFW::FW::NodeId;

struct RouteState {
    ASFW::Discovery::DeviceRegistry registry;
    ASFW::Discovery::DeviceRouteToken route{};

    RouteState() {
        ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = 0xD1CE000000000002ULL;
        rom.gen = Generation{1};
        rom.nodeId = 2;
        (void)registry.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
        route = *registry.CurrentRoute(rom.bib.guid);
    }
};

constexpr uint32_t kExtensionBaseLo = static_cast<uint32_t>(
    ASFW::Audio::DICE::DICEAbsoluteAddress(ASFW::Audio::DICE::kDICEExtensionOffset) & 0xFFFFFFFFULL);
constexpr uint32_t kDiceBaseLo = static_cast<uint32_t>(
    ASFW::Audio::DICE::DICEAbsoluteAddress(0) & 0xFFFFFFFFULL);
constexpr uint32_t kGlobalBaseLo = static_cast<uint32_t>(
    ASFW::Audio::DICE::DICEAbsoluteAddress(0x28) & 0xFFFFFFFFULL);
constexpr uint32_t kAppSectionQuadletOffset = 0x1FU;
constexpr uint32_t kAppSectionBaseLo = kExtensionBaseLo + (kAppSectionQuadletOffset * 4U);
constexpr uint32_t kGlobalReadBytes = 104U;
constexpr uint32_t kGlobalSectionBytes = 0x5FU * 4U;
constexpr uint32_t kTxBaseLo = kDiceBaseLo + 0x69U * 4U;
constexpr uint32_t kRxBaseLo = kDiceBaseLo + 0xF7U * 4U;
constexpr uint32_t kStreamSectionBytes = 70U * 4U;
constexpr uint32_t kClockSelect48kInternal =
    (ASFW::Audio::DICE::ClockRateIndex::k48000 << ASFW::Audio::DICE::ClockSelect::kRateShift) |
    static_cast<uint32_t>(ClockSource::Internal);
constexpr uint32_t kLocked48kStatus =
    ASFW::Audio::DICE::StatusBits::kSourceLocked |
    (ASFW::Audio::DICE::ClockRateIndex::k48000 << ASFW::Audio::DICE::StatusBits::kNominalRateShift);

void PutBe32(uint8_t* dst, uint32_t value) {
    ASFW::FW::WriteBE32(dst, value);
}

std::vector<uint8_t> MakeStreamSectionWire(
    bool isRx, uint32_t count, uint32_t pcm, uint32_t midi, uint32_t iso,
    uint32_t entryQuadlets = 70, uint32_t sectionBytes = kStreamSectionBytes,
    const char* labels = "") {
    std::vector<uint8_t> bytes(sectionBytes, 0);
    PutBe32(bytes.data(), count);
    PutBe32(bytes.data() + 4, entryQuadlets);
    PutBe32(bytes.data() + 8, iso);
    PutBe32(bytes.data() + 12, isRx ? 0 : pcm);
    PutBe32(bytes.data() + 16, isRx ? pcm : midi);
    PutBe32(bytes.data() + 20, isRx ? midi : 2);
    // TX_NAMES/RX_NAMES begin after the header and four stream quadlets.
    for (size_t i = 0; labels[i] != '\0' && 24 + i < bytes.size(); ++i) {
        bytes[24 + (i & ~size_t{3}) + (3 - (i & 3))] = labels[i];
    }
    return bytes;
}

AudioStreamRuntimeCaps RequiredTenChannelGeometry() {
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 10,
        .hostOutputPcmChannels = 10,
        .deviceToHostAm824Slots = 11,
        .hostToDeviceAm824Slots = 11,
        .sampleRateHz = 48000,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };
    caps.deviceToHostStreams[0] = {.pcmChannels = 10, .am824Slots = 11, .midiPorts = 1};
    caps.hostToDeviceStreams[0] = {.pcmChannels = 10, .am824Slots = 11, .midiPorts = 1};
    return caps;
}

DICETcatRuntimePolicy LowRateTenChannelPolicy() {
    return {.requiredRuntimeGeometry = RequiredTenChannelGeometry(),
            .allowedSampleRatesHz = {44100, 48000}};
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
        if (failedReadAddress_ && address.addressLo == *failedReadAddress_) {
            callback(AsyncStatus::kTimeout, {});
            return NextHandle();
        }

        std::vector<uint8_t> payload(length, 0);
        if (address.addressHi == 0xFFFFU && address.addressLo == kDiceBaseLo &&
            length >= GeneralSections::kWireSize) {
            ++generalReadCount;
            auto bytes = MakeGeneralSectionsWire();
            PutBe32(bytes.data() + 0x14, rxSectionBytes_ / 4);
            payload.assign(bytes.begin(), bytes.end());
        } else if (address.addressHi == 0xFFFFU && address.addressLo >= kGlobalBaseLo &&
                   address.addressLo + length <= kGlobalBaseLo + kGlobalSectionBytes) {
            ++globalReadCount;
            const auto core = MakeGlobalStateWire(clockSelect_, status_, extStatus_, sampleRate_, notification_);
            std::vector<uint8_t> bytes(kGlobalSectionBytes, 0);
            std::copy(core.begin(), core.end(), bytes.begin());
            ASFW::FW::WriteBE64(bytes.data(), owner_);
            PutBe32(bytes.data() + ASFW::Audio::DICE::GlobalOffset::kEnable, enable_);
            const auto offset = address.addressLo - kGlobalBaseLo;
            payload.assign(bytes.begin() + offset, bytes.begin() + offset + length);
        } else if (address.addressHi == 0xFFFFU && address.addressLo >= kTxBaseLo &&
                   address.addressLo + length <= kTxBaseLo + kStreamSectionBytes) {
            const auto bytes = MakeStreamSectionWire(false, txCount_, txPcm_, txMidi_, txIso_,
                                                      70, kStreamSectionBytes, "Input 1\\Input 2\\\\");
            const auto offset = address.addressLo - kTxBaseLo;
            payload.assign(bytes.begin() + offset, bytes.begin() + offset + length);
        } else if (address.addressHi == 0xFFFFU && address.addressLo >= kRxBaseLo &&
                   address.addressLo + length <= kRxBaseLo + rxSectionBytes_) {
            const auto bytes = MakeStreamSectionWire(true, rxCount_, rxPcm_, rxMidi_, rxIso_,
                                                     rxEntryQuadlets_, rxSectionBytes_, "Output 1\\Output 2\\\\");
            const auto offset = address.addressLo - kRxBaseLo;
            payload.assign(bytes.begin() + offset, bytes.begin() + offset + length);
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

        if (deferredReadAddress_ && address.addressLo == *deferredReadAddress_) {
            deferredReadAddress_.reset();
            completeRead_ = [callback = std::move(callback), payload = std::move(payload)] {
                callback(AsyncStatus::kSuccess, payload);
            };
        } else {
            callback(AsyncStatus::kSuccess, std::span<const uint8_t>(payload.data(), payload.size()));
        }
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
        if (address.addressHi == 0xFFFFU && data.size() == 4) {
            const uint32_t value = ASFW::FW::ReadBE32(data.data());
            if (address.addressLo == kGlobalBaseLo + ASFW::Audio::DICE::GlobalOffset::kEnable) {
                enable_ = value;
                if (value != 0) ++enableWriteCount_;
            } else if (address.addressLo == kTxBaseLo + 8) {
                txIso_ = value;
                if (value != 0xFFFFFFFFU) ++activeIsoWriteCount_;
            } else if (address.addressLo == kRxBaseLo + 8) {
                rxIso_ = value;
                if (value != 0xFFFFFFFFU) ++activeIsoWriteCount_;
            } else if (address.addressLo == kGlobalBaseLo + ASFW::Audio::DICE::GlobalOffset::kClockSelect) {
                clockWrites_.push_back(value);
                if (applyClockWrites_) {
                    clockSelect_ = value;
                    const uint32_t rateIndex = (value >> 8) & 0xff;
                    if (rateIndex == 1 || rateIndex == 2) {
                        sampleRate_ = rateIndex == 1 ? 44100 : 48000;
                        status_ = 1U | (rateIndex << 8); // Locked, nominal rate.
                        notification_ = 0x20; // CLOCK_ACCEPTED.
                    }
                }
            }
        }
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
        if (address.addressHi == 0xFFFFU && address.addressLo == kGlobalBaseLo &&
            lockOp == LockOp::kCompareSwap && operand.size() == 16 && responseLength == 8) {
            ASFW::FW::WriteBE64(payload.data(), owner_);
            if (ASFW::FW::ReadBE64(operand.data()) == owner_) {
                owner_ = ASFW::FW::ReadBE64(operand.data() + 8);
            }
        }
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

    void SetGeneration(Generation generation) { generation_ = generation; }
    std::optional<uint32_t> deferredReadAddress_;
    std::function<void()> completeRead_;
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
    bool applyClockWrites_{false};
    std::vector<uint32_t> clockWrites_;
    std::optional<uint32_t> failedReadAddress_{};
    uint32_t txCount_{1};
    uint32_t rxCount_{1};
    uint32_t rxSectionBytes_{kStreamSectionBytes};
    uint32_t rxEntryQuadlets_{70};
    uint32_t txPcm_{10};
    uint32_t rxPcm_{10};
    uint32_t txMidi_{1};
    uint32_t rxMidi_{1};
    uint32_t txIso_{0xFFFFFFFFU};
    uint32_t rxIso_{0xFFFFFFFFU};
    uint64_t owner_{ASFW::Audio::DICE::kOwnerNoOwner};
    uint32_t enable_{0};
    uint32_t enableWriteCount_{0};
    uint32_t activeIsoWriteCount_{0};

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
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);

    EXPECT_EQ(protocol.Initialize(), kIOReturnSuccess);

    AudioStreamRuntimeCaps caps{};
    EXPECT_FALSE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(bus.readCount, 0);
    EXPECT_EQ(bus.writeCount, 0);
    EXPECT_EQ(bus.lockCount, 0);
}

TEST(DICETcatProtocolTests, RuntimeDiscoveryAcceptsCompleteInactiveStreamsWithoutWrites) {
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                              nullptr, {.requiredRuntimeGeometry = RequiredTenChannelGeometry()});
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    int completions = 0;
    protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
        ++completions;
        EXPECT_EQ(status, kIOReturnSuccess);
    });
    ASSERT_EQ(completions, 1);
    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 10U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 10U);
    EXPECT_EQ(caps.deviceToHostStreams[0].midiPorts, 1U);
    EXPECT_EQ(caps.hostToDeviceStreams[0].am824Slots, 11U);
    EXPECT_EQ(caps.deviceToHostIsoChannel, AudioStreamRuntimeCaps::kInvalidIsoChannel);
    const int reads = bus.readCount;
    protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
        ++completions;
        EXPECT_EQ(status, kIOReturnSuccess);
    });
    EXPECT_EQ(completions, 2);
    EXPECT_EQ(bus.readCount, reads);
    EXPECT_EQ(bus.writeCount, 0);
    EXPECT_EQ(bus.lockCount, 0);
}

TEST(DICETcatProtocolTests, RuntimeDiscoveryReadErrorsLeaveNoUsableCaps) {
    for (const uint32_t failedAddress : {kDiceBaseLo, kGlobalBaseLo, kTxBaseLo, kRxBaseLo}) {
        SCOPED_TRACE(failedAddress);
        CountingFireWireBus bus;
        bus.failedReadAddress_ = failedAddress;
        RouteState routeState;
        DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);
        ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
        int completions = 0;
        protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
            ++completions;
            EXPECT_NE(status, kIOReturnSuccess);
        });
        EXPECT_EQ(completions, 1);
        AudioStreamRuntimeCaps caps{};
        EXPECT_FALSE(protocol.GetRuntimeAudioStreamCaps(caps));
        EXPECT_EQ(bus.writeCount, 0);
        EXPECT_EQ(bus.lockCount, 0);
    }
}

TEST(DICETcatProtocolTests, RequiredGeometryRejectsMissingDeclaredStreamCoreAfterReadFailure) {
    CountingFireWireBus bus;
    bus.rxCount_ = 2;
    bus.rxEntryQuadlets_ = 128; // Core 2 begins at byte 520, beyond the first chunk.
    bus.rxSectionBytes_ = 8 + 2 * 512;
    bus.failedReadAddress_ = kRxBaseLo + 512;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                              nullptr, {.requiredRuntimeGeometry = RequiredTenChannelGeometry()});
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    int completions = 0;
    protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
        ++completions;
        EXPECT_NE(status, kIOReturnSuccess);
    });
    EXPECT_EQ(completions, 1);
    AudioStreamRuntimeCaps caps{};
    EXPECT_FALSE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(bus.writeCount, 0);
    EXPECT_EQ(bus.lockCount, 0);
}

TEST(DICETcatProtocolTests, RequiredGeometryAllowsMissingLabelsAndUnusedSectionTail) {
    for (bool missingLabelTail : {false, true}) {
        SCOPED_TRACE(missingLabelTail);
        CountingFireWireBus bus;
        // Only one stream is declared. Its core is complete in either case:
        // a section ending before labels, or a failed unused trailing chunk.
        bus.rxSectionBytes_ = missingLabelTail ? 24 : 1032;
        if (!missingLabelTail) bus.failedReadAddress_ = kRxBaseLo + 512;
        RouteState routeState;
        DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                                  nullptr, {.requiredRuntimeGeometry = RequiredTenChannelGeometry()});
        ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
        int completions = 0;
        protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
            ++completions;
            EXPECT_EQ(status, kIOReturnSuccess);
        });
        EXPECT_EQ(completions, 1);
        AudioStreamRuntimeCaps caps{};
        ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
        EXPECT_EQ(caps.hostToDeviceStreamCount, 1U);
        EXPECT_EQ(caps.hostToDeviceStreams[0].pcmChannels, 10U);
        EXPECT_EQ(caps.hostToDeviceStreams[0].midiPorts, 1U);
        EXPECT_EQ(bus.writeCount, 0);
        EXPECT_EQ(bus.lockCount, 0);
    }
}

TEST(DICETcatProtocolTests, RuntimeDiscoveryRejectsStreamCountBeyondParserCapacity) {
    CountingFireWireBus bus;
    bus.rxCount_ = 5;
    bus.rxEntryQuadlets_ = 4; // All five cores fit; truncation is not the reason to reject.
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    int completions = 0;
    protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
        ++completions;
        EXPECT_NE(status, kIOReturnSuccess);
    });
    EXPECT_EQ(completions, 1);
    AudioStreamRuntimeCaps caps{};
    EXPECT_FALSE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(bus.writeCount, 0);
    EXPECT_EQ(bus.lockCount, 0);
}

TEST(DICETcatProtocolTests, RuntimeDiscoveryRejectsZeroAndPartialGeometryAndCanRetry) {
    for (uint32_t failure = 0; failure < 4; ++failure) {
        SCOPED_TRACE(failure);
        CountingFireWireBus bus;
        if (failure == 0) bus.sampleRate_ = 0;
        if (failure == 1) bus.txCount_ = 0;
        if (failure == 2) bus.rxCount_ = 0;
        if (failure == 3) bus.rxPcm_ = 0;
        RouteState routeState;
        DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);
        ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
        int completions = 0;
        protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
            ++completions;
            EXPECT_NE(status, kIOReturnSuccess);
        });
        EXPECT_EQ(completions, 1);
        AudioStreamRuntimeCaps caps{};
        EXPECT_FALSE(protocol.GetRuntimeAudioStreamCaps(caps));

        bus.sampleRate_ = 48000;
        bus.txCount_ = bus.rxCount_ = 1;
        bus.rxPcm_ = 10;
        protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
            ++completions;
            EXPECT_EQ(status, kIOReturnSuccess);
        });
        EXPECT_EQ(completions, 2);
        EXPECT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
        EXPECT_EQ(bus.writeCount, 0);
        EXPECT_EQ(bus.lockCount, 0);
    }
}

TEST(DICETcatProtocolTests, RequiredGeometryRejectsRatePcmMidiAndStreamCountDifferences) {
    for (uint32_t failure = 0; failure < 6; ++failure) {
        SCOPED_TRACE(failure);
        CountingFireWireBus bus;
        auto required = RequiredTenChannelGeometry();
        if (failure == 0) bus.sampleRate_ = 44100;
        if (failure == 1) bus.txPcm_ = 9;
        if (failure == 2) bus.rxPcm_ = 9;
        // Two MIDI ports still occupy one wire slot, so this checks per-stream
        // metadata rather than only comparing aggregate PCM/DBS totals.
        if (failure == 3) bus.txMidi_ = 2;
        if (failure == 4) bus.rxMidi_ = 2;
        if (failure == 5) required.deviceToHostStreamCount = 2;
        RouteState routeState;
        DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                                  nullptr, {.requiredRuntimeGeometry = required});
        ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
        int completions = 0;
        protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
            ++completions;
            EXPECT_EQ(status, kIOReturnUnsupported);
        });
        EXPECT_EQ(completions, 1);
        AudioStreamRuntimeCaps caps{};
        EXPECT_FALSE(protocol.GetRuntimeAudioStreamCaps(caps));
        EXPECT_EQ(bus.writeCount, 0);
        EXPECT_EQ(bus.lockCount, 0);
    }
}

TEST(DICETcatProtocolTests, PrepareGeometryMismatchRollsBackOwnerBeforeCompletingOnce) {
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                              nullptr, {.requiredRuntimeGeometry = RequiredTenChannelGeometry()});
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
        ASSERT_EQ(status, kIOReturnSuccess);
    });
    bus.txPcm_ = 9; // A subsequent prepare must not trust the earlier capture.
    const uint64_t originalOwner = bus.owner_;
    int completions = 0;
    protocol.PrepareDuplex({}, AudioClockConfig{.sampleRateHz = 48000},
        [&](IOReturn status, ASFW::Audio::DICE::DiceDuplexPrepareResult) {
            ++completions;
            EXPECT_EQ(status, kIOReturnUnsupported);
            EXPECT_EQ(bus.owner_, originalOwner);
            EXPECT_FALSE(ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::HasDuplexState(protocol));
        });
    ASSERT_EQ(completions, 1);
    EXPECT_EQ(bus.lockCount, 2); // Claim and rollback compare/swap.
    EXPECT_EQ(bus.enable_, 0U);
    EXPECT_EQ(bus.enableWriteCount_, 0U);
    EXPECT_EQ(bus.activeIsoWriteCount_, 0U);
    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 10U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 10U);

    const int writes = bus.writeCount;
    int programCompletions = 0;
    protocol.ProgramRx([&](IOReturn status, ASFW::Audio::DICE::DiceDuplexStageResult) {
        ++programCompletions;
        EXPECT_NE(status, kIOReturnSuccess);
    });
    EXPECT_EQ(programCompletions, 1);
    EXPECT_EQ(bus.writeCount, writes);
}

TEST(DICETcatProtocolTests, RequiredGeometryRejectsOtherRateRequestsBeforeBusAccess) {
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                              nullptr, {.requiredRuntimeGeometry = RequiredTenChannelGeometry()});
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    int completions = 0;
    protocol.PrepareDuplex({}, AudioClockConfig{.sampleRateHz = 44100},
        [&](IOReturn status, ASFW::Audio::DICE::DiceDuplexPrepareResult) {
            ++completions;
            EXPECT_EQ(status, kIOReturnUnsupported);
        });
    protocol.ApplyClockConfig(AudioClockConfig{.sampleRateHz = 44100},
        [&](IOReturn status, ASFW::Audio::DICE::DiceClockApplyResult) {
            ++completions;
            EXPECT_EQ(status, kIOReturnUnsupported);
        });
    EXPECT_EQ(completions, 2);
    EXPECT_EQ(bus.readCount, 0);
    EXPECT_EQ(bus.writeCount, 0);
    EXPECT_EQ(bus.lockCount, 0);
}

TEST(DICETcatProtocolTests, RateAllowlistAcceptsBothLowRatesWithExactTenChannelGeometry) {
    for (const uint32_t rate : {44100U, 48000U}) {
        SCOPED_TRACE(rate);
        CountingFireWireBus bus;
        bus.sampleRate_ = rate;
        RouteState routeState;
        DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                                  nullptr, LowRateTenChannelPolicy());
        ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
        int completions = 0;
        protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
            ++completions;
            EXPECT_EQ(status, kIOReturnSuccess);
        });
        ASSERT_EQ(completions, 1);
        AudioStreamRuntimeCaps caps{};
        ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
        EXPECT_EQ(caps.sampleRateHz, rate);
        EXPECT_EQ(caps.hostInputPcmChannels, 10U);
        EXPECT_EQ(caps.hostOutputPcmChannels, 10U);
        EXPECT_EQ(caps.deviceToHostAm824Slots, 11U);
        EXPECT_EQ(caps.hostToDeviceAm824Slots, 11U);
        EXPECT_EQ(caps.deviceToHostStreamCount, 1U);
        EXPECT_EQ(caps.hostToDeviceStreamCount, 1U);
        EXPECT_EQ(caps.deviceToHostStreams[0].midiPorts, 1U);
        EXPECT_EQ(caps.hostToDeviceStreams[0].midiPorts, 1U);
        EXPECT_EQ(bus.writeCount, 0);
        EXPECT_EQ(bus.lockCount, 0);
    }
}

TEST(DICETcatProtocolTests, RateAllowlistRejectsOtherRequestedAndObservedRates) {
    for (const uint32_t rate : {0U, 32000U, 88200U, 96000U, 176400U, 192000U}) {
        SCOPED_TRACE(rate);
        CountingFireWireBus bus;
        bus.sampleRate_ = rate;
        RouteState routeState;
        DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                                  nullptr, LowRateTenChannelPolicy());
        ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
        int completions = 0;
        protocol.PrepareDuplex({}, AudioClockConfig{.sampleRateHz = rate},
            [&](IOReturn status, ASFW::Audio::DICE::DiceDuplexPrepareResult) {
                ++completions;
                EXPECT_EQ(status, kIOReturnUnsupported);
            });
        protocol.ApplyClockConfig(AudioClockConfig{.sampleRateHz = rate},
            [&](IOReturn status, ASFW::Audio::DICE::DiceClockApplyResult) {
                ++completions;
                EXPECT_EQ(status, kIOReturnUnsupported);
            });
        EXPECT_EQ(completions, 2);
        EXPECT_EQ(bus.readCount, 0);
        protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
            ++completions;
            EXPECT_NE(status, kIOReturnSuccess);
        });
        EXPECT_EQ(completions, 3);
        AudioStreamRuntimeCaps caps{};
        EXPECT_FALSE(protocol.GetRuntimeAudioStreamCaps(caps));
        EXPECT_EQ(bus.writeCount, 0);
        EXPECT_EQ(bus.lockCount, 0);
    }
}

TEST(DICETcatProtocolTests, RateAllowlistKeeps44100AfterIdleClockChangeAndRestores48000) {
    CountingFireWireBus bus;
    bus.applyClockWrites_ = true;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                              nullptr, LowRateTenChannelPolicy());
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
        ASSERT_EQ(status, kIOReturnSuccess);
    });
    int completions = 0;
    protocol.ApplyClockConfig(AudioClockConfig{.sampleRateHz = 44100},
        [&](IOReturn status, ASFW::Audio::DICE::DiceClockApplyResult result) {
            ++completions;
            EXPECT_EQ(status, kIOReturnSuccess);
            EXPECT_EQ(result.appliedClock.sampleRateHz, 44100U);
            EXPECT_EQ(result.runtimeCaps.sampleRateHz, 44100U);
        });
    ASSERT_EQ(completions, 1);
    EXPECT_EQ(bus.sampleRate_, 44100U);
    EXPECT_EQ(bus.owner_, ASFW::Audio::DICE::kOwnerNoOwner);
    ASSERT_FALSE(bus.clockWrites_.empty());
    EXPECT_EQ(bus.clockWrites_.back(), 0x0000010cU); // 44.1 kHz, Internal.

    // The legacy StartIO method name must not restore its old 48 kHz default.
    protocol.PrepareDuplex48k({}, [&](IOReturn status) {
        ++completions;
        EXPECT_EQ(status, kIOReturnSuccess);
    });
    ASSERT_EQ(completions, 2);
    EXPECT_EQ(bus.sampleRate_, 44100U);
    EXPECT_EQ(bus.clockWrites_.back(), 0x0000010cU);
    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.sampleRateHz, 44100U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 10U);
    ASSERT_EQ(protocol.StopDuplex(), kIOReturnSuccess);
    EXPECT_EQ(bus.owner_, ASFW::Audio::DICE::kOwnerNoOwner);

    protocol.ApplyClockConfig(AudioClockConfig{.sampleRateHz = 48000},
        [&](IOReturn status, ASFW::Audio::DICE::DiceClockApplyResult result) {
            ++completions;
            EXPECT_EQ(status, kIOReturnSuccess);
            EXPECT_EQ(result.runtimeCaps.sampleRateHz, 48000U);
        });
    EXPECT_EQ(completions, 3);
    EXPECT_EQ(bus.clockWrites_.back(), kClockSelect48kInternal);
    EXPECT_EQ(bus.sampleRate_, 48000U);
    EXPECT_EQ(bus.owner_, ASFW::Audio::DICE::kOwnerNoOwner);
    EXPECT_EQ(bus.enableWriteCount_, 0U);
    EXPECT_EQ(bus.activeIsoWriteCount_, 0U);
}

TEST(DICETcatProtocolTests, RateAllowlistStillRollsBackChangedWireGeometryAt44100) {
    for (uint32_t failure = 0; failure < 6; ++failure) {
        SCOPED_TRACE(failure);
        CountingFireWireBus bus;
        bus.sampleRate_ = 44100;
        bus.clockSelect_ = 0x0000010c;
        bus.status_ = 0x00000101;
        RouteState routeState;
        DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                                  nullptr, LowRateTenChannelPolicy());
        ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
        protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
            ASSERT_EQ(status, kIOReturnSuccess);
        });
        if (failure == 0) bus.txPcm_ = 9;
        if (failure == 1) bus.rxPcm_ = 9;
        if (failure == 2) bus.txMidi_ = 2;
        if (failure == 3) bus.rxMidi_ = 2;
        if (failure == 4) bus.txCount_ = 2;
        if (failure == 5) bus.rxCount_ = 2;
        const uint64_t originalOwner = bus.owner_;
        int completions = 0;
        protocol.PrepareDuplex({}, AudioClockConfig{.sampleRateHz = 44100},
            [&](IOReturn status, ASFW::Audio::DICE::DiceDuplexPrepareResult) {
                ++completions;
                EXPECT_NE(status, kIOReturnSuccess);
                EXPECT_EQ(bus.owner_, originalOwner);
                EXPECT_FALSE(ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::HasDuplexState(protocol));
            });
        ASSERT_EQ(completions, 1);
        EXPECT_EQ(bus.lockCount, 2); // Claim and rollback compare/swap.
        EXPECT_EQ(bus.enable_, 0U);
        EXPECT_EQ(bus.enableWriteCount_, 0U);
        EXPECT_EQ(bus.activeIsoWriteCount_, 0U);
        AudioStreamRuntimeCaps caps{};
        ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
        EXPECT_EQ(caps.hostInputPcmChannels, 10U);
        EXPECT_EQ(caps.hostOutputPcmChannels, 10U);
        const int writes = bus.writeCount;
        protocol.ProgramRx([&](IOReturn status, ASFW::Audio::DICE::DiceDuplexStageResult) {
            ++completions;
            EXPECT_NE(status, kIOReturnSuccess);
        });
        EXPECT_EQ(completions, 2);
        EXPECT_EQ(bus.writeCount, writes);
    }
}

TEST(DICETcatProtocolTests, FailedClockOperationsDoNotChangeLegacyStartRate) {
    for (const bool prepareFailure : {false, true}) {
        for (const bool geometryFailure : {false, true}) {
            SCOPED_TRACE(testing::Message() << "prepare=" << prepareFailure
                                           << " geometry=" << geometryFailure);
            CountingFireWireBus bus;
            bus.applyClockWrites_ = true;
            RouteState routeState;
            DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                                      nullptr, LowRateTenChannelPolicy());
            ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
            protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
                ASSERT_EQ(status, kIOReturnSuccess);
            });
            int completions = 0;
            protocol.ApplyClockConfig(AudioClockConfig{.sampleRateHz = 48000},
                [&](IOReturn status, ASFW::Audio::DICE::DiceClockApplyResult) {
                    ++completions;
                    ASSERT_EQ(status, kIOReturnSuccess);
                });
            ASSERT_EQ(completions, 1);
            if (geometryFailure) {
                bus.txPcm_ = 9; // Failure after the hardware clock changes.
            } else {
                // Failure before any clock programming or owner claim.
                bus.failedReadAddress_ = kGlobalBaseLo + ASFW::Audio::DICE::GlobalOffset::kStatus;
            }
            if (prepareFailure) {
                protocol.PrepareDuplex({}, AudioClockConfig{.sampleRateHz = 44100},
                    [&](IOReturn status, ASFW::Audio::DICE::DiceDuplexPrepareResult) {
                        ++completions;
                        EXPECT_NE(status, kIOReturnSuccess);
                    });
            } else {
                protocol.ApplyClockConfig(AudioClockConfig{.sampleRateHz = 44100},
                    [&](IOReturn status, ASFW::Audio::DICE::DiceClockApplyResult) {
                        ++completions;
                        EXPECT_NE(status, kIOReturnSuccess);
                    });
            }
            ASSERT_EQ(completions, 2);
            EXPECT_EQ(bus.sampleRate_, geometryFailure ? 44100U : 48000U);
            EXPECT_EQ(bus.owner_, ASFW::Audio::DICE::kOwnerNoOwner);
            const size_t clockWritesAfterFailure = bus.clockWrites_.size();
            bus.failedReadAddress_.reset();
            bus.txPcm_ = 10;

            // A later StartIO must use the prior successful selection, not
            // silently retry the failed request for 44.1 kHz.
            protocol.PrepareDuplex48k({}, [&](IOReturn status) {
                ++completions;
                EXPECT_EQ(status, kIOReturnSuccess);
            });
            ASSERT_EQ(completions, 3);
            EXPECT_EQ(bus.sampleRate_, 48000U);
            EXPECT_EQ(bus.clockSelect_, kClockSelect48kInternal);
            // A read failure left 48 kHz selected, so no redundant write is
            // needed. A post-clock geometry failure requires one return write.
            EXPECT_EQ(bus.clockWrites_.size(), clockWritesAfterFailure + (geometryFailure ? 1U : 0U));
            if (geometryFailure) {
                ASSERT_FALSE(bus.clockWrites_.empty());
                EXPECT_EQ(bus.clockWrites_.back(), kClockSelect48kInternal);
            }
            AudioStreamRuntimeCaps caps{};
            ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
            EXPECT_EQ(caps.sampleRateHz, 48000U);
            EXPECT_EQ(protocol.StopDuplex(), kIOReturnSuccess);
            EXPECT_EQ(bus.owner_, ASFW::Audio::DICE::kOwnerNoOwner);
        }
    }
}

TEST(DICETcatProtocolTests, FailedPrepareRetainsEarlierSuccessfulDiscovery) {
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
        ASSERT_EQ(status, kIOReturnSuccess);
    });
    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    bus.failedReadAddress_ = kDiceBaseLo;
    int completions = 0;
    protocol.PrepareDuplex({}, AudioClockConfig{.sampleRateHz = 48000},
        [&](IOReturn status, ASFW::Audio::DICE::DiceDuplexPrepareResult) {
            ++completions;
            EXPECT_NE(status, kIOReturnSuccess);
        });
    EXPECT_EQ(completions, 1);
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 10U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 10U);
    std::vector<std::string> inNames, outNames;
    ASSERT_TRUE(protocol.GetChannelLabels(inNames, outNames));
    EXPECT_EQ(inNames, (std::vector<std::string>{"Input 1", "Input 2"}));
    EXPECT_EQ(outNames, (std::vector<std::string>{"Output 1", "Output 2"}));
    const int reads = bus.readCount;
    protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
        EXPECT_EQ(status, kIOReturnSuccess);
    });
    EXPECT_EQ(bus.readCount, reads);
    EXPECT_EQ(bus.owner_, ASFW::Audio::DICE::kOwnerNoOwner);
    EXPECT_EQ(bus.writeCount, 0);
    EXPECT_EQ(bus.lockCount, 0);
}

TEST(DICETcatProtocolTests, TransientStageFailuresPreserveDiscoveryAndRecoverAfterBusReset) {
    for (uint32_t stage = 0; stage < 3; ++stage) {
        SCOPED_TRACE(stage);
        CountingFireWireBus bus;
        RouteState routeState;
        DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                                  nullptr, LowRateTenChannelPolicy());
        ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
        protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
            ASSERT_EQ(status, kIOReturnSuccess);
        });
        if (stage == 1) {
            protocol.PrepareDuplex({}, AudioClockConfig{.sampleRateHz = 48000},
                [](IOReturn status, auto) { ASSERT_EQ(status, kIOReturnSuccess); });
            protocol.ProgramRx([](IOReturn status, auto) { ASSERT_EQ(status, kIOReturnSuccess); });
            protocol.ProgramTxAndEnableDuplex(
                [](IOReturn status, auto) { ASSERT_EQ(status, kIOReturnSuccess); });
        }
        bus.failedReadAddress_ = stage == 0 ? kDiceBaseLo
            : kGlobalBaseLo + ASFW::Audio::DICE::GlobalOffset::kStatus;
        int failedCompletions = 0;
        auto failed = [&](IOReturn status, auto) {
            ++failedCompletions;
            EXPECT_NE(status, kIOReturnSuccess);
        };
        if (stage == 0) {
            protocol.PrepareDuplex({}, AudioClockConfig{.sampleRateHz = 48000}, failed);
        } else if (stage == 1) {
            protocol.ConfirmDuplexStart(failed);
        } else {
            protocol.ApplyClockConfig(AudioClockConfig{.sampleRateHz = 44100}, failed);
        }
        ASSERT_EQ(failedCompletions, 1);
        AudioStreamRuntimeCaps caps{};
        ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
        EXPECT_EQ(caps.sampleRateHz, 48000U);
        EXPECT_EQ(caps.deviceToHostStreams[0].pcmChannels, 10U);
        EXPECT_EQ(caps.hostToDeviceStreams[0].midiPorts, 1U);
        std::vector<std::string> inputs, outputs;
        ASSERT_TRUE(protocol.GetChannelLabels(inputs, outputs));
        EXPECT_EQ(inputs, (std::vector<std::string>{"Input 1", "Input 2"}));
        EXPECT_EQ(outputs, (std::vector<std::string>{"Output 1", "Output 2"}));

        // Bus reset supplies a new route; static discovery remains available
        // even while transaction reads still fail during recovery.
        ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = 0xD1CE000000000002ULL;
        rom.gen = Generation{2};
        rom.nodeId = 3;
        (void)routeState.registry.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
        bus.SetGeneration(rom.gen);
        protocol.UpdateRuntimeContext(*routeState.registry.CurrentRoute(rom.bib.guid), nullptr);
        const int reads = bus.readCount;
        protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
            EXPECT_EQ(status, kIOReturnSuccess);
        });
        EXPECT_EQ(bus.readCount, reads);
        bus.failedReadAddress_.reset();
        bus.owner_ = ASFW::Audio::DICE::kOwnerNoOwner;
        bus.enable_ = 0;
        bus.txIso_ = bus.rxIso_ = 0xFFFFFFFFU;
        protocol.PrepareDuplex({}, AudioClockConfig{.sampleRateHz = 48000},
            [](IOReturn status, auto) { EXPECT_EQ(status, kIOReturnSuccess); });
        EXPECT_EQ(protocol.StopDuplex(), kIOReturnSuccess);
        ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
        EXPECT_EQ(caps.deviceToHostStreams[0].pcmChannels, 10U);
        ASSERT_TRUE(protocol.GetChannelLabels(inputs, outputs));
        EXPECT_EQ(outputs.front(), "Output 1");
    }
}

TEST(DICETcatProtocolTests, OverlappingDiscoveryRequestsPreserveFirstPublishedSnapshot) {
    CountingFireWireBus bus;
    bus.deferredReadAddress_ = kDiceBaseLo;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    int completions = 0;
    auto complete = [&](IOReturn status) {
        ++completions;
        EXPECT_EQ(status, kIOReturnSuccess);
    };
    protocol.EnsureRuntimeStreamGeometry(complete);
    protocol.EnsureRuntimeStreamGeometry(complete);
    // The second discovery completes while the first read is still in flight.
    EXPECT_EQ(completions, 1);
    ASSERT_TRUE(bus.completeRead_);
    bus.rxPcm_ = 8;
    auto finishRead = std::move(bus.completeRead_);
    finishRead();
    EXPECT_EQ(completions, 2);
    EXPECT_EQ(bus.generalReadCount, 2);
    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostOutputPcmChannels, 10U);
}

TEST(DICETcatProtocolTests, RuntimeUpdatesAndLateDiscoveryCannotRewritePublishedTopologyOrLabels) {
    using Peer = ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer;
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
        ASSERT_EQ(status, kIOReturnSuccess);
    });
    AudioStreamRuntimeCaps live = RequiredTenChannelGeometry();
    live.sampleRateHz = 44100;
    live.deviceToHostIsoChannel = 5;
    live.hostToDeviceIsoChannel = 6;
    live.deviceToHostStreams[0].isoChannel = 5;
    live.hostToDeviceStreams[0].isoChannel = 6;
    // Unconstrained profiles may observe changed runtime geometry. That result
    // must not mutate topology already published to readers or Core Audio.
    live.hostInputPcmChannels = live.deviceToHostStreams[0].pcmChannels = 9;
    ASSERT_TRUE(Peer::UpdateOperationalCaps(protocol, live));
    ASFW::Audio::DICE::GlobalState global{};
    global.sampleRate = 48000;
    ASFW::Audio::DICE::StreamConfig tx{}, rx{};
    tx.numStreams = rx.numStreams = 1;
    tx.streams[0].pcmChannels = rx.streams[0].pcmChannels = 2;
    strlcpy(tx.streams[0].labels, "Late Input\\\\", sizeof(tx.streams[0].labels));
    strlcpy(rx.streams[0].labels, "Late Output\\\\", sizeof(rx.streams[0].labels));
    ASSERT_TRUE(Peer::CacheRuntimeCaps(protocol, global, tx, rx));
    AudioStreamRuntimeCaps observed{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(observed));
    EXPECT_EQ(observed.sampleRateHz, 44100U);
    EXPECT_EQ(observed.deviceToHostIsoChannel, 5U);
    EXPECT_EQ(observed.hostToDeviceStreams[0].isoChannel, 6U);
    EXPECT_EQ(observed.hostInputPcmChannels, 10U);
    EXPECT_EQ(observed.deviceToHostStreams[0].pcmChannels, 10U);
    EXPECT_EQ(observed.hostToDeviceStreams[0].pcmChannels, 10U);
    std::vector<std::string> inputs, outputs;
    ASSERT_TRUE(protocol.GetChannelLabels(inputs, outputs));
    EXPECT_EQ(inputs.front(), "Input 1");
    EXPECT_EQ(outputs.front(), "Output 1");
}

TEST(DICETcatProtocolTests, ConcurrentRuntimeReadersRetainImmutableDiscoverySnapshot) {
    using Peer = ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer;
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
        ASSERT_EQ(status, kIOReturnSuccess);
    });
    std::atomic<bool> reading{false};
    std::atomic<bool> done{false};
    std::atomic<bool> consistent{true};
    std::thread reader([&] {
        reading.store(true, std::memory_order_release);
        do {
            AudioStreamRuntimeCaps caps{};
            std::vector<std::string> inputs, outputs;
            if (!protocol.GetRuntimeAudioStreamCaps(caps) ||
                caps.hostInputPcmChannels != 10 ||
                caps.deviceToHostStreams[0].pcmChannels != 10 ||
                caps.hostToDeviceStreams[0].midiPorts != 1 ||
                !protocol.GetChannelLabels(inputs, outputs) ||
                inputs != std::vector<std::string>{"Input 1", "Input 2"} ||
                outputs != std::vector<std::string>{"Output 1", "Output 2"}) {
                consistent.store(false, std::memory_order_relaxed);
            }
        } while (!done.load(std::memory_order_acquire));
    });
    while (!reading.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    for (uint32_t i = 0; i < 500; ++i) {
        auto caps = RequiredTenChannelGeometry();
        caps.sampleRateHz = i % 2 == 0 ? 44100 : 48000;
        caps.hostInputPcmChannels = caps.deviceToHostStreams[0].pcmChannels = 9;
        EXPECT_TRUE(Peer::UpdateOperationalCaps(protocol, caps));
        caps.sampleRateHz = 0; // Rejected operational result must not reset discovery.
        EXPECT_FALSE(Peer::UpdateOperationalCaps(protocol, caps));
        ASFW::Audio::DICE::GlobalState global{};
        global.sampleRate = 48000;
        ASFW::Audio::DICE::StreamConfig tx{}, rx{};
        tx.numStreams = rx.numStreams = 1;
        tx.streams[0].pcmChannels = rx.streams[0].pcmChannels = 2;
        strlcpy(tx.streams[0].labels, "Late\\\\", sizeof(tx.streams[0].labels));
        EXPECT_TRUE(Peer::CacheRuntimeCaps(protocol, global, tx, rx));
    }
    done.store(true, std::memory_order_release);
    reader.join();
    EXPECT_TRUE(consistent.load());
}

TEST(DICETcatProtocolTests, ConcurrentDiscoveryPublishesOneCompleteSnapshot) {
    using Peer = ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer;
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);
    ASFW::Audio::DICE::GlobalState global{};
    global.sampleRate = 48000;
    ASFW::Audio::DICE::StreamConfig first{}, second{};
    first.numStreams = second.numStreams = 1;
    first.streams[0].pcmChannels = 2;
    second.streams[0].pcmChannels = 4;
    strlcpy(first.streams[0].labels, "First\\\\", sizeof(first.streams[0].labels));
    strlcpy(second.streams[0].labels, "Second\\\\", sizeof(second.streams[0].labels));
    std::atomic<bool> start{false};
    auto publish = [&](const auto& config) {
        while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        EXPECT_TRUE(Peer::CacheRuntimeCaps(protocol, global, config, config));
    };
    std::thread a([&] { publish(first); });
    std::thread b([&] { publish(second); });
    start.store(true, std::memory_order_release);
    a.join();
    b.join();
    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    std::vector<std::string> inputs, outputs;
    ASSERT_TRUE(protocol.GetChannelLabels(inputs, outputs));
    const bool firstWon = caps.hostInputPcmChannels == 2;
    EXPECT_EQ(caps.hostOutputPcmChannels, firstWon ? 2U : 4U);
    EXPECT_EQ(caps.deviceToHostStreams[0].pcmChannels, firstWon ? 2U : 4U);
    EXPECT_EQ(caps.hostToDeviceStreams[0].pcmChannels, firstWon ? 2U : 4U);
    EXPECT_EQ(inputs, (std::vector<std::string>{firstWon ? "First" : "Second"}));
    EXPECT_EQ(outputs, inputs);
}

TEST(DICETcatProtocolTests, LateInitialDiscoveryDoesNotOverwriteSuccessfulClockAndIsoState) {
    using Peer = ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer;
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    auto caps = RequiredTenChannelGeometry();
    caps.sampleRateHz = 44100;
    caps.deviceToHostIsoChannel = 5;
    caps.hostToDeviceIsoChannel = 6;
    caps.deviceToHostStreams[0].isoChannel = 5;
    caps.hostToDeviceStreams[0].isoChannel = 6;
    ASSERT_TRUE(Peer::UpdateOperationalCaps(protocol, caps));
    EXPECT_FALSE(protocol.GetRuntimeAudioStreamCaps(caps));
    protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
        ASSERT_EQ(status, kIOReturnSuccess);
    });
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.sampleRateHz, 44100U);
    EXPECT_EQ(caps.deviceToHostIsoChannel, 5U);
    EXPECT_EQ(caps.hostToDeviceIsoChannel, 6U);
    EXPECT_EQ(caps.deviceToHostStreams[0].isoChannel, 5U);
    EXPECT_EQ(caps.hostToDeviceStreams[0].isoChannel, 6U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 10U);
}

TEST(DICETcatProtocolTests, RequiredWireGeometryAllowsHiddenCoreAudioCapture) {
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr,
                              nullptr, {.exposeDeviceToHostToCoreAudio = false,
                                        .requiredRuntimeGeometry = RequiredTenChannelGeometry()});
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    int completions = 0;
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
        protocol.EnsureRuntimeStreamGeometry([&](IOReturn status) {
            ++completions;
            EXPECT_EQ(status, kIOReturnSuccess);
        });
    }
    EXPECT_EQ(completions, 2);
    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 0U);
    EXPECT_EQ(caps.deviceToHostStreams[0].pcmChannels, 10U);
    EXPECT_EQ(caps.deviceToHostStreams[0].am824Slots, 11U);
    EXPECT_EQ(bus.writeCount, 0);
    EXPECT_EQ(bus.lockCount, 0);
}

TEST(DICETcatProtocolTests, NeutralClockRequestMapsToDiceClockSelectInsideAdapter) {
    DiceClockConfiguration mapped{};
    EXPECT_TRUE(ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::MakeDiceClockConfiguration(
        AudioClockConfig{.sampleRateHz = 48000U}, mapped));
    EXPECT_EQ(mapped.sampleRateHz, 48000U);
    EXPECT_EQ(mapped.clockSelect, kClockSelect48kInternal);

    // Validated 1x rates map to their CLOCK_SELECT encoding (rate index << 8 |
    // internal source).
    EXPECT_TRUE(ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::MakeDiceClockConfiguration(
        AudioClockConfig{.sampleRateHz = 44100U}, mapped));
    EXPECT_EQ(mapped.sampleRateHz, 44100U);
    EXPECT_EQ(mapped.clockSelect,
              (ASFW::Audio::DICE::ClockRateIndex::k44100
               << ASFW::Audio::DICE::ClockSelect::kRateShift) |
                  static_cast<uint32_t>(ClockSource::Internal));

    // 2x/4x rates stay rejected until the stream geometry is HW-validated.
    EXPECT_FALSE(ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::MakeDiceClockConfiguration(
        AudioClockConfig{.sampleRateHz = 96000U}, mapped));
}

TEST(DICETcatProtocolTests, RuntimeCapsAggregateTotalConfiguredStreams) {
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);

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

TEST(DICETcatProtocolTests, RuntimePolicyHidesCoreAudioInputWithoutChangingWireGeometry) {
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus,
                              bus,
                              routeState.registry,
                              routeState.route,
                              nullptr,
                              nullptr,
                              DICETcatRuntimePolicy{.exposeDeviceToHostToCoreAudio = false});

    ASFW::Audio::DICE::GlobalState global{};
    global.sampleRate = 48000;
    ASFW::Audio::DICE::StreamConfig tx{};
    tx.numStreams = 1;
    tx.streams[0].isoChannel = 0;
    tx.streams[0].pcmChannels = 2;
    strlcpy(tx.streams[0].labels, "Return L\\Return R\\\\", sizeof(tx.streams[0].labels));
    ASFW::Audio::DICE::StreamConfig rx{};
    rx.numStreams = 1;
    rx.streams[0].isoChannel = 1;
    rx.streams[0].pcmChannels = 2;
    strlcpy(rx.streams[0].labels, "Out L\\Out R\\\\", sizeof(rx.streams[0].labels));

    ASFW::Audio::DICE::TCAT::DICETcatProtocolTestPeer::CacheRuntimeCaps(protocol, global, tx, rx);

    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 0U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 2U);
    EXPECT_EQ(caps.deviceToHostAm824Slots, 2U);
    EXPECT_EQ(caps.hostToDeviceAm824Slots, 2U);
    ASSERT_EQ(caps.deviceToHostStreamCount, 1U);
    ASSERT_EQ(caps.hostToDeviceStreamCount, 1U);
    EXPECT_EQ(caps.deviceToHostStreams[0].pcmChannels, 2U);
    EXPECT_EQ(caps.hostToDeviceStreams[0].pcmChannels, 2U);

    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    ASSERT_TRUE(protocol.GetChannelLabels(inNames, outNames));
    EXPECT_TRUE(inNames.empty());
    ASSERT_EQ(outNames.size(), 2U);
    EXPECT_EQ(outNames[0], "Out L");
}

TEST(DICETcatProtocolTests, ChannelLabelsFlattenAcrossStreamsInChannelOrder) {
    CountingFireWireBus bus;
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);

    // No labels before caps are cached.
    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    EXPECT_FALSE(protocol.GetChannelLabels(inNames, outNames));

    ASFW::Audio::DICE::GlobalState global{};
    global.sampleRate = 48000;

    // Host input == device TX; two streams, names concatenated in stream order.
    ASFW::Audio::DICE::StreamConfig tx{};
    tx.numStreams = 2;
    tx.streams[0].pcmChannels = 2;
    tx.streams[1].pcmChannels = 2;
    strlcpy(tx.streams[0].labels, "Mic 1\\Mic 2\\\\", sizeof(tx.streams[0].labels));
    strlcpy(tx.streams[1].labels, "Line 3\\Line 4\\\\", sizeof(tx.streams[1].labels));

    // Host output == device RX.
    ASFW::Audio::DICE::StreamConfig rx{};
    rx.numStreams = 1;
    rx.streams[0].pcmChannels = 2;
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
    RouteState routeState;
    DICETcatProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);
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
    RouteState routeState;
    SPro24DspProtocol protocol(bus, bus, routeState.registry, routeState.route, nullptr);
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
