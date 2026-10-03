// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcGoldenTests.cpp - Golden wire traces of TODAY's AV/C stack behaviour.
//
// Characterizes exact bus writes and sequences for:
// - Duet attach discovery (AVCUnit::Initialize, generic discovery)
// - Duet streaming start/stop (SignalFormat rate control + CMP)
// - Phase 88 attach discovery (AVCUnit::Initialize + BridgeCo inventory)
// - Onyx-i attach discovery (generic discovery against the documented Onyx-i capture)
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
#include "ExchangeReplayUnit.hpp"
#include "ASFWDriver/Protocols/AVC/Discovery/DiscoverySession.hpp"
#include "ASFWDriver/Protocols/AVC/Graph/DiscoveryGraph.hpp"
#include "ASFWDriver/UserClient/WireFormats/AVCDiscoveryDocument.hpp"
#include "ASFWDriver/Protocols/AVC/Discovery/DiscoveryLog.hpp"
#include "ASFWDriver/UserClient/Handlers/AVCHandler.hpp"
#include "ASFWDriver/Protocols/AVC/IAVCDiscovery.hpp"
#include <DriverKit/IOUserClient.h>
#include <DriverKit/OSData.h>
#include "DuetDescriptorFixture.hpp"
#include "Phase88DescriptorFixtures.hpp"
#include "ASFWDriver/Protocols/AVC/Graph/AvcDeviceGraph.hpp"
#include "FakeTimerScheduler.hpp"

#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Discovery/FWDevice.hpp"
#include "ASFWDriver/Discovery/FWUnit.hpp"

#include "ASFWDriver/Protocols/AVC/AVCUnit.hpp"
#include "ASFWDriver/Audio/Protocols/AVC/AvcAudioConfig.hpp"
#include "ASFWDriver/Audio/Protocols/AVC/AvcExtensionInventory.hpp"
#include "ASFWDriver/Protocols/AVC/FCPTransport.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/GeneralCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Core/RateCodes.hpp"

#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeDuetDuplex.hpp"

#include "ASFWDriver/Protocols/AVC/CMP/CMPClient.hpp"
#include "ASFWDriver/Bus/IRM/IRMClient.hpp"

