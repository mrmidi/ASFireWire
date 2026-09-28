// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcDifferentialTests.cpp - Differential unit tests comparing legacy AV/C encoders
// and decoders against the rebuilt ASFW::AVC codec family (Phase 1, Step 1.3(c)).
//
// Every command built by today's code is compared against the new codecs:
// - UNIT INFO (identifies intended legacy 3-byte vs spec 8-byte difference)
// - SUBUNIT INFO (pages 0..7, entry parsing, intentional enum difference kMusic 0x0C vs 0x1C)
// - PLUG INFO (unit plug counts, response parsing)
// - PLUG SIGNAL FORMAT (query and control across all sampling rates, response parsing)
// - STREAM FORMAT (opcode 0x2F / 0xBF list and single across directions and indices)
// - BridgeCo Extended PLUG INFO (plug type, channel positions, cluster info)
// - Audio Function Block (selector match, feature mute/volume intended spec corrections)
// - Apogee Vendor-Dependent framing
// - M-Audio Special Allowlist Parity

#include <gtest/gtest.h>

// New ASFW::AVC stack
#include "ASFWDriver/Protocols/AVC/Commands/FunctionBlockCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/GeneralCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/SignalSourceCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/StreamFormatCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcError.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcFrame.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcTypes.hpp"
#include "ASFWDriver/Protocols/AVC/Core/RateCodes.hpp"
#include "ASFWDriver/Protocols/AVC/Extensions/BridgeCoPlugInfo.hpp"
#include "ASFWDriver/Protocols/AVC/AVCCommandFilter.hpp"

// Legacy AV/C stack
#include "ASFWDriver/Protocols/AVC/AVCCommands.hpp"
#include "ASFWDriver/Protocols/AVC/AVCUnitPlugInfoCommand.hpp"
#include "ASFWDriver/Protocols/AVC/StreamFormats/AVCUnitPlugSignalFormatCommand.hpp"
#include "ASFWDriver/Protocols/AVC/StreamFormats/AVCStreamFormatCommands.hpp"
#include "ASFWDriver/Protocols/AVC/AVCSignalFormatCommand.hpp"
#include "ASFWDriver/Protocols/AVC/AVCStreamFormatCommand.hpp"
#include "ASFWDriver/Protocols/AVC/AudioFunctionBlockCommand.hpp"
#include "ASFWDriver/Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"
#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.hpp"
#include "AvcTestRig.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ASFW::AVC::Test {

// ===========================================================================
// Test Harness Helpers
// ===========================================================================

class MockAvcSubmitter : public Protocols::AVC::IAVCCommandSubmitter {
public:
    Protocols::AVC::AVCCdb lastCdb{};
    Protocols::AVC::AVCCompletion lastCompletion{};

    void SubmitCommand(const Protocols::AVC::AVCCdb& cdb, Protocols::AVC::AVCCompletion completion) override {
        lastCdb = cdb;
        lastCompletion = std::move(completion);
    }
};

class TestSubunitInfoCommand : public Protocols::AVC::AVCSubunitInfoCommand {
public:
    using Protocols::AVC::AVCSubunitInfoCommand::AVCSubunitInfoCommand;
    const Protocols::AVC::AVCCdb& Cdb() const { return cdb_; }
};

class TestPlugInfoCommand : public Protocols::AVC::AVCPlugInfoCommand {
public:
    using Protocols::AVC::AVCPlugInfoCommand::AVCPlugInfoCommand;
    const Protocols::AVC::AVCCdb& Cdb() const { return cdb_; }
};

class TestSignalFormatCommand : public Protocols::AVC::StreamFormats::AVCUnitPlugSignalFormatCommand {
public:
    using Protocols::AVC::StreamFormats::AVCUnitPlugSignalFormatCommand::AVCUnitPlugSignalFormatCommand;
    const Protocols::AVC::AVCCdb& Cdb() const { return cdb_; }
};

class TestRootOutputPlugSignalFormatCommand : public Protocols::AVC::AVCOutputPlugSignalFormatCommand {
public:
    using Protocols::AVC::AVCOutputPlugSignalFormatCommand::AVCOutputPlugSignalFormatCommand;
    const Protocols::AVC::AVCCdb& Cdb() const { return cdb_; }
};

class TestRootSignalFormatCommand : public Protocols::AVC::AVCSignalFormatCommand {
public:
    using Protocols::AVC::AVCSignalFormatCommand::AVCSignalFormatCommand;
    const Protocols::AVC::AVCCdb& Cdb() const { return cdb_; }
};

class TestRootStreamFormatCommand : public Protocols::AVC::AVCStreamFormatCommand {
public:
    using Protocols::AVC::AVCStreamFormatCommand::AVCStreamFormatCommand;
    const Protocols::AVC::AVCCdb& Cdb() const { return cdb_; }
};

// ===========================================================================
// 1. UNIT INFO Differential Tests
// ===========================================================================

