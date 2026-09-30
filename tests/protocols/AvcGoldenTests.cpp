// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcGoldenTests.cpp - Golden wire traces of TODAY's AV/C stack behaviour.
//
// Characterizes exact bus writes and sequences for:
// - Duet attach discovery (AVCUnit::Initialize + OxfwStreamFormats)
// - Duet streaming start/stop (SignalFormat rate control + CMP)
// - Phase 88 attach discovery (AVCUnit::Initialize + BeBoBPlug0StreamDiscovery)
// - Onyx-i Oxford discovery (OxfwStreamFormats against documented Onyx-i capture)
// - 1814 allowlist enforcement (admitted probes vs refused commands)
// - Generic bus reset recovery (idempotent replay across generations)
// - Generic interim deferral and timeout handling
//
// Captures TODAY's behavior into tests/golden/avc/<device>__<scenario>.trace
// before Phase 2b caller migration.
//
// Regenerate after an intended change: ASFW_UPDATE_GOLDEN=1, then review the diff.

#include <gtest/gtest.h>

#include "AvcDeviceImages.inc"
#include "RecordingFireWireBus.hpp"
#include "SimulatedAvcUnit.hpp"
#include "WireTrace.hpp"
#include "DuetDescriptorFixture.hpp"
#include "ASFWDriver/Protocols/AVC/Graph/AvcDeviceGraph.hpp"
#include "FakeTimerScheduler.hpp"

#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Discovery/FWDevice.hpp"
#include "ASFWDriver/Discovery/FWUnit.hpp"

#include "ASFWDriver/Protocols/AVC/AVCUnit.hpp"
#include "ASFWDriver/Protocols/AVC/FCPTransport.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/GeneralCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Core/RateCodes.hpp"

#include "ASFWDriver/Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"
#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeDuetDuplex.hpp"
#include "ASFWDriver/Audio/Protocols/Oxford/OxfwStreamFormats.hpp"

#include "ASFWDriver/Protocols/AVC/CMP/CMPClient.hpp"
#include "ASFWDriver/Bus/IRM/IRMClient.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ASFW;
using namespace ASFW::AVC;
using namespace ASFW::AVC::Testing;
using namespace ASFW::Protocols::AVC;
using namespace ASFW::Testing;

struct AvcGoldenRigOptions {
    uint64_t guid{0x0003DB0A0000D112ULL};
    uint16_t nodeId{0};
    uint32_t generation{3};
    Discovery::AvcCommandFilterId filter{Discovery::AvcCommandFilterId::Unrestricted};
    bool allowBusResetRetry{false};
    uint32_t timeoutMs{100};
    uint32_t interimTimeoutMs{500};
    uint8_t maxRetries{1};
};

class AvcGoldenRig {
public:
    explicit AvcGoldenRig(const AvcDeviceImage& image, AvcGoldenRigOptions options = {})
        : options_(options) {
        bus_.SetGeneration(FW::Generation{options_.generation});
        bus_.SetLocalNodeID(FW::NodeId{static_cast<uint8_t>(options_.nodeId == 0 ? 1 : 0)});

        Discovery::ConfigROM rom{};
        rom.bib.guid = options_.guid;
        rom.gen = FW::Generation{options_.generation};
        rom.nodeId = options_.nodeId;
        auto record = routes_.UpsertFromROM(rom, Discovery::LinkPolicy{});
        record.avcCommandFilter = options_.filter;
        device_ = Discovery::FWDevice::Create(record, Discovery::ConfigROM{});
        fwUnit_ = Discovery::FWUnit::Create(device_, 0x400, {});

        avcUnit_ = std::make_shared<AVCUnit>(device_, fwUnit_, routes_, bus_, bus_, timers_);

        simUnit_ = std::make_unique<SimulatedAvcUnit>(image);
        simUnit_->SetNodeId(FW::NodeId{static_cast<uint8_t>(options_.nodeId)});
        simUnit_->SetGeneration(FW::Generation{options_.generation});

        simUnit_->AttachToBus(bus_, [this](uint16_t srcNode, uint32_t gen, std::span<const uint8_t> payload) {
            if (activeTransport_) {
                activeTransport_->OnFCPResponse(srcNode, gen, payload);
            } else if (avcUnit_) {
                avcUnit_->GetFCPTransport().OnFCPResponse(srcNode, gen, payload);
            }
        });

        // Setup CMP registers and compare-swap locks
        bus_.SetReadResponder([](Async::FWAddress addr, uint32_t) -> std::optional<std::vector<uint8_t>> {
            if (addr.addressHi == 0xFFFF && (addr.addressLo == 0xF0000900 || addr.addressLo == 0xF0000980)) {
                // MPR: 0x80000001 (1 plug)
                return std::vector<uint8_t>{0x80, 0x00, 0x00, 0x01};
            }
            if (addr.addressHi == 0xFFFF && (addr.addressLo == 0xF0000904 || addr.addressLo == 0xF0000984)) {
                // PCR: 0x80000000 (online, 0 connections)
                return std::vector<uint8_t>{0x80, 0x00, 0x00, 0x00};
            }
            return std::nullopt;
        });

        bus_.SetLockResponder([](Async::FWAddress, bool, uint64_t expected, uint64_t) -> std::optional<uint64_t> {
            return expected; // Compare-swap succeeds
        });
    }