#include <algorithm>
#include <cstring>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <regex>
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
    /// The extension inventory AVCDiscovery would give this unit.
    AVCUnit::DiscoveryOptions unitOptions{};
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

        avcUnit_ = std::make_shared<AVCUnit>(device_, fwUnit_, routes_, bus_, bus_, timers_,
                                             options_.unitOptions);

        simUnit_ = std::make_unique<SimulatedAvcUnit>(image);
        simUnit_->SetNodeId(FW::NodeId{static_cast<uint8_t>(options_.nodeId)});
        simUnit_->SetGeneration(FW::Generation{options_.generation});

        simUnit_->AttachToBus(bus_, [this](uint16_t srcNode, uint32_t gen, std::span<const uint8_t> payload) {
            if (activeTransport_) {
                activeTransport_->OnFCPResponse(srcNode, gen, payload);
            } else if (avcUnit_) {
                avcUnit_->GetFCPTransportShared()->OnFCPResponse(srcNode, gen, payload);
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
        // Every command must follow our write response to the previous answer.
        EXPECT_TRUE(simUnit_->CommandsWhileResponseOpen().empty())
            << simUnit_->CommandsWhileResponseOpen().size()
            << " command(s) written before the previous response was acknowledged";
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
    /// Run the work queued for after the receive handler: FCP delivers each
    /// response once our write response to it is out (FCPTransport::OnFCPResponse).
    void Settle() { timers_.Advance(0); }
    [[nodiscard]] Discovery::DeviceRegistry& Routes() noexcept { return routes_; }
    [[nodiscard]] std::shared_ptr<AVCUnit> Unit() noexcept { return avcUnit_; }
    [[nodiscard]] Protocols::AVC::FCPTransport& Transport() noexcept {
        return activeTransport_ ? *activeTransport_ : *avcUnit_->GetFCPTransportShared();
    }
    /// The transport as family code receives it: owned.
    [[nodiscard]] std::shared_ptr<Protocols::AVC::FCPTransport> TransportShared() noexcept {
        return activeTransport_ ? activeTransport_ : avcUnit_->GetFCPTransportShared();
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

// Attach must send only frames the device was captured answering, or frames a
// reference stack sends that the capture lacks (each listed with its source).
// The simulator answers any other frame NOT IMPLEMENTED, which hides frames a
// real device may never answer at all: a bare UNIT INFO wedged a Phase 88.
struct UncapturedFrame {
    const char* hexPattern;  // regex over the lowercase hex frame
    const char* source;
};

void ExpectOnlyMeasuredFrames(const SimulatedAvcUnit& sim, std::span<const UncapturedFrame> allowed) {
    for (const auto& frame : sim.UnmeasuredCommands()) {
        std::string hex;
        for (const auto byte : frame) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%02x", byte);
            hex += buf;
        }
        const bool known = std::ranges::any_of(allowed, [&hex](const UncapturedFrame& entry) {
            return std::regex_match(hex, std::regex(entry.hexPattern));
        });
        if (!known) {
            ADD_FAILURE() << "attach sent a frame the device was never captured answering: " << hex;
        }
    }
}

// OPEN DESCRIPTOR (read) on the unit: Apple's stack sends it
// (FireBug trace tools/pydice/isitduet.txt:112).
constexpr UncapturedFrame kAppleUnitOpenDescriptor{"00ff088001ff0000", "Apple, isitduet.txt:112"};

constexpr UncapturedFrame kDuetUncaptured[] = {
    kAppleUnitOpenDescriptor,
    // Phase 4 now collects lists for all inventoried audio plugs, routing,
    // and descriptor-advertised control STATUS. Codec layouts are cross-checked
    // with ta1394 stream-format lib.rs:785-1060, FFADO avc_signal_source.cpp:125-171,
    // and ta1394 audio lib.rs:820-862; these are reference-backed, not HW evidence.
    {"0108(bf|2f)c1.*", "ta1394 stream-format lib.rs:785-1060"},
    {"01ff1afffffe(ff|08|60)..", "TA 2002010 Figure 7.7, Table C.1; FFADO avc_signal_source.cpp:125-171"},
    {"(01|02)08b8(80|81).*", "ta1394 audio lib.rs:280-350,820-862"},
    {"02ff1a0f.*", "TA 2002010 Figure 7.1, Apple QuerySyncPlugReconnect 0x10f18, FFADO avc_signal_source.cpp (resultStatus & 0xF)"},

    // Music subunit status descriptor OPEN and READ: Apple, isitduet.txt:157,168.
    {"0060088001ff0000", "Apple, isitduet.txt:157"},
    {"00600980ff0000800+", "Apple, isitduet.txt:168"},
    // Audio subunit identifier OPEN: captured on the Phase 88, not on the Duet.
    {"0008080001ff0000", "Phase 88 descriptor capture"},
    // Audio subunit plug formats: FFADO avc_plug.cpp:231-249 asks every plug.
    {"0108(bf|2f)c0.*", "FFADO avc_plug.cpp:231"},
};

constexpr UncapturedFrame kPhase88Uncaptured[] = {
    kAppleUnitOpenDescriptor,
    // Phase 4 now collects lists for all inventoried audio plugs, routing,
    // and descriptor-advertised control STATUS. Codec layouts are cross-checked
    // with ta1394 stream-format lib.rs:785-1060, FFADO avc_signal_source.cpp:125-171,
    // and ta1394 audio lib.rs:820-862; these are reference-backed, not HW evidence.
    {"0108(bf|2f)c1.*", "ta1394 stream-format lib.rs:785-1060"},
    {"01ff1afffffe(ff|08|60)..", "TA 2002010 Figure 7.7, Table C.1; FFADO avc_signal_source.cpp:125-171"},
    {"(01|02)08b8(80|81).*", "ta1394 audio lib.rs:280-350,820-862"},
    {"02ff1a0f.*", "TA 2002010 Figure 7.1, Apple QuerySyncPlugReconnect 0x10f18, FFADO avc_signal_source.cpp (resultStatus & 0xF)"},

    // Audio subunit plug formats, 0x2F only: FFADO avc_plug.cpp:231-249 with
    // avc_extended_stream_format.cpp:296.
    {"01082fc0.*", "FFADO avc_plug.cpp:231"},
    // BridgeCo EXTENDED PLUG INFO, section type (info type 0x07) per cluster:
    // Linux bebob_command.c:214-227, bebob_stream.c:298.
    {"01ff02c00[01]000000ff07..00", "Linux bebob_command.c:214"},
};

using Reply = ASFW::AVC::Expected<ASFW::AVC::Response>;

// A frame given as wire bytes: ctype, address, opcode, operands.
ASFW::AVC::CommandFrame FrameOf(std::span<const uint8_t> bytes) {
    return *ASFW::AVC::CommandFrame::Make(static_cast<CommandType>(bytes[0] & 0x0F),
                                          ASFW::AVC::SubunitAddress::FromByte(bytes[1]),
                                          static_cast<ASFW::AVC::Opcode>(bytes[2]), bytes.subspan(3));
}

// nullopt for a response, else the error the engine reported.
std::optional<ASFW::AVC::AvcErrorKind> KindOf(const Reply& reply) {
    return reply ? std::nullopt : std::optional<ASFW::AVC::AvcErrorKind>{reply.error().kind};
}

TEST(AvcGoldenTests, ChainedCommandsDoNotGrowTheStack) {
    // Every completion submits the next command, and the simulated unit answers
    // inside the write -- as a bus reset fails every remaining command inside
    // Submit on hardware. FCPTransport must run each completion after the
    // previous one returned, not inside it: nested, a 227-command Phase 88
    // attach overflowed the stack under ASan, and this chain overflows it
    // without ASan (about 4 KB per command).
    AvcGoldenRig rig(kDuet);
    constexpr int kCommands = 2000;
    int answered = 0;
    uintptr_t lowest = UINTPTR_MAX;
    uintptr_t highest = 0;
    std::function<void()> next = [&] {
        rig.Unit()->Status(Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> reply) {
            int marker = 0;
            const auto here = reinterpret_cast<uintptr_t>(&marker);
            lowest = std::min(lowest, here);
            highest = std::max(highest, here);
            if (reply) {
                ++answered;
            }
            if (answered < kCommands && reply) {
                next();
            }
        });
    };
    next();
    rig.Settle();
    EXPECT_EQ(answered, kCommands);
    EXPECT_LT(highest - lowest, 64U * 1024U) << "completions nest: the stack grows with each command";
}

// ============================================================================
// 1. Duet Attach Discovery
// ============================================================================

TEST(AvcGoldenTests, DuetAttachDiscovery) {
    AvcGoldenRigOptions opts;
    opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kNone);
    AvcGoldenRig rig(kDuet, opts);

    // Attach as AVCDiscovery runs it: generic discovery, then the Oxford
    // stream-format lists in both directions.
    std::vector<uint8_t> audioIdentifier;
    for (size_t i = 0; i < Fixtures::kDuetAudioIdentifierHex.size(); i += 2)
        audioIdentifier.push_back(static_cast<uint8_t>(std::stoul(Fixtures::kDuetAudioIdentifierHex.substr(i, 2), nullptr, 16)));
    rig.Sim().SetDescriptor(0x08, {0x00}, std::move(audioIdentifier));
    rig.Mark("## AVCUnit::Initialize + Oxford inventory");
    bool initOk = false;
    rig.Unit()->Initialize([&](bool ok) { initOk = ok; });
    rig.Settle();
    EXPECT_TRUE(initOk);
    EXPECT_EQ(rig.Unit()->GetDiscoveryStatus(), Protocols::AVC::AVCDiscoveryStatus::Completed);

    rig.ExpectGolden("duet__phase4_attach_discovery");
    ExpectOnlyMeasuredFrames(rig.Sim(), kDuetUncaptured);
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
    rig.Settle();
    ASSERT_TRUE(done);
    const auto graph = rig.Unit()->GetDiscoveredGraph();
    ASSERT_TRUE(graph);
    EXPECT_EQ(graph->playback.subunitPlugId, 0);
    EXPECT_EQ(graph->capture.subunitPlugId, 1);
    EXPECT_EQ(graph->capture.channelNames[0], "Analog Out 1");
    EXPECT_EQ(graph->playback.dataBlockSize, 2);
    EXPECT_EQ(graph->capture.currentSampleRate, 48000); // Unit signal STATUS is authoritative.
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
    rig.Settle();
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
    rig.Settle();
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
    rig.Settle();
    EXPECT_TRUE(set48Done);

    rig.Mark("## ApogeeDuetDuplex::ProgramRx");
    IRM::IRMClient irm(rig.Bus());
    CMP::CMPClient cmp(rig.Bus(), rig.Bus(), rig.Routes());
    Audio::Oxford::Apogee::ApogeeDuetProtocol protocol(
        rig.Bus(), rig.Bus(), rig.Route(), &rig.Routes(), &irm, &cmp);
    protocol.UpdateRuntimeContext(rig.Route(), rig.TransportShared());
    auto& duplex = protocol.Duplex();

    bool rxDone = false;
    duplex.ProgramRx([&](IOReturn status, Audio::DuplexStageResult) {
        rxDone = true;
        EXPECT_EQ(status, kIOReturnSuccess);
    });
    rig.Settle();
    EXPECT_TRUE(rxDone);

    rig.Mark("## ApogeeDuetDuplex::ProgramTxAndEnable");
    bool txDone = false;
    duplex.ProgramTxAndEnableDuplex([&](IOReturn status, Audio::DuplexStageResult) {
        txDone = true;
        EXPECT_EQ(status, kIOReturnSuccess);
    });
    rig.Settle();
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
    opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kBridgeCo);
    AvcGoldenRig rig(kPhase88, opts);
    // The device's own descriptors and its unit ISO output 0 source (captured
    // 2026-09-28): the attach image predates descriptor capture.
    rig.Sim().SetDescriptor(0x60, {0x80}, Fixtures::Phase88MusicStatus());
    rig.Sim().SetDescriptor(0x60, {0x00}, Fixtures::kPhase88MusicIdentifier);
    rig.Sim().SetDescriptor(0x08, {0x00}, Fixtures::kPhase88AudioIdentifier);
    rig.Sim().SetDescriptor(0x08, {0x10, 0x18, 0x00}, Fixtures::kPhase88TextRoot);
    rig.Sim().SetDescriptor(0x08, {0x10, 0x18, 0x01}, Fixtures::kPhase88TextChild);
    rig.Sim().SetResponseOverride({0x01, 0xFF, 0x1A, 0xFF, 0xFF, 0xFE, 0xFF, 0x00},
                                  {0x0C, 0xFF, 0x1A, 0x10, 0x60, 0x00, 0xFF, 0x00});

    // Attach as AVCDiscovery runs it: generic discovery, then the BridgeCo
    // inventory, before the discovery status completes.
    rig.Mark("## AVCUnit::Initialize + BridgeCo inventory");
    bool initOk = false;
    rig.Unit()->Initialize([&](bool ok) { initOk = ok; });
    rig.Settle();
    EXPECT_TRUE(initOk);
    EXPECT_EQ(rig.Unit()->GetDiscoveryStatus(), Protocols::AVC::AVCDiscoveryStatus::Completed);

    // The live BridgeCo formations size both streams at the current 48 kHz,
    // over the music subunit's own (stale) capture format.
    const auto graph = rig.Unit()->GetDiscoveredGraph();
    ASSERT_NE(graph, nullptr);
    EXPECT_EQ(graph->playback.channelCount, 10U);
    EXPECT_EQ(graph->playback.dataBlockSize, 11U);
    EXPECT_EQ(graph->capture.channelCount, 10U);
    EXPECT_EQ(graph->capture.dataBlockSize, 11U);
    EXPECT_EQ(graph->playback.currentSampleRate, 48000U);
    EXPECT_EQ(graph->capture.currentSampleRate, 48000U);
    EXPECT_EQ(graph->playback.supportedSampleRates.size(), 5U);
    // Planar playback block, as the BridgeCo channel positions also say.
    constexpr uint8_t kPlanar[]{1, 6, 2, 7, 3, 8, 4, 9, 0, 5};
    for (uint32_t channel = 0; channel < 10; ++channel) {
        EXPECT_EQ(graph->playback.slotMap.SlotFor(channel), kPlanar[channel]) << "channel " << channel;
    }
    // Labels from the source plugs' audio info blocks, per music plug
    // (TA 2001007 Table 6.2): capture reads source plug 0's list; playback takes
    // the label of the output each channel is routed to (source plugs 1 and 2).
    ASSERT_EQ(graph->capture.channelNames.size(), 10U);
    EXPECT_EQ(graph->capture.channelNames[0], "Line_1/2 left PHASE88 FW");
    EXPECT_EQ(graph->capture.channelNames[1], "Line_1/2 right PHASE88 FW");
    EXPECT_EQ(graph->capture.channelNames[9], "SPDIF right PHASE88 FW");
    ASSERT_EQ(graph->playback.channelNames.size(), 10U);
    EXPECT_EQ(graph->playback.channelNames[0], "Multichannel 1 PHASE88 FW");
    EXPECT_EQ(graph->playback.channelNames[7], "Multichannel 8 PHASE88 FW");
    EXPECT_EQ(graph->playback.channelNames[8], "SPDIF/AC3 left PHASE88 FW");
    EXPECT_EQ(graph->playback.channelNames[9], "SPDIF/AC3 right PHASE88 FW");

    // What CoreAudio is offered: the one rate the device starts at.
    DeviceProfiles::Audio::StaticAudioEndpointPlan plan{};
    plan.profileBuilder = DeviceProfiles::Audio::ProfileBuilderId::TerraTecPhase88;
    plan.streamTraits.wire.forcedStreamMode = DeviceProfiles::Audio::ForcedStreamMode::Blocking;
    const auto config = BuildGraphAudioConfig(
        {.guid = kPhase88.guid, .vendorId = 0x000AAC, .modelId = 3, .modelName = "PHASE 88 Rack FW"},
        plan, *graph);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->sampleRates, std::vector<uint32_t>{48000U});
    EXPECT_EQ(config->currentSampleRate, 48000U);
    EXPECT_EQ(config->inputChannelCount, 10U);
    EXPECT_EQ(config->outputChannelCount, 10U);
    ASSERT_EQ(config->playbackStreams.size(), 1U);
    EXPECT_EQ(config->playbackStreams[0].am824Slots, 11U);
    EXPECT_EQ(config->playbackStreams[0].pcmSlotMap.SlotFor(0), 1U);
    EXPECT_EQ(config->streamMode, Audio::Model::StreamMode::kBlocking);

    rig.ExpectGolden("phase88__phase4_attach_discovery");
    ExpectOnlyMeasuredFrames(rig.Sim(), kPhase88Uncaptured);
}