TEST(AvcDifferentialTests, UnitInfo_IdentifiesIntendedDifference) {
    // Legacy AVCUnit::ProbeUnitInfo sent 0 operands: [0x01, 0xFF, 0x30], 3 bytes unpadded.
    Protocols::AVC::AVCCdb legacyCdb{};
    legacyCdb.ctype = static_cast<uint8_t>(Protocols::AVC::AVCCommandType::kStatus);
    legacyCdb.subunit = Protocols::AVC::kAVCSubunitUnit;
    legacyCdb.opcode = static_cast<uint8_t>(Protocols::AVC::AVCOpcode::kUnitInfo);
    legacyCdb.operandLength = 0;
    auto legacyEncoded = legacyCdb.Encode();

    // The rebuilt ASFW::AVC codec sends the standard 5 dummy operands [0x07, FF, FF, FF, FF]
    // per AV/C General Specification 4.2 §10.1 and Linux ta1394 general.rs:43.
    auto newCmd = Cmd::BuildUnitInfoStatus();
    ASSERT_TRUE(newCmd.has_value());

    // Difference: legacy was 3 bytes (padded to 4), new is 8 bytes (padded to 8).
    EXPECT_EQ(legacyEncoded.length, 4U);
    EXPECT_EQ(newCmd->WireBytes().size(), 8U);
    EXPECT_EQ(newCmd->Bytes().size(), 8U);

    // Header matches
    EXPECT_EQ(legacyEncoded.data[0], newCmd->Bytes()[0]); // 0x01 STATUS
    EXPECT_EQ(legacyEncoded.data[1], newCmd->Bytes()[1]); // 0xFF Unit
    EXPECT_EQ(legacyEncoded.data[2], newCmd->Bytes()[2]); // 0x30 Opcode::kUnitInfo

    // Operands differ (intended: legacy omitted operands, new carries spec dummy bytes)
    const std::array<uint8_t, 5> kExpectedOperands = {0x07, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(newCmd->Operands().size(), 5U);
    EXPECT_TRUE(std::equal(kExpectedOperands.begin(), kExpectedOperands.end(), newCmd->Operands().begin()));
}

TEST(AvcDifferentialTests, UnitInfo_ResponseParsing) {
    // Response from Apogee Duet: STABLE, unit, 0x30, 0x07 (dummy), 0x48 (audio 0x09, id 0), OUI 00:03:DB
    const uint8_t respBytes[] = {0x0C, 0xFF, 0x30, 0x07, 0x48, 0x00, 0x03, 0xDB};

    // Note: Legacy AVCUnit::ProbeUnitInfo (AVCUnit.cpp:83) checked IsSuccess(result)
    // but never parsed the operand payload (no UnitInfo decode struct existed).
    // The rebuilt ASFW::AVC stack decodes the full UnitInfo struct per AV/C General Spec 4.2 §10.1.
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());

    auto info = Cmd::ParseUnitInfo(*resp);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->unitType, SubunitType::kPanel); // 0x09 per Duet capture
    EXPECT_EQ(info->unitId, 0);
    EXPECT_EQ(info->companyId, (CompanyId{0x00, 0x03, 0xDB}));
}

// ===========================================================================
// 2. SUBUNIT INFO Differential Tests
// ===========================================================================

TEST(AvcDifferentialTests, SubunitInfo_CommandBytesMatchAcrossPages) {
    auto dummyTransport = reinterpret_cast<Protocols::AVC::FCPTransport*>(0x1000);

    for (uint8_t page = 0; page < 8; ++page) {
        TestSubunitInfoCommand legacyCmd(*dummyTransport, page);
        auto legacyEncoded = legacyCmd.Cdb().Encode();

        auto newCmd = Cmd::BuildSubunitInfoStatus(page, 0x07);
        ASSERT_TRUE(newCmd.has_value());

        ASSERT_EQ(legacyEncoded.length, newCmd->WireBytes().size());
        EXPECT_EQ(legacyEncoded.length, 8U);
        for (size_t i = 0; i < legacyEncoded.length; ++i) {
            EXPECT_EQ(legacyEncoded.data[i], newCmd->WireBytes()[i]) << "Mismatch at byte " << i << " for page " << int(page);
        }
    }
}

TEST(AvcDifferentialTests, SubunitInfo_ResponseParsingAndEnumParity) {
    // Response containing Audio (0x01 << 3 | 0 = 0x08) and Music (0x0C << 3 | 0 = 0x60)
    const uint8_t respBytes[] = {0x0C, 0xFF, 0x31, 0x00, 0x08, 0x60, 0xFF, 0xFF};

    // Real legacy AVCSubunitInfoCommand execution via AvcTestRig:
    ASFW::Testing::AvcTestRig rig;
    rig.Target().Script(ASFW::Testing::AvcReply::RawBytes(
        std::vector<uint8_t>(std::begin(respBytes), std::end(respBytes))));

    auto legacyCmd = std::make_shared<Protocols::AVC::AVCSubunitInfoCommand>(*rig.Transport(), 0);
    Protocols::AVC::AVCSubunitInfoCommand::SubunitInfo legacyInfo{};
    Protocols::AVC::AVCResult legacyResult = Protocols::AVC::AVCResult::kTimeout;

    legacyCmd->Submit([&](Protocols::AVC::AVCResult res, const Protocols::AVC::AVCSubunitInfoCommand::SubunitInfo& info) {
        legacyResult = res;
        legacyInfo = info;
    });
    rig.Drain();

    ASSERT_EQ(legacyResult, Protocols::AVC::AVCResult::kImplementedStable);
    ASSERT_EQ(legacyInfo.subunits.size(), 2U);
    EXPECT_EQ(legacyInfo.subunits[0].type, 0x01); // Audio
    EXPECT_EQ(legacyInfo.subunits[0].maxID, 0);
    EXPECT_EQ(legacyInfo.subunits[1].type, 0x0C); // Music code on wire
    EXPECT_EQ(legacyInfo.subunits[1].maxID, 0);

    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto newParsed = Cmd::ParseSubunitInfo(*resp);
    ASSERT_TRUE(newParsed.has_value());
    ASSERT_EQ(newParsed->entryCount, 2U);
    EXPECT_EQ(newParsed->entries[0].type, SubunitType::kAudio);
    EXPECT_EQ(newParsed->entries[0].maximumId, 0);
    EXPECT_EQ(newParsed->entries[1].type, SubunitType::kMusic); // 0x0C per ta1394
    EXPECT_EQ(newParsed->entries[1].maximumId, 0);

    // Intended difference note: Legacy AVCDefs.hpp defined kMusic = 0x1C (and kMusic0C = 0x0C).
    // The rebuilt codec conforms to ta1394 where SubunitType::kMusic is 0x0C.
    EXPECT_EQ(static_cast<uint8_t>(SubunitType::kMusic), 0x0C);
}

// ===========================================================================
// 3. PLUG INFO (Unit) Differential Tests
// ===========================================================================

TEST(AvcDifferentialTests, UnitPlugInfo_CommandBytesMatch) {
    MockAvcSubmitter submitter;
    Protocols::AVC::AVCUnitPlugInfoCommand legacyCmd(submitter);
    legacyCmd.Submit([](Protocols::AVC::AVCResult, const Protocols::AVC::UnitPlugCounts&) {});
    auto legacyEncoded = submitter.lastCdb.Encode();

    auto newCmd = Cmd::BuildUnitPlugInfoStatus(Cmd::UnitPlugInfoKind::kIsochronousExternal);
    ASSERT_TRUE(newCmd.has_value());

    ASSERT_EQ(legacyEncoded.length, newCmd->WireBytes().size());
    EXPECT_EQ(legacyEncoded.length, 8U);
    for (size_t i = 0; i < legacyEncoded.length; ++i) {
        EXPECT_EQ(legacyEncoded.data[i], newCmd->WireBytes()[i]) << "Mismatch at byte " << i;
    }
}