    ~AvcGoldenRig() {
        if (activeTransport_) {
            activeTransport_->Shutdown();
        }
        if (avcUnit_) {
            avcUnit_->Shutdown();
        }
    }

    void SetActiveTransport(std::shared_ptr<Protocols::AVC::FCPTransport> transport) {
        activeTransport_ = std::move(transport);
    }

    [[nodiscard]] RecordingFireWireBus& Bus() noexcept { return bus_; }
    [[nodiscard]] FakeTimerScheduler& Timers() noexcept { return timers_; }
    [[nodiscard]] Discovery::DeviceRegistry& Routes() noexcept { return routes_; }
    [[nodiscard]] std::shared_ptr<AVCUnit> Unit() noexcept { return avcUnit_; }
    [[nodiscard]] Protocols::AVC::FCPTransport& Transport() noexcept {
        return activeTransport_ ? *activeTransport_ : avcUnit_->GetFCPTransport();
    }
    [[nodiscard]] SimulatedAvcUnit& Sim() noexcept { return *simUnit_; }

    [[nodiscard]] Discovery::DeviceRouteToken Route() const {
        return routes_.CurrentRoute(options_.guid).value_or(Discovery::DeviceRouteToken{});
    }

    void Mark(std::string_view line) { bus_.Trace().Add(line); }

    void ExpectGolden(std::string_view scenario) {
        std::string relPath = "avc/" + std::string(scenario) + ".trace";
        ::ASFW::Testing::ExpectMatchesGolden(bus_.Trace(), relPath);
    }

private:
    AvcGoldenRigOptions options_;
    RecordingFireWireBus bus_;
    FakeTimerScheduler timers_;
    Discovery::DeviceRegistry routes_;
    std::shared_ptr<Discovery::FWDevice> device_;
    std::shared_ptr<Discovery::FWUnit> fwUnit_;
    std::shared_ptr<AVCUnit> avcUnit_;
    std::shared_ptr<Protocols::AVC::FCPTransport> activeTransport_;
    std::unique_ptr<SimulatedAvcUnit> simUnit_;
};