TEST(AvcGoldenTests, Phase88GeometryWithoutAnyDescriptorComesFromBridgeCoFormations) {
    // No descriptor answers at all: formations and the current signal format
    // still give both streams' geometry, rates and the BridgeCo slot map.
    AvcGoldenRigOptions opts;
    opts.guid = kPhase88.guid;
    opts.nodeId = static_cast<uint16_t>(kPhase88.nodeId);
    opts.generation = kPhase88.generation;
    opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kBridgeCo);
    AvcGoldenRig rig(kPhase88, opts);
    bool initOk = false;
    rig.Unit()->Initialize([&](bool ok) { initOk = ok; });
    rig.Settle();
    ASSERT_TRUE(initOk);
    const auto snapshot = rig.Unit()->GetDiscoverySnapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_TRUE(std::none_of(snapshot->contents.begin(), snapshot->contents.end(),
                             [](const auto& c) { return c.music.has_value(); }));
    const auto graph = rig.Unit()->GetDiscoveredGraph();
    ASSERT_NE(graph, nullptr);
    for (const auto* stream : {&graph->playback, &graph->capture}) {
        EXPECT_EQ(stream->channelCount, 10U);
        EXPECT_EQ(stream->dataBlockSize, 11U);
        EXPECT_EQ(stream->currentSampleRate, 48000U);
        EXPECT_EQ(stream->supportedSampleRates.size(), 5U);
    }
    // The attach image holds no BridgeCo channel-position answers, so the
    // inventory reports no map and the stream keeps the identity layout; the
    // graph uses exactly what the inventory reported, never a guess.
    EXPECT_EQ(graph->playback.slotMap, snapshot->extension.playback.pcmSlots);
    EXPECT_EQ(graph->capture.slotMap, snapshot->extension.capture.pcmSlots);
    // Frame admission is pinned by Phase88AttachDiscovery; here the descriptor
    // OPENs are refused by the image rather than answered from a capture.
}


// ============================================================================
// Export -> replay -> same contents and graph (phase 4.5)
// ============================================================================