TEST(AvcDifferentialTests, UnitPlugInfo_ResponseParsing) {
    // Response: STABLE, unit, 0x02, subfunction 0x00, isoIn 2, isoOut 2, extIn 8, extOut 7
    const uint8_t respBytes[] = {0x0C, 0xFF, 0x02, 0x00, 0x02, 0x02, 0x08, 0x07};
    Protocols::AVC::FCPFrame fcpFrame{};
    std::copy(std::begin(respBytes), std::end(respBytes), fcpFrame.data.begin());
    fcpFrame.length = sizeof(respBytes);
    auto legacyCdb = Protocols::AVC::AVCCdb::Decode(fcpFrame);
    ASSERT_TRUE(legacyCdb.has_value());

    MockAvcSubmitter submitter;
    Protocols::AVC::AVCUnitPlugInfoCommand legacyCmd(submitter);
    Protocols::AVC::UnitPlugCounts legacyCounts{};
    legacyCmd.Submit([&](Protocols::AVC::AVCResult, const Protocols::AVC::UnitPlugCounts& c) {
        legacyCounts = c;
    });
    submitter.lastCompletion(Protocols::AVC::AVCResult::kImplementedStable, *legacyCdb);

    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto newPlugs = Cmd::ParseUnitIsochronousExternalPlugs(*resp);
    ASSERT_TRUE(newPlugs.has_value());

    EXPECT_EQ(legacyCounts.isoInputPlugs, newPlugs->isochronousInputs);
    EXPECT_EQ(legacyCounts.isoOutputPlugs, newPlugs->isochronousOutputs);
    EXPECT_EQ(legacyCounts.extInputPlugs, newPlugs->externalInputs);
    EXPECT_EQ(legacyCounts.extOutputPlugs, newPlugs->externalOutputs);
}

// ===========================================================================
// 4. PLUG SIGNAL FORMAT Differential Tests
// ===========================================================================

TEST(AvcDifferentialTests, PlugSignalFormat_StatusQueryBytesMatch) {
    auto dummyTransport = reinterpret_cast<Protocols::AVC::FCPTransport*>(0x1000);

    // Input plug 0
    TestSignalFormatCommand legacyIn0(*dummyTransport, 0, true);
    auto legacyIn0Encoded = legacyIn0.Cdb().Encode();
    auto newIn0 = Cmd::BuildPlugSignalFormatStatus(Cmd::PlugSignalDirection::kInput, 0, Cmd::SignalFormatQuery::kAllWildcard);
    ASSERT_TRUE(newIn0.has_value());
    ASSERT_EQ(legacyIn0Encoded.length, newIn0->WireBytes().size());
    for (size_t i = 0; i < legacyIn0Encoded.length; ++i) {
        EXPECT_EQ(legacyIn0Encoded.data[i], newIn0->WireBytes()[i]);
    }

    // Output plug 0
    TestSignalFormatCommand legacyOut0(*dummyTransport, 0, false);
    auto legacyOut0Encoded = legacyOut0.Cdb().Encode();
    auto newOut0 = Cmd::BuildPlugSignalFormatStatus(Cmd::PlugSignalDirection::kOutput, 0, Cmd::SignalFormatQuery::kAllWildcard);
    ASSERT_TRUE(newOut0.has_value());
    ASSERT_EQ(legacyOut0Encoded.length, newOut0->WireBytes().size());
    for (size_t i = 0; i < legacyOut0Encoded.length; ++i) {
        EXPECT_EQ(legacyOut0Encoded.data[i], newOut0->WireBytes()[i]);
    }
}

TEST(AvcDifferentialTests, PlugSignalFormat_ControlSetBytesMatchAcrossRates) {
    auto dummyTransport = reinterpret_cast<Protocols::AVC::FCPTransport*>(0x1000);

    struct RateEntry {
        Protocols::AVC::StreamFormats::SampleRate legacyRate;
        CipSfc sfc;
    };

    const RateEntry kRates[] = {
        {Protocols::AVC::StreamFormats::SampleRate::k32000Hz, CipSfc::k32000},
        {Protocols::AVC::StreamFormats::SampleRate::k44100Hz, CipSfc::k44100},
        {Protocols::AVC::StreamFormats::SampleRate::k48000Hz, CipSfc::k48000},
        {Protocols::AVC::StreamFormats::SampleRate::k88200Hz, CipSfc::k88200},
        {Protocols::AVC::StreamFormats::SampleRate::k96000Hz, CipSfc::k96000},
        {Protocols::AVC::StreamFormats::SampleRate::k176400Hz, CipSfc::k176400},
        {Protocols::AVC::StreamFormats::SampleRate::k192000Hz, CipSfc::k192000},
    };

    for (const auto& r : kRates) {
        TestSignalFormatCommand legacyCmd(*dummyTransport, 0, true, r.legacyRate);
        auto legacyEncoded = legacyCmd.Cdb().Encode();

        auto newCmd = Cmd::BuildPlugSignalFormatControl(Cmd::PlugSignalDirection::kInput, Cmd::Am824SignalFormat(0, r.sfc));
        ASSERT_TRUE(newCmd.has_value());

        ASSERT_EQ(legacyEncoded.length, newCmd->WireBytes().size());
        for (size_t i = 0; i < legacyEncoded.length; ++i) {
            EXPECT_EQ(legacyEncoded.data[i], newCmd->WireBytes()[i]) << "Mismatch for rate " << int(static_cast<uint8_t>(r.sfc));
        }
    }
}

TEST(AvcDifferentialTests, PlugSignalFormat_ResponseParsing) {
    const uint8_t respBytes[] = {0x0C, 0xFF, 0x19, 0x00, 0x90, 0x02, 0xFF, 0xFF}; // 48 kHz
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());

    auto fmt = Cmd::ParsePlugSignalFormat(*resp, ResponseCode::kImplementedStable);
    ASSERT_TRUE(fmt.has_value());
    EXPECT_EQ(fmt->plugId, 0);
    EXPECT_EQ(fmt->fmt, 0x90);
    auto sfc = Cmd::SfcOf(*fmt);
    ASSERT_TRUE(sfc.has_value());
    EXPECT_EQ(*sfc, CipSfc::k48000);
}