// Documented Onyx-i Oxford image (AV/C EXTENDED STREAM FORMAT INFORMATION):
// 8ch capture, 2ch playback, 44.1/48/88.2/96 kHz, compound AM824, 0 MIDI.
namespace OnyxiData {

// Index 0: 44.1 kHz (code 0x03)
inline constexpr uint8_t kCmd_0[] = {0x01, 0xFF, 0xBF, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00};
inline constexpr uint8_t kResp_0[] = {0x0C, 0xFF, 0xBF, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
                                      0x90, 0x40, 0x03, 0x00, 0x01, 0x08, 0x06};

// Index 1: 48 kHz (code 0x04)
inline constexpr uint8_t kCmd_1[] = {0x01, 0xFF, 0xBF, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x01};
inline constexpr uint8_t kResp_1[] = {0x0C, 0xFF, 0xBF, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x01,
                                      0x90, 0x40, 0x04, 0x00, 0x01, 0x08, 0x06};

// Index 2: 88.2 kHz (code 0x05)
inline constexpr uint8_t kCmd_2[] = {0x01, 0xFF, 0xBF, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x02};
inline constexpr uint8_t kResp_2[] = {0x0C, 0xFF, 0xBF, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x02,
                                      0x90, 0x40, 0x05, 0x00, 0x01, 0x08, 0x06};

// Index 3: 96 kHz (code 0x0A)
inline constexpr uint8_t kCmd_3[] = {0x01, 0xFF, 0xBF, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x03};
inline constexpr uint8_t kResp_3[] = {0x0C, 0xFF, 0xBF, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x03,
                                      0x90, 0x40, 0x0A, 0x00, 0x01, 0x08, 0x06};

// Index 4: End of list (Rejected)
inline constexpr uint8_t kCmd_4[] = {0x01, 0xFF, 0xBF, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x04};
inline constexpr uint8_t kResp_4[] = {0x0A, 0xFF, 0xBF, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x04};

inline constexpr AvcExchangeRecord kRecords[] = {
    {"stream_format_idx_0", kCmd_0, kResp_0, 0x0C, 5000U, true},
    {"stream_format_idx_1", kCmd_1, kResp_1, 0x0C, 5000U, true},
    {"stream_format_idx_2", kCmd_2, kResp_2, 0x0C, 5000U, true},
    {"stream_format_idx_3", kCmd_3, kResp_3, 0x0C, 5000U, true},
    {"stream_format_idx_4", kCmd_4, kResp_4, 0x0A, 5000U, true},
};

inline constexpr AvcDeviceImage kOnyxi{
    .key = "onyxi",
    .modelName = "Onyx-i",
    .vendorName = "Mackie",
    .guid = 0x000FF20400000000ULL,
    .nodeId = 0U,
    .generation = 1U,
    .driverVersion = "0.3.1",
    .capturedAt = "2026-08-17T00:00:00.000000+00:00",
    .records = kRecords,
};

} // namespace OnyxiData

} // namespace

// ============================================================================
// 1. Duet Attach Discovery
// ============================================================================

TEST(AvcGoldenTests, DuetAttachDiscovery) {
    AvcGoldenRig rig(kDuet);

    rig.Mark("## AVCUnit::Initialize");
    bool initOk = false;
    rig.Unit()->Initialize([&](bool ok) { initOk = ok; });
    EXPECT_TRUE(initOk);

    rig.Mark("## OxfwStreamFormats::DetectStreamFormats");
    bool detectFired = false;
    Audio::Oxford::DetectStreamFormats(
        rig.Transport(), false, [&](IOReturn status, const Audio::Oxford::StreamFormatSet& set) {
            detectFired = true;
            EXPECT_EQ(status, kIOReturnSuccess);
            EXPECT_FALSE(set.assumed);
        });
    EXPECT_TRUE(detectFired);

    rig.ExpectGolden("duet__attach_discovery");
}

TEST(AvcGoldenTests, DescriptorGraphSelectsRoutedStreamsAndValidatesGeometry) {
    AvcGoldenRig rig(kDuet);
    std::vector<uint8_t> descriptor;
    const auto& hex = Fixtures::kDuetMusicStatusHex;
    for (size_t i = 0; i < hex.size(); i += 2)
        descriptor.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    rig.Sim().SetDescriptor(0x60, {0x80}, std::move(descriptor));
    // Playback source unit ISO 0 -> Music destination 0; capture comes from
    // Music source 1, deliberately not the descriptor's first source.
    rig.Sim().SetResponseOverride({0x01,0xff,0x1a,0xff,0xff,0xfe,0x60,0x00},
        {0x0c,0xff,0x1a,0xff,0xff,0x00,0x60,0x00});
    rig.Sim().SetResponseOverride({0x01,0xff,0x1a,0xff,0xff,0xfe,0xff,0x00},
        {0x0c,0xff,0x1a,0xff,0x60,0x01,0xff,0x00});
    for (uint8_t direction = 0; direction < 2; ++direction) {
        for (uint8_t plug = 0; plug < 2; ++plug) {
            rig.Sim().SetResponseOverride({0x01,0x60,0xbf,0xc0,direction,0x01,plug,0xff,0xff,0xff},
                {0x0c,0x60,0xbf,0xc0,direction,0x01,plug,0xff,0xff,0x00,0x90,0x40,0x03,0x00,0x01,0x02,0x06});
        }
    }
    bool done = false;
    rig.Unit()->Initialize([&](bool ok) { done = ok; });
    ASSERT_TRUE(done);
    const auto graph = rig.Unit()->GetDiscoveredGraph();
    ASSERT_TRUE(graph);
    EXPECT_EQ(graph->playback.subunitPlugId, 0);
    EXPECT_EQ(graph->capture.subunitPlugId, 1);
    EXPECT_EQ(graph->capture.channelNames[0], "Analog Out 1");
    EXPECT_EQ(graph->playback.dataBlockSize, 2);
    EXPECT_EQ(graph->capture.currentSampleRate, 44100);
    EXPECT_EQ(graph->capture.slotMapValidation, Graph::SlotMapValidation::kValidated);
}