namespace ReplayChecks {
namespace E = ASFW::AVC::DiscoveryEngine;

/// Replay through the same session/reducer, with the same chip inventory as
/// attach (the hook takes IAvcUnit, so it runs against the recorded unit).
E::SnapshotLease Replay(const std::shared_ptr<AVCUnit>& unit, AvcExtensionInventory inventory,
                        ExchangeReplayUnit*& replayOut, std::unique_ptr<ExchangeReplayUnit>& owner) {
    const auto original = unit->GetDiscoverySnapshot();
    owner = std::make_unique<ExchangeReplayUnit>(unit->CopyExchangeLog(), original->route.guid,
                                                 FW::NodeId{static_cast<uint8_t>(original->route.nodeId)},
                                                 original->route.generation);
    owner->SetStreamFormatOpcodePolicy(unit->GetStreamFormatOpcodePolicy());
    replayOut = owner.get();
    E::SnapshotLease replayed;
    const auto options = DiscoveryOptionsFor(inventory);
    auto* recorded = owner.get();
    E::Session::Extension extension;
    if (options.extensionInventory) {
        extension = [recorded, run = options.extensionInventory](E::SnapshotLease discovered, std::function<void(E::ExtensionFacts)> done) {
            run(*recorded, std::move(discovered), std::move(done));
        };
    }
    auto session = E::Session::Create(*owner, original->session, [&](E::SnapshotLease r) { replayed = std::move(r); },
                                      std::move(extension));
    session->Start();
    return replayed;
}

void ExpectSameContents(const E::DiscoverySnapshot& a, const E::DiscoverySnapshot& b) {
    EXPECT_EQ(a.complete, b.complete);
    EXPECT_EQ(a.unit.subunits, b.unit.subunits);
    ASSERT_EQ(a.plugs.size(), b.plugs.size());
    for (size_t i = 0; i < a.plugs.size(); ++i) {
        const auto& x = a.plugs[i]; const auto& y = b.plugs[i];
        EXPECT_EQ(x.address, y.address); EXPECT_EQ(x.direction, y.direction); EXPECT_EQ(x.id.value, y.id.value);
        EXPECT_EQ(x.current.has_value(), y.current.has_value());
        if (x.current && y.current) EXPECT_TRUE(std::ranges::equal(x.current->Raw(), y.current->Raw()));
        ASSERT_EQ(x.formations.size(), y.formations.size()) << "plug " << i;
        for (size_t f = 0; f < x.formations.size(); ++f)
            EXPECT_TRUE(std::ranges::equal(x.formations[f].Raw(), y.formations[f].Raw()));
        EXPECT_EQ(x.route.has_value(), y.route.has_value());
    }
    ASSERT_EQ(a.descriptors.size(), b.descriptors.size());
    for (size_t i = 0; i < a.descriptors.size(); ++i) EXPECT_EQ(a.descriptors[i].bytes, b.descriptors[i].bytes);
    EXPECT_EQ(a.features.size(), b.features.size());
    EXPECT_EQ(a.selectors.size(), b.selectors.size());
    EXPECT_EQ(a.confirmedClockRoutes.size(), b.confirmedClockRoutes.size());
    for (const auto& [x, y] : {std::pair{&a.extension.playback, &b.extension.playback},
                               std::pair{&a.extension.capture, &b.extension.capture}}) {
        EXPECT_EQ(x->formations, y->formations);
        EXPECT_EQ(x->pcmSlots, y->pcmSlots);
        EXPECT_EQ(x->currentRateHz, y->currentRateHz);
    }
}

void ExpectSameGraph(const Graph::DeviceGraph& a, const Graph::DeviceGraph& b) {
    for (const auto& [x, y] : {std::pair{&a.playback, &b.playback}, std::pair{&a.capture, &b.capture}}) {
        EXPECT_EQ(x->channelCount, y->channelCount);
        EXPECT_EQ(x->dataBlockSize, y->dataBlockSize);
        EXPECT_EQ(x->currentSampleRate, y->currentSampleRate);
        EXPECT_EQ(x->supportedSampleRates, y->supportedSampleRates);
        EXPECT_EQ(x->slotMap, y->slotMap);
        EXPECT_EQ(x->channelNames, y->channelNames);
    }
    EXPECT_EQ(a.clockSources.size(), b.clockSources.size());
    EXPECT_EQ(a.selectors.size(), b.selectors.size());
}
} // namespace ReplayChecks

TEST(AvcGoldenTests, DuetExchangeLogReplaysToTheSameContentsAndGraph) {
    AvcGoldenRigOptions opts;
    opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kNone);
    AvcGoldenRig rig(kDuet, opts);
    std::vector<uint8_t> audioIdentifier(Fixtures::kDuetAudioIdentifierBytes.begin(), Fixtures::kDuetAudioIdentifierBytes.end());
    rig.Sim().SetDescriptor(0x08, {0x00}, std::move(audioIdentifier));
    bool ok = false;
    rig.Unit()->Initialize([&](bool done) { ok = done; });
    rig.Settle();
    ASSERT_TRUE(ok);
    const auto original = rig.Unit()->GetDiscoverySnapshot();
    ExchangeReplayUnit* replay = nullptr;
    std::unique_ptr<ExchangeReplayUnit> owner;
    const auto replayed = ReplayChecks::Replay(rig.Unit(), AvcExtensionInventory::kNone, replay, owner);
    ASSERT_TRUE(replayed);
    EXPECT_TRUE(replay->Unmatched().empty()) << "replay sent a frame the capture never saw";
    EXPECT_EQ(replay->Unused(), 0U) << "part of the capture was never replayed";
    EXPECT_GT(replay->Replayed(), 40U);
    ReplayChecks::ExpectSameContents(*original, *replayed);
    ReplayChecks::ExpectSameGraph(Graph::BuildDiscoveryGraph(*original, "Duet"), Graph::BuildDiscoveryGraph(*replayed, "Duet"));
}

TEST(AvcGoldenTests, Phase88ExchangeLogReplaysToTheSamePublishedShape) {
    AvcGoldenRigOptions opts;
    opts.guid = kPhase88.guid;
    opts.nodeId = static_cast<uint16_t>(kPhase88.nodeId);
    opts.generation = kPhase88.generation;
    opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kBridgeCo);
    AvcGoldenRig rig(kPhase88, opts);
    rig.Sim().SetDescriptor(0x60, {0x80}, Fixtures::Phase88MusicStatus());
    rig.Sim().SetDescriptor(0x60, {0x00}, Fixtures::kPhase88MusicIdentifier);
    rig.Sim().SetDescriptor(0x08, {0x00}, Fixtures::kPhase88AudioIdentifier);
    rig.Sim().SetDescriptor(0x08, {0x10, 0x18, 0x00}, Fixtures::kPhase88TextRoot);
    rig.Sim().SetDescriptor(0x08, {0x10, 0x18, 0x01}, Fixtures::kPhase88TextChild);
    bool ok = false;
    rig.Unit()->Initialize([&](bool done) { ok = done; });
    rig.Settle();
    ASSERT_TRUE(ok);
    const auto original = rig.Unit()->GetDiscoverySnapshot();
    ExchangeReplayUnit* replay = nullptr;
    std::unique_ptr<ExchangeReplayUnit> owner;
    const auto replayed = ReplayChecks::Replay(rig.Unit(), AvcExtensionInventory::kBridgeCo, replay, owner);
    ASSERT_TRUE(replayed);
    EXPECT_TRUE(replay->Unmatched().empty()) << "replay sent a frame the capture never saw";
    EXPECT_EQ(replay->Unused(), 0U) << "part of the capture was never replayed";
    EXPECT_GT(replay->Replayed(), 40U);
    ReplayChecks::ExpectSameContents(*original, *replayed);
    // The BridgeCo inventory replays too, so the published shape must match:
    // 10 PCM + 1 MIDI at 48 kHz, five rates, and the slot map.
    EXPECT_FALSE(replayed->extension.playback.formations.empty());
    const auto replayedGraph = Graph::BuildDiscoveryGraph(*replayed, "Phase 88");
    ReplayChecks::ExpectSameGraph(*rig.Unit()->GetDiscoveredGraph(), replayedGraph);
    EXPECT_EQ(replayedGraph.playback.channelCount, 10U);
    EXPECT_EQ(replayedGraph.playback.dataBlockSize, 11U);
    EXPECT_EQ(replayedGraph.playback.currentSampleRate, 48000U);
    EXPECT_EQ(replayedGraph.playback.supportedSampleRates.size(), 5U);
}