// ===========================================================================
// 4b. Root AVCSignalFormatCommand Differential Tests (AVCUnit.cpp:12 duplicate)
// ===========================================================================

TEST(AvcDifferentialTests, RootSignalFormatCommand_OutputPlugMatches) {
    ASFW::Testing::AvcTestRig rig;

    // 1. Command framing parity
    for (uint8_t plug = 0; plug < 4; ++plug) {
        TestRootOutputPlugSignalFormatCommand legacyCmd(*rig.Transport(), plug);
        auto legacyEncoded = legacyCmd.Cdb().Encode();

        auto newCmd = Cmd::BuildPlugSignalFormatStatus(
            Cmd::PlugSignalDirection::kOutput, plug, Cmd::SignalFormatQuery::kAllWildcard);
        ASSERT_TRUE(newCmd.has_value());

        ASSERT_EQ(legacyEncoded.length, newCmd->WireBytes().size());
        for (size_t i = 0; i < legacyEncoded.length; ++i) {
            EXPECT_EQ(legacyEncoded.data[i], newCmd->WireBytes()[i]) << "Mismatch at byte " << i;
        }
    }

    // 2. Decode parity via real legacy Submit callback execution
    const uint8_t respBytes[] = {0x0C, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF}; // 48 kHz
    rig.Target().Script(ASFW::Testing::AvcReply::RawBytes(
        std::vector<uint8_t>(std::begin(respBytes), std::end(respBytes))));

    auto legacyCmd = std::make_shared<Protocols::AVC::AVCOutputPlugSignalFormatCommand>(*rig.Transport(), 0);
    Protocols::AVC::AVCOutputPlugSignalFormatCommand::SignalFormat legacyFmt{};
    Protocols::AVC::AVCResult legacyResult = Protocols::AVC::AVCResult::kTimeout;

    legacyCmd->Submit([&](Protocols::AVC::AVCResult res, const Protocols::AVC::AVCOutputPlugSignalFormatCommand::SignalFormat& fmt) {
        legacyResult = res;
        legacyFmt = fmt;
    });
    rig.Drain();

    ASSERT_EQ(legacyResult, Protocols::AVC::AVCResult::kImplementedStable);
    EXPECT_EQ(legacyFmt.formatHierarchy, 0x90);
    EXPECT_EQ(legacyFmt.formatSync, 0x02);

    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto newFmt = Cmd::ParsePlugSignalFormat(*resp, ResponseCode::kImplementedStable);
    ASSERT_TRUE(newFmt.has_value());
    EXPECT_EQ(newFmt->fmt, legacyFmt.formatHierarchy);
    EXPECT_EQ(newFmt->fdf[0], legacyFmt.formatSync);
}

TEST(AvcDifferentialTests, RootSignalFormatCommand_MusicSubunitMatches) {
    ASFW::Testing::AvcTestRig rig;

    // Legacy AVCSignalFormatCommand queries Music Subunit signal format (0xA0 input, 0xA1 output)
    TestRootSignalFormatCommand legacyInCmd(*rig.Transport(), 0x08, true, 0);
    EXPECT_EQ(legacyInCmd.Cdb().opcode, 0xA0);
    EXPECT_EQ(legacyInCmd.Cdb().operandLength, 2U);
    EXPECT_EQ(legacyInCmd.Cdb().operands[0], 0xFF);
    EXPECT_EQ(legacyInCmd.Cdb().operands[1], 0xFF);

    TestRootSignalFormatCommand legacyOutCmd(*rig.Transport(), 0x08, false, 0);
    EXPECT_EQ(legacyOutCmd.Cdb().opcode, 0xA1);
    EXPECT_EQ(legacyOutCmd.Cdb().operandLength, 2U);
    EXPECT_EQ(legacyOutCmd.Cdb().operands[0], 0xFF);
    EXPECT_EQ(legacyOutCmd.Cdb().operands[1], 0xFF);
}

// ===========================================================================
// 5. STREAM FORMAT (0x2F / 0xBF) Differential Tests
// ===========================================================================

TEST(AvcDifferentialTests, StreamFormatList_CommandBytesMatchBeBoBDiscovery) {
    const auto directions = {
        Audio::BeBoB::PlugDirection::kInput,
        Audio::BeBoB::PlugDirection::kOutput,
    };

    for (auto dir : directions) {
        for (uint8_t idx = 0; idx < 5; ++idx) {
            auto legacyCdb = Audio::BeBoB::BuildReadOnlyProbeCommand(
                Audio::BeBoB::ReadOnlyProbeCommand::kStreamFormatList, dir, idx);
            auto legacyEncoded = legacyCdb.Encode();

            auto newDir = (dir == Audio::BeBoB::PlugDirection::kInput)
                              ? Cmd::PlugDirection::kInput
                              : Cmd::PlugDirection::kOutput;
            auto newCmd = Cmd::BuildStreamFormatListStatus(
                Cmd::StreamFormatOpcode::kStreamFormatSupport,
                SubunitAddress::Unit(),
                Cmd::PlugAddress::UnitPlug(newDir, Cmd::UnitPlugType::kPcr, 0),
                idx);
            ASSERT_TRUE(newCmd.has_value());

            ASSERT_EQ(legacyEncoded.length, newCmd->WireBytes().size());
            for (size_t i = 0; i < legacyEncoded.length; ++i) {
                EXPECT_EQ(legacyEncoded.data[i], newCmd->WireBytes()[i])
                    << "Mismatch at byte " << i << " for dir " << int(static_cast<uint8_t>(dir)) << " idx " << int(idx);
            }
        }
    }
}

TEST(AvcDifferentialTests, StreamFormatList_ResponseParsingComparison) {
    // Phase 88 list entry 0: AM824 compound, 32 kHz (0x02), 8 MBLA + 2 IEC60958 + 1 MIDI = 10 PCM + 1 MIDI
    const uint8_t respBytes[] = {
        0x0C, 0xFF, 0x2F, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00,
        0x90, 0x40, 0x02, 0x01, 0x03, 0x08, 0x06, 0x02, 0x00, 0x01, 0x0D
    };

    // Legacy parse:
    std::span<const uint8_t> legacyOperands{respBytes + 3, sizeof(respBytes) - 3};
    auto legacyFormation = Audio::BeBoB::ParseExtendedStreamFormatListResponse(0, legacyOperands);
    ASSERT_TRUE(legacyFormation.has_value());
    EXPECT_EQ(legacyFormation->rateCode, 0x02);
    EXPECT_EQ(legacyFormation->pcmChannels, 10);
    EXPECT_EQ(legacyFormation->midiSlots, 1);

    // New parse:
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto newEntry = Cmd::ParseStreamFormatList(*resp, 0);
    ASSERT_TRUE(newEntry.has_value());
    EXPECT_EQ(newEntry->index, 0);
    EXPECT_EQ(newEntry->format.kind, Cmd::StreamFormat::Kind::kCompoundAm824);
    EXPECT_EQ(newEntry->format.compound.rate, StreamFormatRate::k32000);
    EXPECT_EQ(newEntry->format.compound.PcmChannels(), 10U);
    EXPECT_EQ(newEntry->format.compound.MidiChannels(), 1U);
}