// ============================================================================
// 2. Duet Streaming Start / Stop
// ============================================================================

TEST(AvcGoldenTests, DuetStreamingStartStop) {
    AvcGoldenRig rig(kDuet);

    rig.Mark("## SignalFormat::QueryOutputPlug0");
    Cmd::PlugSignalFormatCommand queryCmd{
        .operands = {
            .direction = Cmd::PlugSignalDirection::kOutput,
            .plugId = 0,
            .query = Cmd::SignalFormatQuery::kAllWildcard,
        }
    };
    bool queryDone = false;
    rig.Unit()->Status(queryCmd, [&](Expected<Cmd::PlugSignalFormat> res) {
        queryDone = true;
        EXPECT_TRUE(res.has_value());
    });
    EXPECT_TRUE(queryDone);

    rig.Mark("## SignalFormat::Set44100");
    rig.Sim().SetResponseOverride(
        {0x00, 0xFF, 0x18, 0x00, 0x90, 0x01, 0xFF, 0xFF},
        {0x09, 0xFF, 0x18, 0x00, 0x90, 0x01, 0xFF, 0xFF});
    Cmd::PlugSignalFormatCommand setCmd44{
        .operands = {
            .direction = Cmd::PlugSignalDirection::kOutput,
            .plugId = 0,
            .format = Cmd::Am824SignalFormat(0, CipSfc::k44100),
        }
    };
    bool set44Done = false;
    rig.Unit()->Control(setCmd44, [&](Expected<Cmd::PlugSignalFormat> res) {
        set44Done = true;
        EXPECT_TRUE(res.has_value());
    });
    EXPECT_TRUE(set44Done);

    rig.Mark("## SignalFormat::Set48000");
    rig.Sim().SetResponseOverride(
        {0x00, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF},
        {0x09, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF});
    Cmd::PlugSignalFormatCommand setCmd48{
        .operands = {
            .direction = Cmd::PlugSignalDirection::kOutput,
            .plugId = 0,
            .format = Cmd::Am824SignalFormat(0, CipSfc::k48000),
        }
    };
    bool set48Done = false;
    rig.Unit()->Control(setCmd48, [&](Expected<Cmd::PlugSignalFormat> res) {
        set48Done = true;
        EXPECT_TRUE(res.has_value());
    });
    EXPECT_TRUE(set48Done);

    rig.Mark("## ApogeeDuetDuplex::ProgramRx");
    IRM::IRMClient irm(rig.Bus());
    CMP::CMPClient cmp(rig.Bus(), rig.Bus(), rig.Routes());
    Audio::Oxford::Apogee::ApogeeDuetProtocol protocol(
        rig.Bus(), rig.Bus(), rig.Route(), &rig.Routes(), &rig.Transport(), &irm, &cmp);
    auto& duplex = protocol.Duplex();

    bool rxDone = false;
    duplex.ProgramRx([&](IOReturn status, Audio::DuplexStageResult) {
        rxDone = true;
        EXPECT_EQ(status, kIOReturnSuccess);
    });
    EXPECT_TRUE(rxDone);

    rig.Mark("## ApogeeDuetDuplex::ProgramTxAndEnable");
    bool txDone = false;
    duplex.ProgramTxAndEnableDuplex([&](IOReturn status, Audio::DuplexStageResult) {
        txDone = true;
        EXPECT_EQ(status, kIOReturnSuccess);
    });
    EXPECT_TRUE(txDone);

    rig.Mark("## ApogeeDuetDuplex::StopDuplex");
    EXPECT_EQ(duplex.StopDuplex(), kIOReturnSuccess);

    rig.ExpectGolden("duet__streaming_start_stop");
}