TEST(AvcGoldenTests, Phase88DiscoveryDocumentPagesReassembleWithinTheWireLimit) {
    AvcGoldenRigOptions opts;
    opts.guid = kPhase88.guid;
    opts.nodeId = static_cast<uint16_t>(kPhase88.nodeId);
    opts.generation = kPhase88.generation;
    opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kBridgeCo);
    AvcGoldenRig rig(kPhase88, opts);
    rig.Sim().SetDescriptor(0x60, {0x80}, Fixtures::Phase88MusicStatus());
    rig.Sim().SetDescriptor(0x60, {0x00}, Fixtures::kPhase88MusicIdentifier);
    rig.Sim().SetDescriptor(0x08, {0x00}, Fixtures::kPhase88AudioIdentifier);
    rig.Sim().SetDescriptor(0x08, {0x10, 0x18, 0x00}, Fixtures::kPhase88TextRoot);
    rig.Sim().SetDescriptor(0x08, {0x10, 0x18, 0x01}, Fixtures::kPhase88TextChild);
    bool ok = false;
    rig.Unit()->Initialize([&](bool done) { ok = done; });
    rig.Settle();
    ASSERT_TRUE(ok);
    const auto snapshot = rig.Unit()->GetDiscoverySnapshot();
    const auto graph = rig.Unit()->GetDiscoveredGraph();
    const auto document = UserClient::Wire::BuildAVCDiscoveryDocument(snapshot.get(), graph.get(), rig.Unit()->CopyExchangeLog());

    // Well-formed: brackets balance outside strings, and the key facts are there.
    int depth = 0; bool inString = false, escaped = false;
    for (const char c : document) {
        if (inString) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') inString = false; continue; }
        if (c == '"') inString = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') { --depth; ASSERT_GE(depth, 0); }
        ASSERT_TRUE(static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F);
    }
    EXPECT_EQ(depth, 0);
    EXPECT_FALSE(inString);
    EXPECT_NE(document.find("\"format\":\"asfw.avc.discovery\""), std::string::npos);
    EXPECT_NE(document.find("\"complete\":true"), std::string::npos);
    EXPECT_NE(document.find("\"channels\":10"), std::string::npos);
    EXPECT_NE(document.find("\"elapsedUs\":"), std::string::npos);

    std::string reassembled;
    const auto checksum = UserClient::Wire::Fnv1a32(document);
    for (uint32_t offset = 0;;) {
        const auto page = UserClient::Wire::SerializeDiscoveryPage(document, static_cast<uint32_t>(snapshot->session.value),
                                                                   snapshot->route.generation.value, offset, 4096);
        ASSERT_LE(page.size(), 4096U);
        UserClient::Wire::AVCDiscoveryPageWire header{};
        std::memcpy(&header, page.data(), sizeof(header));
        EXPECT_EQ(header.magic, UserClient::Wire::kAVCDiscoveryDocumentMagic);
        EXPECT_EQ(header.totalBytes, document.size());
        EXPECT_EQ(header.checksum, checksum);
        EXPECT_EQ(header.offset, offset);
        if (header.length == 0) break;
        reassembled.append(reinterpret_cast<const char*>(page.data() + sizeof(header)), header.length);
        offset += header.length;
    }
    EXPECT_EQ(reassembled, document);
    EXPECT_GT(document.size(), 4096U) << "the Phase 88 document should need several pages";
    // The whole document, pinned: the Swift app's fixture is this file (ASFWTests/Fixtures).
    ::ASFW::Testing::ExpectTextMatchesGolden(document + "\n", "avc/phase88__discovery_document.json");
}


// ============================================================================
// User-client outputs pinned byte for byte (legacy removal bar)
// ============================================================================

namespace UserClientGolden {
/// The unit as the user client sees it through discovery.
class OneUnitDiscovery final : public IAVCDiscovery {
public:
    explicit OneUnitDiscovery(std::shared_ptr<AVCUnit> unit) : unit_(std::move(unit)) {}
    std::shared_ptr<AVCUnit> Unit(uint64_t) override { return unit_; }
    std::vector<std::shared_ptr<AVCUnit>> Units() override { return {unit_}; }
    void ReScanAllUnits() override {}
    std::shared_ptr<ASFW::AVC::IAvcUnit> LiveUnit(uint64_t) override { return nullptr; }
    std::shared_ptr<FCPTransport> AcquireFCPTransportForNodeID(uint16_t) override { return nullptr; }
private:
    std::shared_ptr<AVCUnit> unit_;
};

std::string Hex(const OSData* data) {
    if (!data) return "<none>\n";
    const auto* bytes = static_cast<const uint8_t*>(data->getBytesNoCopy());
    std::string out;
    for (size_t i = 0; i < data->getLength(); ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02x", bytes[i]);
        out += buf;
        out += (i % 32 == 31) ? '\n' : ' ';
    }
    return out + "\n";
}

/// Every user-client output derived from discovery, as text.
std::string Capture(const std::shared_ptr<AVCUnit>& unit, uint64_t guid) {
    OneUnitDiscovery discovery(unit);
    UserClient::AVCHandler handler(&discovery);
    std::string out;
    const auto call = [&](const char* name, auto method, std::vector<uint64_t> scalars) {
        IOUserClientMethodArguments args{};
        args.scalarInput = scalars.data();
        args.scalarInputCount = static_cast<uint32_t>(scalars.size());
        const auto kr = (handler.*method)(&args);
        char header[96];
        std::snprintf(header, sizeof(header), "## %s kr=0x%08x\n", name, static_cast<unsigned>(kr));
        out += header;
        out += Hex(kr == kIOReturnSuccess ? args.structureOutput : nullptr);
        if (args.structureOutput) args.structureOutput->release();
    };
    call("GetAVCUnits", &UserClient::AVCHandler::GetAVCUnits, {});
    for (const auto& sub : unit->GetDiscoverySnapshot()->unit.subunits) {
        const auto type = static_cast<uint64_t>(sub.id.type);
        const std::vector<uint64_t> scalars{guid >> 32, guid & 0xFFFFFFFFu, type, sub.id.id};
        char name[64];
        std::snprintf(name, sizeof(name), "GetSubunitCapabilities type=%02llx id=%u",
                      static_cast<unsigned long long>(type), sub.id.id);
        call(name, &UserClient::AVCHandler::GetSubunitCapabilities, scalars);
        std::snprintf(name, sizeof(name), "GetSubunitDescriptor type=%02llx id=%u",
                      static_cast<unsigned long long>(type), sub.id.id);
        call(name, &UserClient::AVCHandler::GetSubunitDescriptor, scalars);
    }
    return out;
}
} // namespace UserClientGolden

TEST(AvcGoldenTests, DuetUserClientOutputsAreUnchanged) {
    AvcGoldenRigOptions opts;
    opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kNone);
    AvcGoldenRig rig(kDuet, opts);
    std::vector<uint8_t> music;
    for (size_t i = 0; i < Fixtures::kDuetMusicStatusHex.size(); i += 2)
        music.push_back(static_cast<uint8_t>(std::stoul(Fixtures::kDuetMusicStatusHex.substr(i, 2), nullptr, 16)));
    rig.Sim().SetDescriptor(0x60, {0x80}, std::move(music));
    rig.Sim().SetDescriptor(0x08, {0x00}, std::vector<uint8_t>(Fixtures::kDuetAudioIdentifierBytes.begin(),
                                                              Fixtures::kDuetAudioIdentifierBytes.end()));
    bool ok = false;
    rig.Unit()->Initialize([&](bool done) { ok = done; });
    rig.Settle();
    ASSERT_TRUE(ok);
    ::ASFW::Testing::ExpectTextMatchesGolden(UserClientGolden::Capture(rig.Unit(), kDuet.guid),
                                             "avc/duet__user_client.txt");
}