// ===========================================================================
// 5b. Root AVCStreamFormatCommand Differential Tests (OxfwStreamFormats.cpp:9 duplicate)
// ===========================================================================

TEST(AvcDifferentialTests, RootStreamFormatCommand_CurrentFormatMatches) {
    auto dummyTransport = reinterpret_cast<Protocols::AVC::FCPTransport*>(0x1000);

    const bool dirs[] = {true, false}; // isInput
    const bool opcodes[] = {false, true}; // false = 0xBF, true = 0x2F

    for (bool isInput : dirs) {
        for (bool useAlt : opcodes) {
            TestRootStreamFormatCommand legacyCmd(*dummyTransport, 0xFF, 0, isInput, useAlt);
            auto legacyEncoded = legacyCmd.Cdb().Encode();

            auto newDir = isInput ? Cmd::PlugDirection::kInput : Cmd::PlugDirection::kOutput;
            auto newOpcode = useAlt ? Cmd::StreamFormatOpcode::kStreamFormatSupport
                                    : Cmd::StreamFormatOpcode::kExtendedStreamFormat;
            auto newCmd = Cmd::BuildStreamFormatSingleStatus(
                newOpcode, SubunitAddress::Unit(), Cmd::PlugAddress::UnitPlug(newDir, Cmd::UnitPlugType::kPcr, 0));
            ASSERT_TRUE(newCmd.has_value());

            ASSERT_EQ(legacyEncoded.length, newCmd->WireBytes().size());
            // Bytes 0..8 match identically (header + subfunc + plug address)
            for (size_t i = 0; i < 9; ++i) {
                EXPECT_EQ(legacyEncoded.data[i], newCmd->WireBytes()[i])
                    << "Mismatch at byte " << i << " isInput=" << isInput << " useAlt=" << useAlt;
            }
            // Byte 9: Intended difference!
            // Legacy wrote 6 operands and zero-padded byte 9; new sends SupportStatus::kNotUsed (0xFF)
            // per TA 2001002 §8.1.1 (and matches Linux ta1394).
            EXPECT_EQ(legacyEncoded.data[9], 0x00);
            EXPECT_EQ(newCmd->WireBytes()[9], 0xFF);
            // Bytes 10..11 are quadlet zero padding in both
            EXPECT_EQ(legacyEncoded.data[10], 0x00);
            EXPECT_EQ(newCmd->WireBytes()[10], 0x00);
            EXPECT_EQ(legacyEncoded.data[11], 0x00);
            EXPECT_EQ(newCmd->WireBytes()[11], 0x00);
        }
    }
}

TEST(AvcDifferentialTests, RootStreamFormatCommand_SupportedListMatches) {
    auto dummyTransport = reinterpret_cast<Protocols::AVC::FCPTransport*>(0x1000);

    const bool dirs[] = {true, false}; // isInput
    const bool opcodes[] = {false, true}; // false = 0xBF, true = 0x2F

    for (bool isInput : dirs) {
        for (bool useAlt : opcodes) {
            for (uint8_t idx = 0; idx < 5; ++idx) {
                TestRootStreamFormatCommand legacyCmd(*dummyTransport, 0xFF, 0, isInput, idx, useAlt);
                auto legacyEncoded = legacyCmd.Cdb().Encode();

                auto newDir = isInput ? Cmd::PlugDirection::kInput : Cmd::PlugDirection::kOutput;
                auto newOpcode = useAlt ? Cmd::StreamFormatOpcode::kStreamFormatSupport
                                        : Cmd::StreamFormatOpcode::kExtendedStreamFormat;
                auto newCmd = Cmd::BuildStreamFormatListStatus(
                    newOpcode, SubunitAddress::Unit(), Cmd::PlugAddress::UnitPlug(newDir, Cmd::UnitPlugType::kPcr, 0), idx);
                ASSERT_TRUE(newCmd.has_value());

                ASSERT_EQ(legacyEncoded.length, newCmd->WireBytes().size());
                for (size_t i = 0; i < legacyEncoded.length; ++i) {
                    EXPECT_EQ(legacyEncoded.data[i], newCmd->WireBytes()[i])
                        << "Mismatch at byte " << i << " idx=" << int(idx) << " isInput=" << isInput << " useAlt=" << useAlt;
                }
            }
        }
    }
}

TEST(AvcDifferentialTests, RootStreamFormatCommand_ResponseParsingMatches) {
    ASFW::Testing::AvcTestRig rig;

    // Duet 0xBF single response (48 kHz, 2 PCM)
    const uint8_t duetSingleResp[] = {
        0x0C, 0xFF, 0xBF, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00,
        0x90, 0x40, 0x04, 0x00, 0x01, 0x02, 0x06
    };

    rig.Target().Script(ASFW::Testing::AvcReply::RawBytes(
        std::vector<uint8_t>(std::begin(duetSingleResp), std::end(duetSingleResp))));
    auto legacySingleCmd = std::make_shared<Protocols::AVC::AVCStreamFormatCommand>(*rig.Transport(), 0xFF, 0, true, false);
    std::optional<Protocols::AVC::StreamFormat> legacyFmt;
    Protocols::AVC::AVCResult legacyResult = Protocols::AVC::AVCResult::kTimeout;

    legacySingleCmd->Submit([&](Protocols::AVC::AVCResult res, const std::optional<Protocols::AVC::StreamFormat>& fmt) {
        legacyResult = res;
        legacyFmt = fmt;
    });
    rig.Drain();

    ASSERT_EQ(legacyResult, Protocols::AVC::AVCResult::kImplementedStable);
    ASSERT_TRUE(legacyFmt.has_value());
    EXPECT_EQ(legacyFmt->formatType, 0x90);
    EXPECT_EQ(legacyFmt->formatSubtype, 0x40);
    EXPECT_EQ(legacyFmt->sampleRate, 0x04); // 48 kHz in TA 2001002 table
    EXPECT_EQ(legacyFmt->numChannels, 1);   // Legacy stores infoCount (1 format info pair)

    auto newResp = ParseResponse(duetSingleResp);
    ASSERT_TRUE(newResp.has_value());
    auto newParsed = Cmd::ParseStreamFormatSingle(*newResp, ResponseCode::kImplementedStable);
    ASSERT_TRUE(newParsed.has_value());
    EXPECT_EQ(newParsed->format.kind, Cmd::StreamFormat::Kind::kCompoundAm824);
    EXPECT_EQ(newParsed->format.compound.rate, StreamFormatRate::k48000);
    EXPECT_EQ(newParsed->format.compound.entryCount, 1U);
    EXPECT_EQ(newParsed->format.compound.PcmChannels(), 2U); // New computes actual PCM channels
}