// ============================================================================
// 3. Phase 88 Attach Discovery
// ============================================================================

TEST(AvcGoldenTests, Phase88AttachDiscovery) {
    AvcGoldenRigOptions opts;
    opts.guid = kPhase88.guid;
    opts.nodeId = static_cast<uint16_t>(kPhase88.nodeId);
    opts.generation = kPhase88.generation;
    AvcGoldenRig rig(kPhase88, opts);

    rig.Mark("## AVCUnit::Initialize");
    bool initOk = false;
    rig.Unit()->Initialize([&](bool ok) { initOk = ok; });
    EXPECT_TRUE(initOk);

    rig.Mark("## BeBoBPlug0StreamDiscovery");
    bool bebobDone = false;
    Audio::BeBoB::StartBeBoBPlug0Discovery(
        *rig.Unit(), rig.Unit()->GetGUID(), [&](const Audio::BeBoB::DeviceModel& model) {
            bebobDone = true;
            EXPECT_TRUE(model.unitPlugCounts.has_value());
            EXPECT_EQ(model.input.supportedFormations.size(), 5U);
            EXPECT_EQ(model.output.supportedFormations.size(), 5U);
            EXPECT_EQ(model.CurrentRateHz(), 48000U);
        });
    EXPECT_TRUE(bebobDone);

    rig.ExpectGolden("phase88__attach_discovery");
}

// ============================================================================
// 4. Onyx-i Attach Discovery
// ============================================================================

TEST(AvcGoldenTests, OnyxiAttachDiscovery) {
    AvcGoldenRigOptions opts;
    opts.guid = OnyxiData::kOnyxi.guid;
    opts.nodeId = static_cast<uint16_t>(OnyxiData::kOnyxi.nodeId);
    opts.generation = OnyxiData::kOnyxi.generation;
    AvcGoldenRig rig(OnyxiData::kOnyxi, opts);

    rig.Mark("## OxfwStreamFormats::DetectStreamFormats");
    bool detectFired = false;
    Audio::Oxford::DetectStreamFormats(
        rig.Transport(), false, [&](IOReturn status, const Audio::Oxford::StreamFormatSet& set) {
            detectFired = true;
            EXPECT_EQ(status, kIOReturnSuccess);
            EXPECT_FALSE(set.assumed);
            EXPECT_EQ(set.Rates(), (std::vector<uint32_t>{44100, 48000, 96000, 88200}));
            ASSERT_EQ(set.entries.size(), 4U);
            EXPECT_EQ(set.entries[0].pcmChannels, 8);
            EXPECT_EQ(set.entries[0].midiSlots, 0);
        });
    EXPECT_TRUE(detectFired);

    rig.ExpectGolden("onyxi__attach_discovery");
}

// ============================================================================
// 5. 1814 Allowlist Enforcement
// ============================================================================