// What the driver ring shows for an attach: every discovered fact by its spec name, every unnamed value as
// UNKNOWN. The golden pins each name, so a renamed or dropped table entry fails here.
namespace {
[[nodiscard]] std::string JoinLines(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) text += line + "\n";
    return text;
}
} // namespace

TEST(AvcGoldenTests, Phase88DiscoveryLogNamesEveryFact) {
    AvcGoldenRigOptions opts;
    opts.guid = kPhase88.guid;
    opts.nodeId = static_cast<uint16_t>(kPhase88.nodeId);
    opts.generation = kPhase88.generation;
    opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kBridgeCo);
    AvcGoldenRig rig(kPhase88, opts);
    rig.Sim().SetDescriptor(0x60, {0x80}, Fixtures::Phase88MusicStatus());
    rig.Sim().SetDescriptor(0x60, {0x00}, Fixtures::kPhase88MusicIdentifier);
    rig.Sim().SetDescriptor(0x08, {0x00}, Fixtures::kPhase88AudioIdentifier);
    rig.Sim().SetDescriptor(0x08, {0x10, 0x18, 0x00}, Fixtures::kPhase88TextRoot);
    rig.Sim().SetDescriptor(0x08, {0x10, 0x18, 0x01}, Fixtures::kPhase88TextChild);
    rig.Sim().SetResponseOverride({0x01, 0xFF, 0x1A, 0xFF, 0xFF, 0xFE, 0xFF, 0x00},
                                  {0x0C, 0xFF, 0x1A, 0x10, 0x60, 0x00, 0xFF, 0x00});
    bool ok = false;
    rig.Unit()->Initialize([&](bool done) { ok = done; });
    rig.Settle();
    ASSERT_TRUE(ok);
    const auto lines = ASFW::AVC::DiscoveryEngine::DescribeDiscovery(*rig.Unit()->GetDiscoverySnapshot(),
                                                                     rig.Unit()->GetDiscoveredGraph().get());
    for (const auto& line : lines) EXPECT_LE(line.size(), ASFW::AVC::DiscoveryEngine::kMaxDiscoveryLogLine) << line;
    ::ASFW::Testing::ExpectTextMatchesGolden(JoinLines(lines), "avc/phase88__discovery_log.txt");
}

TEST(AvcGoldenTests, DuetDiscoveryLogNamesEveryFact) {
    AvcGoldenRigOptions opts;
    opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kNone);
    AvcGoldenRig rig(kDuet, opts);
    std::vector<uint8_t> music;
    for (size_t i = 0; i < Fixtures::kDuetMusicStatusHex.size(); i += 2)
        music.push_back(static_cast<uint8_t>(std::stoul(Fixtures::kDuetMusicStatusHex.substr(i, 2), nullptr, 16)));
    rig.Sim().SetDescriptor(0x60, {0x80}, std::move(music));
    rig.Sim().SetDescriptor(0x08, {0x00}, std::vector<uint8_t>(Fixtures::kDuetAudioIdentifierBytes.begin(),
                                                              Fixtures::kDuetAudioIdentifierBytes.end()));
    bool ok = false;
    rig.Unit()->Initialize([&](bool done) { ok = done; });
    rig.Settle();
    ASSERT_TRUE(ok);
    const auto lines = ASFW::AVC::DiscoveryEngine::DescribeDiscovery(*rig.Unit()->GetDiscoverySnapshot(),
                                                                     rig.Unit()->GetDiscoveredGraph().get());
    for (const auto& line : lines) EXPECT_LE(line.size(), ASFW::AVC::DiscoveryEngine::kMaxDiscoveryLogLine) << line;
    ::ASFW::Testing::ExpectTextMatchesGolden(JoinLines(lines), "avc/duet__discovery_log.txt");
}

// The route's STATUS byte used to be dropped from the discovery document. It is there now, with the
// spec names beside the bytes, so a document reader sees `ready` and the departure from Table 7.7.
TEST(AvcGoldenTests, DuetDiscoveryDocumentCarriesTheDecodedRouteStatus) {
    AvcGoldenRigOptions opts;
    opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kNone);
    AvcGoldenRig rig(kDuet, opts);
    std::vector<uint8_t> music;
    for (size_t i = 0; i < Fixtures::kDuetMusicStatusHex.size(); i += 2)
        music.push_back(static_cast<uint8_t>(std::stoul(Fixtures::kDuetMusicStatusHex.substr(i, 2), nullptr, 16)));
    rig.Sim().SetDescriptor(0x60, {0x80}, std::move(music));
    rig.Sim().SetDescriptor(0x08, {0x00}, std::vector<uint8_t>(Fixtures::kDuetAudioIdentifierBytes.begin(),
                                                              Fixtures::kDuetAudioIdentifierBytes.end()));
    bool ok = false;
    rig.Unit()->Initialize([&](bool done) { ok = done; });
    rig.Settle();
    ASSERT_TRUE(ok);
    const auto snapshot = rig.Unit()->GetDiscoverySnapshot();
    const auto graph = rig.Unit()->GetDiscoveredGraph();
    const std::string document = UserClient::Wire::BuildAVCDiscoveryDocument(snapshot.get(), graph.get(), rig.Unit()->CopyExchangeLog());

    ASSERT_EQ(graph->probeResults.size(), snapshot->outcomes.size());
    bool rejected = false, unsupported = false, stable = false;
    for (const auto& probe : graph->probeResults) {
        EXPECT_FALSE(probe.command.empty());
        if (probe.responseCode == ResponseCode::kRejected) rejected = true;
        if (probe.responseCode == ResponseCode::kNotImplemented) unsupported = true;
        if (probe.responseCode == ResponseCode::kImplementedStable) {
            stable = true;
            EXPECT_FALSE(probe.responseOperands.empty());
        }
    }
    EXPECT_TRUE(rejected && unsupported && stable);
    EXPECT_NE(document.find("\"probeResults\":"), std::string::npos);
    EXPECT_NE(document.find("\"responseName\":\"NOT IMPLEMENTED(0x8)\""), std::string::npos);
    EXPECT_NE(document.find("\"firstOperand\":112"), std::string::npos);
    EXPECT_NE(document.find("\"status\":\"output_status=ready(0x3) conv=can change format(1) signal_status=identical(0x0)\""),
              std::string::npos);
    EXPECT_NE(document.find("\"sourceName\":\"iPCR[0] [ff 00]\""), std::string::npos);
    EXPECT_NE(document.find("\"deviations\":\"output_status beyond effective/not effective; conv set on a plug that is not an oPCR\""),
              std::string::npos);
}