// ===========================================================================
// 6. BridgeCo Extended PLUG INFO (0xC0) Differential Tests
// ===========================================================================

TEST(AvcDifferentialTests, BridgeCoExtendedPlugInfo_CommandBytesMatchBeBoBDiscovery) {
    const auto directions = {
        Audio::BeBoB::PlugDirection::kInput,
        Audio::BeBoB::PlugDirection::kOutput,
    };

    for (auto dir : directions) {
        auto newDir = (dir == Audio::BeBoB::PlugDirection::kInput)
                          ? Cmd::PlugDirection::kInput
                          : Cmd::PlugDirection::kOutput;
        auto plugAddr = Cmd::PlugAddress::UnitPlug(newDir, Cmd::UnitPlugType::kPcr, 0);

        // 1. Plug Type
        auto legPt = Audio::BeBoB::BuildReadOnlyProbeCommand(
            Audio::BeBoB::ReadOnlyProbeCommand::kIsochPlugType, dir, 0);
        auto legPtEnc = legPt.Encode();
        auto newPt = BridgeCo::BuildExtendedPlugInfoStatus(SubunitAddress::Unit(), plugAddr, BridgeCo::InfoType::kPlugType);
        ASSERT_TRUE(newPt.has_value());
        ASSERT_EQ(legPtEnc.length, newPt->WireBytes().size());
        for (size_t i = 0; i < legPtEnc.length; ++i) {
            EXPECT_EQ(legPtEnc.data[i], newPt->WireBytes()[i]);
        }

        // 2. Channel Positions
        auto legCp = Audio::BeBoB::BuildReadOnlyProbeCommand(
            Audio::BeBoB::ReadOnlyProbeCommand::kChannelPositions, dir, 0);
        auto legCpEnc = legCp.Encode();
        auto newCp = BridgeCo::BuildExtendedPlugInfoStatus(SubunitAddress::Unit(), plugAddr, BridgeCo::InfoType::kChannelPositions);
        ASSERT_TRUE(newCp.has_value());
        ASSERT_EQ(legCpEnc.length, newCp->WireBytes().size());
        for (size_t i = 0; i < legCpEnc.length; ++i) {
            EXPECT_EQ(legCpEnc.data[i], newCp->WireBytes()[i]);
        }

        // 3. Cluster / Section Info
        for (uint8_t sec = 0; sec < 3; ++sec) {
            auto legSec = Audio::BeBoB::BuildReadOnlyProbeCommand(
                Audio::BeBoB::ReadOnlyProbeCommand::kSectionType, dir, sec);
            auto legSecEnc = legSec.Encode();
            auto newSec = BridgeCo::BuildExtendedPlugInfoStatus(
                SubunitAddress::Unit(), plugAddr, BridgeCo::InfoType::kClusterInfo, static_cast<uint8_t>(sec + 1));
            ASSERT_TRUE(newSec.has_value());
            ASSERT_EQ(legSecEnc.length, newSec->WireBytes().size());
            for (size_t i = 0; i < legSecEnc.length; ++i) {
                EXPECT_EQ(legSecEnc.data[i], newSec->WireBytes()[i]);
            }
        }
    }
}

// ===========================================================================
// 6b. BridgeCo Channel Positions Trailing-Bytes Strictness Parity
// ===========================================================================

TEST(AvcDifferentialTests, BridgeCoExtendedPlugInfo_ChannelPositionsStrictnessDifference) {
    // Valid channel position payload: 1 section, 2 channels
    // Format: [section_count: 1][position_count: 2][stream_pos: 1, sec_loc: 1][stream_pos: 2, sec_loc: 2]
    const std::vector<uint8_t> exactPayload = {0x01, 0x02, 0x01, 0x01, 0x02, 0x02};

    // 1. Both legacy and new accept exact payload
    auto legacyExact = Audio::BeBoB::ParseChannelPositionSections(exactPayload);
    ASSERT_TRUE(legacyExact.has_value());
    ASSERT_EQ(legacyExact->size(), 1U);
    EXPECT_EQ((*legacyExact)[0].positions.size(), 2U);

    // Build FCP response with exact payload (operands 0..6 header + extra, payload starts at operand 7)
    // Response frame: [0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x03] + exactPayload
    std::vector<uint8_t> exactFrame = {0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x03};
    exactFrame.insert(exactFrame.end(), exactPayload.begin(), exactPayload.end());

    auto respExact = ParseResponse(exactFrame);
    ASSERT_TRUE(respExact.has_value());
    auto newExact = BridgeCo::ParseChannelPositions(*respExact);
    ASSERT_TRUE(newExact.has_value());
    EXPECT_EQ(newExact->sectionCount, 1U);
    EXPECT_EQ(newExact->sections[0].positionCount, 2U);
    EXPECT_EQ(newExact->sections[0].positions[0].streamPosition, 0); // 0-based
    EXPECT_EQ(newExact->sections[0].positions[0].sectionLocation, 0);

    // 2. Trailing padding bytes behavior (e.g. 2 zero quadlet-padding bytes appended)
    std::vector<uint8_t> paddedPayload = exactPayload;
    paddedPayload.push_back(0x00);
    paddedPayload.push_back(0x00);

    // Legacy strictly enforces cursor == payload.size() and rejects trailing bytes:
    auto legacyPadded = Audio::BeBoB::ParseChannelPositionSections(paddedPayload);
    EXPECT_FALSE(legacyPadded.has_value())
        << "Legacy ParseChannelPositionSections strictly fails when trailing bytes are present";

    // New codec ignores trailing quadlet padding bytes per specification:
    std::vector<uint8_t> paddedFrame = {0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x03};
    paddedFrame.insert(paddedFrame.end(), paddedPayload.begin(), paddedPayload.end());

    auto respPadded = ParseResponse(paddedFrame);
    ASSERT_TRUE(respPadded.has_value());
    auto newPadded = BridgeCo::ParseChannelPositions(*respPadded);
    ASSERT_TRUE(newPadded.has_value())
        << "New BridgeCo::ParseChannelPositions safely accepts trailing quadlet padding bytes";
    EXPECT_EQ(newPadded->sectionCount, 1U);
    EXPECT_EQ(newPadded->sections[0].positionCount, 2U);
}