TEST(AvcGoldenTests, Fw1814AllowlistEnforcement) {
    AvcGoldenRigOptions opts;
    opts.guid = 0x000D6C0400DA3D9AULL;
    opts.filter = Discovery::AvcCommandFilterId::MAudioSpecialBeBoB;
    AvcGoldenRig rig(kPhase88, opts);

    rig.Mark("## Allowed: InputSignalFormatProbe");
    Cmd::PlugSignalFormatCommand inCmd{
        .operands = {
            .direction = Cmd::PlugSignalDirection::kInput,
            .plugId = 0,
            .query = Cmd::SignalFormatQuery::kAm824Wildcard,
        }
    };
    auto inFrame = inCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(inFrame.has_value());
    FCPFrame inFcp{};
    std::copy(inFrame->WireBytes().begin(), inFrame->WireBytes().end(), inFcp.data.begin());
    inFcp.length = inFrame->WireBytes().size();
    bool inDone = false;
    (void)rig.Transport().SubmitCommand(inFcp, [&](FCPStatus status, const FCPFrame&) {
        inDone = true;
        EXPECT_EQ(status, FCPStatus::kOk);
    });
    EXPECT_TRUE(inDone);

    rig.Mark("## Allowed: OutputSignalFormatProbe");
    Cmd::PlugSignalFormatCommand outCmd{
        .operands = {
            .direction = Cmd::PlugSignalDirection::kOutput,
            .plugId = 0,
            .query = Cmd::SignalFormatQuery::kAm824Wildcard,
        }
    };
    auto outFrame = outCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(outFrame.has_value());
    FCPFrame outFcp{};
    std::copy(outFrame->WireBytes().begin(), outFrame->WireBytes().end(), outFcp.data.begin());
    outFcp.length = outFrame->WireBytes().size();
    bool outDone = false;
    (void)rig.Transport().SubmitCommand(outFcp, [&](FCPStatus status, const FCPFrame&) {
        outDone = true;
        EXPECT_EQ(status, FCPStatus::kOk);
    });
    EXPECT_TRUE(outDone);

    rig.Mark("## Allowed: RateControl48k");
    rig.Sim().SetResponseOverride(
        {0x00, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF},
        {0x09, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF});
    FCPFrame ctrlFrame{};
    const uint8_t ctrlBytes[] = {0x00, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF};
    std::copy(std::begin(ctrlBytes), std::end(ctrlBytes), ctrlFrame.data.begin());
    ctrlFrame.length = sizeof(ctrlBytes);
    bool ctrlDone = false;
    (void)rig.Transport().SubmitCommand(ctrlFrame, [&](FCPStatus status, const FCPFrame&) {
        ctrlDone = true;
        EXPECT_EQ(status, FCPStatus::kOk);
    });
    EXPECT_TRUE(ctrlDone);

    // Refused commands (must never appear on wire)
    auto expectRefused = [&](const char* label, std::initializer_list<uint8_t> bytes) {
        rig.Mark(std::string("## Refused: ") + label);
        FCPFrame f{};
        std::copy(bytes.begin(), bytes.end(), f.data.begin());
        f.length = bytes.size();
        bool refused = false;
        (void)rig.Transport().SubmitCommand(f, [&](FCPStatus status, const FCPFrame&) {
            refused = true;
            EXPECT_EQ(status, FCPStatus::kRefusedByFilter);
        });
        EXPECT_TRUE(refused);
    };

    expectRefused("UnitInfo", {0x01, 0xFF, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    expectRefused("SubunitInfo", {0x01, 0x08, 0x31, 0x07, 0xFF, 0xFF, 0xFF, 0xFF});
    expectRefused("PlugInfo", {0x01, 0xFF, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00});
    expectRefused("ExtendedStreamFormat", {0x01, 0xFF, 0x2F, 0xC1, 0x00, 0x00, 0x00, 0x00});

    rig.ExpectGolden("fw1814__allowlist_enforcement");
}

// ============================================================================
// 6. Generic Bus Reset Recovery
// ============================================================================

TEST(AvcGoldenTests, GenericBusResetRecovery) {
    AvcGoldenRigOptions opts;
    opts.guid = 0x0003DB0A0000D112ULL;
    opts.nodeId = 0;
    opts.generation = 1;
    opts.allowBusResetRetry = true;

    AvcGoldenRig rig(kDuet, opts);

    Protocols::AVC::FCPTransportConfig config{};
    config.allowBusResetRetry = true;
    config.maxRetries = 1;
    config.timeoutMs = 100;
    config.interimTimeoutMs = 500;

    auto transport = std::make_shared<Protocols::AVC::FCPTransport>();
    ASSERT_TRUE(transport->init(&rig.Bus(), &rig.Bus(), rig.Unit()->GetDevice().get(),
                                rig.Routes(), rig.Timers(), config));
    rig.SetActiveTransport(transport);

    rig.Mark("## Submit Idempotent Command Gen 1");

    bool resetTriggered = false;
    rig.Bus().SetWriteResponder([&](Async::FWAddress addr, std::span<const uint8_t> data) {
        if (!resetTriggered && addr.addressHi == 0xFFFF && addr.addressLo == 0xF0000B00) {
            resetTriggered = true;
            rig.Bus().BusReset();
            rig.Transport().OnBusReset(2);

            Discovery::ConfigROM romGen2{};
            romGen2.bib.guid = opts.guid;
            romGen2.gen = FW::Generation{2};
            romGen2.nodeId = opts.nodeId;
            (void)rig.Routes().UpsertFromROM(romGen2, Discovery::LinkPolicy{});
            const auto newRoute = rig.Routes().CurrentRoute(opts.guid);
            ASSERT_TRUE(newRoute.has_value());
            rig.Transport().OnRouteRevalidated(*newRoute);
            return;
        }

        if (addr.addressHi == 0xFFFF && addr.addressLo == 0xF0000B00) {
            auto respOpt = rig.Sim().FindResponse(data);
            if (respOpt) {
                rig.Transport().OnFCPResponse(opts.nodeId, 2, *respOpt);
            }
        }
    });

    FCPFrame cmd{};
    const uint8_t queryBytes[] = {0x01, 0xFF, 0x18, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    std::copy(std::begin(queryBytes), std::end(queryBytes), cmd.data.begin());
    cmd.length = sizeof(queryBytes);

    FCPCommandPolicy policy{};
    policy.retryClass = FCPRetryClass::kIdempotent;

    bool done = false;
    (void)rig.Transport().SubmitCommand(cmd, [&](FCPStatus status, const FCPFrame&) {
        done = true;
        EXPECT_EQ(status, FCPStatus::kOk);
    }, policy);

    EXPECT_TRUE(done);
    rig.ExpectGolden("generic__bus_reset_recovery");
}

// ============================================================================
// 7. Generic Interim and Timeout
// ============================================================================

TEST(AvcGoldenTests, GenericInterimAndTimeout) {
    AvcGoldenRigOptions opts;
    opts.guid = 0x0003DB0A0000D112ULL;
    opts.nodeId = 0;
    opts.generation = 1;
    opts.timeoutMs = 50;
    opts.interimTimeoutMs = 150;

    AvcGoldenRig rig(kDuet, opts);

    Protocols::AVC::FCPTransportConfig config{};
    config.timeoutMs = 50;
    config.interimTimeoutMs = 150;
    config.maxRetries = 0;

    auto transport = std::make_shared<Protocols::AVC::FCPTransport>();
    ASSERT_TRUE(transport->init(&rig.Bus(), &rig.Bus(), rig.Unit()->GetDevice().get(),
                                rig.Routes(), rig.Timers(), config));
    rig.SetActiveTransport(transport);

    // Part 1: Interim response followed by final response
    rig.Mark("## InterimResponse");
    rig.Sim().SetInterimNext(true);

    FCPFrame cmd{};
    const uint8_t queryBytes[] = {0x01, 0xFF, 0x18, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    std::copy(std::begin(queryBytes), std::end(queryBytes), cmd.data.begin());
    cmd.length = sizeof(queryBytes);

    bool interimDone = false;
    (void)rig.Transport().SubmitCommand(cmd, [&](FCPStatus status, const FCPFrame&) {
        interimDone = true;
        EXPECT_EQ(status, FCPStatus::kOk);
    });
    EXPECT_TRUE(interimDone);

    // Part 2: Timeout response
    rig.Mark("## TimeoutResponse");
    rig.Sim().SetTimeoutNext(true);

    bool timeoutDone = false;
    (void)rig.Transport().SubmitCommand(cmd, [&](FCPStatus status, const FCPFrame&) {
        timeoutDone = true;
        EXPECT_EQ(status, FCPStatus::kTimeout);
    });
    rig.Timers().Advance(51ULL * 1'000'000ULL);
    EXPECT_TRUE(timeoutDone);

    rig.ExpectGolden("generic__interim_and_timeout");
}