TEST(AvcGoldenTests, Phase88UserClientOutputsAreUnchanged) {
    AvcGoldenRigOptions opts;
    opts.guid = kPhase88.guid;
    opts.nodeId = static_cast<uint16_t>(kPhase88.nodeId);
    opts.generation = kPhase88.generation;
    opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kBridgeCo);
    AvcGoldenRig rig(kPhase88, opts);
    rig.Sim().SetDescriptor(0x60, {0x80}, Fixtures::Phase88MusicStatus());
    rig.Sim().SetDescriptor(0x60, {0x00}, Fixtures::kPhase88MusicIdentifier);
    rig.Sim().SetDescriptor(0x08, {0x00}, Fixtures::kPhase88AudioIdentifier);
    rig.Sim().SetDescriptor(0x08, {0x10, 0x18, 0x00}, Fixtures::kPhase88TextRoot);
    rig.Sim().SetDescriptor(0x08, {0x10, 0x18, 0x01}, Fixtures::kPhase88TextChild);
    bool ok = false;
    rig.Unit()->Initialize([&](bool done) { ok = done; });
    rig.Settle();
    ASSERT_TRUE(ok);
    ::ASFW::Testing::ExpectTextMatchesGolden(UserClientGolden::Capture(rig.Unit(), kPhase88.guid),
                                             "avc/phase88__user_client.txt");
}

// ============================================================================
// 4. Onyx-i Attach Discovery
// ============================================================================