// ===========================================================================
// 7. Audio Function Block Differential Tests
// ===========================================================================

TEST(AvcDifferentialTests, AudioFunctionBlock_SelectorCommandBytesMatch) {
    MockAvcSubmitter submitter;

    for (uint8_t fbId = 1; fbId <= 4; ++fbId) {
        for (uint8_t inputPlug = 0; inputPlug < 2; ++inputPlug) {
            Protocols::AVC::AudioFunctionBlockCommand legacyCmd(
                submitter,
                0x08, // Audio subunit 0
                Protocols::AVC::AudioFunctionBlockCommand::CommandType::kControl,
                Protocols::AVC::AudioFunctionBlockCommand::BlockType::kSelector,
                fbId,
                Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kSelectorControl,
                std::vector<uint8_t>{inputPlug});
            legacyCmd.Submit([](Protocols::AVC::AVCResult, const std::vector<uint8_t>&) {});
            auto legacyEncoded = submitter.lastCdb.Encode();

            auto newCmd = Cmd::BuildSelectorControl(kAudioSubunit0, fbId, inputPlug);
            ASSERT_TRUE(newCmd.has_value());

            ASSERT_EQ(legacyEncoded.length, newCmd->WireBytes().size());
            for (size_t i = 0; i < legacyEncoded.length; ++i) {
                EXPECT_EQ(legacyEncoded.data[i], newCmd->WireBytes()[i])
                    << "Mismatch at byte " << i << " for fbId " << int(fbId);
            }
        }
    }
}

TEST(AvcDifferentialTests, AudioFunctionBlock_FeatureMuteAndVolume_IdentifiesIntendedDifference) {
    MockAvcSubmitter submitter;

    // Legacy BeBoBProtocol::SetFeatureMute:
    // sent selector length 4, data {channel, 0x01, unmute ? 0x60 : 0x00}
    Protocols::AVC::AudioFunctionBlockCommand legacyMute(
        submitter, 0x08,
        Protocols::AVC::AudioFunctionBlockCommand::CommandType::kControl,
        Protocols::AVC::AudioFunctionBlockCommand::BlockType::kFeature,
        1,
        Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kMute,
        std::vector<uint8_t>{0x00, 0x01, 0x60});
    legacyMute.Submit([](Protocols::AVC::AVCResult, const std::vector<uint8_t>&) {});
    auto legacyMuteEncoded = submitter.lastCdb.Encode();

    // Rebuilt codec per TA 1394 Audio Subunit 1.0 §10.3 / §10.3.1:
    // selector length is ALWAYS 2: [channel][control selector]
    // followed by [data length 1][0x60/0x70]
    //
    // HARDWARE PROOF ON TERRAMAC PHASE 88 (2026-09-27):
    // Phase 88 ACCEPTED both the spec form (selector length 02, FB1 ch1/ch2 volume)
    // and the old driver's form (length 05), and read back the value.
    // The new form is now hardware-proven on the Phase 88, not just spec-correct.
    auto newMute = Cmd::BuildFeatureMuteControl(kAudioSubunit0, 1, 0x00, false);
    ASSERT_TRUE(newMute.has_value());

    // Both commands have the same total wire length (12 bytes padded)
    EXPECT_EQ(legacyMuteEncoded.length, 12U);
    EXPECT_EQ(newMute->WireBytes().size(), 12U);

    // Header matches: 00 08 B8 (Control, Audio Subunit 0, Function Block 0xB8)
    EXPECT_EQ(legacyMuteEncoded.data[0], newMute->Bytes()[0]);
    EXPECT_EQ(legacyMuteEncoded.data[1], newMute->Bytes()[1]);
    EXPECT_EQ(legacyMuteEncoded.data[2], newMute->Bytes()[2]);
    EXPECT_EQ(legacyMuteEncoded.data[3], newMute->Bytes()[3]); // FB Type: 0x81 (Feature)
    EXPECT_EQ(legacyMuteEncoded.data[4], newMute->Bytes()[4]); // FB ID: 1
    EXPECT_EQ(legacyMuteEncoded.data[5], newMute->Bytes()[5]); // Attr: 0x10 (Current)

    // Intended difference at operand 3 (frame byte 6):
    // Legacy sent selector length 4 (invalid per spec §10.3); new sends 2.
    EXPECT_EQ(legacyMuteEncoded.data[6], 0x04);
    EXPECT_EQ(newMute->Bytes()[6], 0x02);

    // Legacy BeBoBProtocol::SetFeatureVolume:
    // sent selector length 5, data {channel, 0x02, hi, lo}
    Protocols::AVC::AudioFunctionBlockCommand legacyVol(
        submitter, 0x08,
        Protocols::AVC::AudioFunctionBlockCommand::CommandType::kControl,
        Protocols::AVC::AudioFunctionBlockCommand::BlockType::kFeature,
        1,
        Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kVolume,
        std::vector<uint8_t>{0x00, 0x02, 0x00, 0x00});
    legacyVol.Submit([](Protocols::AVC::AVCResult, const std::vector<uint8_t>&) {});
    auto legacyVolEncoded = submitter.lastCdb.Encode();

    auto newVol = Cmd::BuildFeatureVolumeControl(kAudioSubunit0, 1, 0x00, 0x0000);
    ASSERT_TRUE(newVol.has_value());

    // Intended difference at operand 3: legacy sent 5; new sends 2 per spec §10.3.
    EXPECT_EQ(legacyVolEncoded.data[6], 0x05);
    EXPECT_EQ(newVol->Bytes()[6], 0x02);
}

// ===========================================================================
// 8. Apogee Vendor-Dependent Differential Tests
// ===========================================================================

TEST(AvcDifferentialTests, ApogeeVendorDependent_CommandBytesMatch) {
    // Apogee OutMute query via ApogeeVendorCodec:
    // OUI 00 03 DB, Magic 'P','C','M' (50 43 4D), Code OutMute (0x09), Args FF FF
    auto legacyVendorCmd = Audio::Oxford::Apogee::ApogeeVendorCommand::Bool(
        Audio::Oxford::Apogee::ApogeeVendorCommand::Code::OutMute, false);
    auto baseOperands = legacyVendorCmd.BuildOperandBase();
    ASSERT_EQ(baseOperands.size(), 9U);

    // Frame payload: OUI (3) + Magic (3) + Code (1) + Args (2) = 9 bytes
    // Rebuilt BuildVendorDependent:
    // [01][FF][00][OUI 3][Vendor Data 6]
    // Where Vendor Data is Magic (3) + Code (1) + Args (2)
    std::span<const uint8_t> vendorPayload{baseOperands.data() + 3, baseOperands.size() - 3};
    auto newVendorCmd = Cmd::BuildVendorDependent(
        CommandType::kStatus,
        SubunitAddress::Unit(),
        CompanyId{0x00, 0x03, 0xDB},
        vendorPayload);
    ASSERT_TRUE(newVendorCmd.has_value());

    // Unpadded size: 3 header + 3 OUI + 6 vendor data = 12 bytes
    ASSERT_EQ(newVendorCmd->Bytes().size(), 12U);
    EXPECT_EQ(newVendorCmd->Bytes()[0], 0x01); // STATUS
    EXPECT_EQ(newVendorCmd->Bytes()[1], 0xFF); // Unit
    EXPECT_EQ(newVendorCmd->Bytes()[2], 0x00); // VENDOR DEPENDENT
    EXPECT_EQ(newVendorCmd->Bytes()[3], 0x00); // OUI
    EXPECT_EQ(newVendorCmd->Bytes()[4], 0x03);
    EXPECT_EQ(newVendorCmd->Bytes()[5], 0xDB);
    EXPECT_EQ(newVendorCmd->Bytes()[6], 'P');
    EXPECT_EQ(newVendorCmd->Bytes()[7], 'C');
    EXPECT_EQ(newVendorCmd->Bytes()[8], 'M');
    EXPECT_EQ(newVendorCmd->Bytes()[9], 0x09); // OutMute
    EXPECT_EQ(newVendorCmd->Bytes()[10], 0x80); // kApogeeArgIndexed
    EXPECT_EQ(newVendorCmd->Bytes()[11], 0xFF); // kApogeeArgDefault
}

// ===========================================================================
// 9. M-Audio Special Allowlist Differential / Parity Tests
// ===========================================================================

TEST(AvcDifferentialTests, MAudioSpecialPermittedFrames_MatchesAllowlistContract) {
    const auto table = Protocols::AVC::PermittedFramesFor(Discovery::AvcCommandFilterId::MAudioSpecialBeBoB);
    ASSERT_EQ(table.size(), 7U);

    // 1. sig-fmt STATUS input plug
    const uint8_t inSigFmtStatus[] = {0x01, 0xFF, 0x19, 0x00, 0x90, 0x00, 0x00, 0x00};
    EXPECT_TRUE(Protocols::AVC::FrameIsPermitted(table, inSigFmtStatus));

    // 2. sig-fmt STATUS output plug
    const uint8_t outSigFmtStatus[] = {0x01, 0xFF, 0x18, 0x00, 0x90, 0x00, 0x00, 0x00};
    EXPECT_TRUE(Protocols::AVC::FrameIsPermitted(table, outSigFmtStatus));

    // 3. sig-fmt CONTROL input plug
    const uint8_t inSigFmtCtrl[] = {0x00, 0xFF, 0x19, 0x00, 0x90, 0x02, 0x00, 0x00};
    EXPECT_TRUE(Protocols::AVC::FrameIsPermitted(table, inSigFmtCtrl));

    // 4. sig-fmt CONTROL output plug
    const uint8_t outSigFmtCtrl[] = {0x00, 0xFF, 0x18, 0x00, 0x90, 0x02, 0x00, 0x00};
    EXPECT_TRUE(Protocols::AVC::FrameIsPermitted(table, outSigFmtCtrl));

    // 5. M-Audio vendor clock/format (16 bytes, lock = 0x00)
    const uint8_t clkCmd[] = {
        0x00, 0xFF, 0x00, 0x04, 0x00, 0x04, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    EXPECT_TRUE(Protocols::AVC::FrameIsPermitted(table, clkCmd));

    // 6. M-Audio blank-slate input selector (12 bytes)
    const uint8_t selCmd[] = {
        0x00, 0x08, 0xB8, 0x80, 0x04, 0x10, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00
    };
    EXPECT_TRUE(Protocols::AVC::FrameIsPermitted(table, selCmd));

    // 7. M-Audio LED (8 bytes)
    const uint8_t ledCmd[] = {0x00, 0xFF, 0x00, 0x03, 0x00, 0x01, 0x01, 0x00};
    EXPECT_TRUE(Protocols::AVC::FrameIsPermitted(table, ledCmd));

    // Ensure hazardous frames are blocked:
    // SUBUNIT_INFO (0x31)
    const uint8_t subInfo[] = {0x01, 0xFF, 0x31, 0x07, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(Protocols::AVC::FrameIsPermitted(table, subInfo));

    // PLUG_INFO (0x02)
    const uint8_t plugInfo[] = {0x01, 0xFF, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(Protocols::AVC::FrameIsPermitted(table, plugInfo));

    // Extended stream format (0x2F / 0xBF)
    const uint8_t extFmt[] = {0x01, 0xFF, 0x2F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF};
    EXPECT_FALSE(Protocols::AVC::FrameIsPermitted(table, extFmt));

    // UNIT_INFO (0x30)
    const uint8_t unitInfo[] = {0x01, 0xFF, 0x30, 0x07, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(Protocols::AVC::FrameIsPermitted(table, unitInfo));
}

} // namespace ASFW::AVC::Test