TEST(AvcGoldenTests, OnyxiCapturedFormationsDecodeWithTheCanonicalCodec) {
    // The documented Onyx-i capture holds only its unit ISO input plug 0
    // stream-format list (0xBF C1, four entries then REJECTED). It has no
    // PLUG INFO answer, so generic discovery cannot be replayed against it;
    // what it proves is the decode: four compound formats of 8 PCM, no MIDI.
    std::vector<uint32_t> rates;
    for (const auto& command : OnyxiData::kOnyxi.records) {
        if (command.responseCode != 0x0C) continue;
        const auto response = ParseResponse(command.response);
        ASSERT_TRUE(response.has_value()) << command.name;
        const auto reply = Cmd::StreamFormatCommand{
            .operands = {.form = Cmd::StreamFormatSubfunction::kList,
                         .opcode = Cmd::StreamFormatOpcode::kExtendedStreamFormat,
                         .plug = Cmd::PlugAddress::UnitPlug(Cmd::PlugDirection::kInput, Cmd::UnitPlugType::kPcr, 0),
                         .index = command.command[10]}}.Decode(response->operands);
        ASSERT_TRUE(reply.has_value()) << command.name;
        ASSERT_EQ(reply->format.kind, Cmd::StreamFormat::Kind::kCompoundAm824) << command.name;
        EXPECT_EQ(reply->format.compound.PcmChannels(), 8U);
        EXPECT_EQ(reply->format.compound.MidiChannels(), 0U);
        rates.push_back(*ToHz(reply->format.compound.rate));
    }
    EXPECT_EQ(rates, (std::vector<uint32_t>{44100, 48000, 96000, 88200}));
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
    bool inDone = false;
    rig.Transport().Submit(*inFrame, rig.Route().generation, [&](Reply reply) {
        inDone = true;
        EXPECT_EQ(KindOf(reply), std::nullopt);
    });
    rig.Settle();
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
    bool outDone = false;
    rig.Transport().Submit(*outFrame, rig.Route().generation, [&](Reply reply) {
        outDone = true;
        EXPECT_EQ(KindOf(reply), std::nullopt);
    });
    rig.Settle();
    EXPECT_TRUE(outDone);

    rig.Mark("## Allowed: RateControl48k");
    rig.Sim().SetResponseOverride(
        {0x00, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF},
        {0x09, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF});
    constexpr uint8_t ctrlBytes[] = {0x00, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF};
    bool ctrlDone = false;
    rig.Transport().Submit(FrameOf(ctrlBytes), rig.Route().generation, [&](Reply reply) {
        ctrlDone = true;
        EXPECT_EQ(KindOf(reply), std::nullopt);
    });
    rig.Settle();
    EXPECT_TRUE(ctrlDone);

    // Refused commands (must never appear on wire)
    auto expectRefused = [&](const char* label, std::initializer_list<uint8_t> bytes) {
        rig.Mark(std::string("## Refused: ") + label);
        const std::vector<uint8_t> wire(bytes);
        bool refused = false;
        rig.Transport().Submit(FrameOf(wire), rig.Route().generation, [&](Reply reply) {
            refused = true;
            EXPECT_EQ(KindOf(reply), ASFW::AVC::AvcErrorKind::kRefused);
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

    // A STATUS: idempotent, so the reset may replay it on the rebound route.
    constexpr uint8_t queryBytes[] = {0x01, 0xFF, 0x18, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    bool done = false;
    rig.Transport().Submit(FrameOf(queryBytes), rig.Route().generation, [&](Reply reply) {
        done = true;
        EXPECT_EQ(KindOf(reply), std::nullopt);
    });

    rig.Settle();

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

    constexpr uint8_t queryBytes[] = {0x01, 0xFF, 0x18, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    const auto cmd = FrameOf(queryBytes);

    bool interimDone = false;
    rig.Transport().Submit(cmd, rig.Route().generation, [&](Reply reply) {
        interimDone = true;
        EXPECT_EQ(KindOf(reply), std::nullopt);
    });
    rig.Settle();
    EXPECT_TRUE(interimDone);

    // Part 2: Timeout response
    rig.Mark("## TimeoutResponse");
    rig.Sim().SetTimeoutNext(true);

    bool timeoutDone = false;
    rig.Transport().Submit(cmd, rig.Route().generation, [&](Reply reply) {
        timeoutDone = true;
        EXPECT_EQ(KindOf(reply), ASFW::AVC::AvcErrorKind::kTimeout);
    });
    rig.Timers().Advance(51ULL * 1'000'000ULL);
    rig.Settle();
    EXPECT_TRUE(timeoutDone);

    rig.ExpectGolden("generic__interim_and_timeout");
}

// ============================================================================
// Extension inventory runs inside the discovery status
// ============================================================================

TEST(AvcGoldenTests, ExtensionInventoryHoldsDiscoveryOpenUntilItFinishes) {
    std::function<void()> finish;
    AvcGoldenRigOptions opts;
    opts.unitOptions.extensionInventory = [&finish](ASFW::AVC::IAvcUnit&, ASFW::AVC::DiscoveryEngine::SnapshotLease,
                                                    std::function<void(ASFW::AVC::DiscoveryEngine::ExtensionFacts)> done) {
        finish = [done = std::move(done)] { done({}); };
    };
    AvcGoldenRig rig(kDuet, opts);

    bool completed = false;
    rig.Unit()->Initialize([&](bool) { completed = true; });
    rig.Settle();
    // A refresh polls this status: it must not read Completed while the
    // vendor inventory is still on the wire.
    ASSERT_TRUE(finish);
    EXPECT_FALSE(completed);
    EXPECT_EQ(rig.Unit()->GetDiscoveryStatus(), Protocols::AVC::AVCDiscoveryStatus::Running);

    finish();
    rig.Settle();
    EXPECT_TRUE(completed);
    EXPECT_EQ(rig.Unit()->GetDiscoveryStatus(), Protocols::AVC::AVCDiscoveryStatus::Completed);
}

// A successful clock operation must update the published graph, not merely the family's
// appliedClock_. The device readback is supplied only after the settle delay.
TEST(AvcGoldenTests, DuetConfirmedRateUpdatesGraphAndDocumentButFailedReadbackDoesNot) {
    for (const bool confirm : {false, true}) {
        SCOPED_TRACE(confirm);
        AvcGoldenRigOptions opts;
        opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kNone);
        AvcGoldenRig rig(kDuet, opts);
        bool discovered = false;
        rig.Unit()->Initialize([&](bool ok) { discovered = ok; });
        rig.Settle();
        ASSERT_TRUE(discovered);
        const auto before = rig.Unit()->GetDiscoveredGraph();
        ASSERT_NE(before, nullptr);
        const auto originalRate = before->playback.currentSampleRate;
        const auto snapshot = rig.Unit()->GetDiscoverySnapshot();
        const auto query = [&](uint8_t opcode, uint8_t sfc) {
            rig.Sim().SetResponseOverride({0x01, 0xff, opcode, 0x00, 0xff, 0xff, 0xff, 0xff},
                                          {0x0c, 0xff, opcode, 0x00, 0x90, sfc, 0xff, 0xff});
        };
        query(0x19, 0x01); query(0x18, 0x01);
        for (uint8_t opcode : {uint8_t{0x19}, uint8_t{0x18}}) {
            rig.Sim().SetResponseOverride({0x00, 0xff, opcode, 0x00, 0x90, 0x02, 0xff, 0xff},
                                          {0x09, 0xff, opcode, 0x00, 0x90, 0x02, 0xff, 0xff});
            rig.Sim().SetResponseOverride({0x00, 0xff, opcode, 0x00, 0x90, 0x01, 0xff, 0xff},
                                          {0x09, 0xff, opcode, 0x00, 0x90, 0x01, 0xff, 0xff});
        }
        Audio::Oxford::Apogee::ApogeeDuetProtocol protocol(
            rig.Bus(), rig.Bus(), rig.Route(), &rig.Routes(), nullptr, nullptr, 100U, &rig.Timers());
        protocol.UpdateRuntimeContext(rig.Route(), rig.Unit());
        bool completed = false;
        protocol.ApplyClockConfig(Audio::AudioClockConfig{.sampleRateHz = 48000},
            [&](IOReturn status, const Audio::DuplexClockApplyResult&) {
                completed = true;
                EXPECT_EQ(status == kIOReturnSuccess, confirm);
                EXPECT_EQ(rig.Unit()->GetDiscoveredGraph()->playback.currentSampleRate,
                          confirm ? 48000U : originalRate);
            });
        rig.Settle();
        EXPECT_FALSE(completed);
        EXPECT_EQ(rig.Unit()->GetDiscoveredGraph(), before);
        rig.Sim().ClearOverrides();
        query(0x19, 0x02); query(0x18, confirm ? 0x02 : 0x01);
        rig.Timers().Advance(101ULL * 1'000'000ULL);
        rig.Settle();
        EXPECT_TRUE(completed);
        const auto after = rig.Unit()->GetDiscoveredGraph();
        EXPECT_EQ(before->playback.currentSampleRate, originalRate); // old lease stays immutable
        EXPECT_EQ(after->capture.currentSampleRate, confirm ? 48000U : before->capture.currentSampleRate);
        EXPECT_EQ(rig.Unit()->GetDiscoverySnapshot(), snapshot); // captured probes remain intact
        if (confirm) {
            const auto document = UserClient::Wire::BuildAVCDiscoveryDocument(snapshot.get(), after.get(), {});
            EXPECT_NE(document.find("\"rate\":48000"), std::string::npos);
            auto staleRoute = rig.Route();
            ++staleRoute.routeEpoch;
            rig.Unit()->RememberConfirmedDuplexRate(staleRoute, 96000);
            rig.Unit()->RememberConfirmedDuplexRate(rig.Route(), 0);
            EXPECT_EQ(rig.Unit()->GetDiscoveredGraph(), after);
            rig.Sim().ClearOverrides();
            rig.Sim().SetResponseOverride({0x01, 0xff, 0x31}, {0x08, 0xff, 0x31, 0x07, 0xff, 0xff, 0xff, 0xff});
            bool rescanCompleted = false;
            rig.Unit()->Initialize([&](bool ok) { rescanCompleted = true; EXPECT_FALSE(ok); });
            rig.Settle();
            rig.Timers().Advance(2'000'000'000ULL);
            rig.Settle();
            EXPECT_TRUE(rescanCompleted);
            EXPECT_EQ(rig.Unit()->GetDiscoveredGraph(), after);
        }
    }
}

TEST(AvcGoldenTests, VolumeLimitsAreReadPerConfirmedChannelAndRetainedInTheGraph) {
    AvcGoldenRigOptions opts; opts.unitOptions = DiscoveryOptionsFor(AvcExtensionInventory::kNone);
    AvcGoldenRig rig(kDuet, opts);
    rig.Sim().SetDescriptor(0x08, {0x00}, std::vector<uint8_t>(Fixtures::kDuetAudioIdentifierBytes.begin(),
                                                          Fixtures::kDuetAudioIdentifierBytes.end()));
    // Synthetic per-channel limits exercise the discovery logic. These are not
    // new hardware observations or assumptions about the Duet master channel.
    for (uint8_t channel = 0; channel <= 2; ++channel) {
        for (uint8_t attribute : {uint8_t{0x10}, uint8_t{2}, uint8_t{3}, uint8_t{1}}) {
            std::vector<uint8_t> command{1, 8, 0xb8, 0x81, 1, attribute, 2, channel, 2, 2, 0xff, 0xff};
            auto response = command; response[0] = 0x0c;
            response[10] = attribute == 2 ? 0xc0 : attribute == 1 ? 1 : 0;
            response[11] = 0;
            rig.Sim().SetResponseOverride(command, response);
        }
    }
    bool initialized{};
    rig.Unit()->Initialize([&](bool ok) { initialized = ok; }); rig.Settle(); ASSERT_TRUE(initialized);
    const auto graph = rig.Unit()->GetDiscoveredGraph(); ASSERT_TRUE(graph); ASSERT_EQ(graph->featureChannels.size(), 3);
    for (const auto& channel : graph->featureChannels) {
        EXPECT_EQ(channel.volume, 0); EXPECT_EQ(channel.minimum, -16384); EXPECT_EQ(channel.maximum, 0); EXPECT_EQ(channel.resolution, 256);
    }
    const auto snapshot = rig.Unit()->GetDiscoverySnapshot(); ASSERT_TRUE(snapshot);
    EXPECT_EQ(std::ranges::count_if(snapshot->features, [](const auto& status) {
        return status.control == Cmd::FeatureControl::kVolume && status.attribute != Cmd::ControlAttribute::kCurrent;
    }), 9);
    auto changed = Cmd::FeatureReply{.functionBlockId = 1, .channel = 0, .control = Cmd::FeatureControl::kVolume,
                                    .data = {0xff, 0x00}, .dataLength = 2};
    rig.Unit()->RememberConfirmedFeature(rig.Route(), 0, changed);
    EXPECT_TRUE(rig.Unit()->HasUserFeaturePreference(0, 1));
    EXPECT_EQ(rig.Unit()->GetDiscoveredGraph()->featureChannels.front().volume, -256);
    EXPECT_EQ(graph->featureChannels.front().volume, 0); // previous immutable lease remains valid
    auto stale = rig.Route(); ++stale.routeEpoch;
    changed.data = {0xfe, 0x00};
    rig.Unit()->RememberConfirmedFeature(stale, 0, changed);
    EXPECT_EQ(rig.Unit()->GetDiscoveredGraph()->featureChannels.front().volume, -256);
    const auto document = UserClient::Wire::BuildAVCDiscoveryDocument(snapshot.get(), rig.Unit()->GetDiscoveredGraph().get(), {});
    EXPECT_NE(document.find("\"featureChannels\""), std::string::npos);
    EXPECT_NE(document.find("\"minimum\":-16384"), std::string::npos);
}
