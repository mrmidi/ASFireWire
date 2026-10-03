// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiscoveryLogTests.cpp - The naming layer and the discovery log.
//
// The contract under test (Core/AvcNames.hpp): a value a table knows is `name(0xNN)`, a value it does
// not is `UNKNOWN(<table>:0xNN)`. A log that silently dropped or guessed an unnamed value would hide
// exactly the thing the log exists to show, so every table gets one known and one unknown case, and the
// formatter is checked end to end with a descriptor that carries blocks and codes nobody has named.

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Commands/CommandNames.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcNames.hpp"
#include "ASFWDriver/Protocols/AVC/Descriptors/DescriptorNames.hpp"
#include "ASFWDriver/Protocols/AVC/Descriptors/MusicSubunitDescriptor.hpp"
#include "ASFWDriver/Protocols/AVC/Discovery/DiscoveryLog.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ASFW::AVC;
namespace D = ASFW::Protocols::AVC::Descriptors;
namespace DE = ASFW::AVC::DiscoveryEngine;

[[nodiscard]] bool Contains(const std::vector<std::string>& lines, std::string_view needle) {
    return std::ranges::any_of(lines, [needle](const std::string& line) { return line.find(needle) != std::string::npos; });
}

[[nodiscard]] std::vector<uint8_t> InfoBlock(uint16_t type, std::vector<uint8_t> primary, const std::vector<uint8_t>& nested = {}) {
    std::vector<uint8_t> out;
    const size_t compound = 4 + primary.size() + nested.size();
    out.push_back(static_cast<uint8_t>(compound >> 8));
    out.push_back(static_cast<uint8_t>(compound));
    out.push_back(static_cast<uint8_t>(type >> 8));
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(static_cast<uint8_t>(primary.size() >> 8));
    out.push_back(static_cast<uint8_t>(primary.size()));
    out.insert(out.end(), primary.begin(), primary.end());
    out.insert(out.end(), nested.begin(), nested.end());
    return out;
}

[[nodiscard]] std::vector<uint8_t> Descriptor(const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out{static_cast<uint8_t>(body.size() >> 8), static_cast<uint8_t>(body.size())};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// A snapshot holding one music subunit parsed from `descriptor`.
[[nodiscard]] DE::DiscoverySnapshot SnapshotWithMusic(const std::vector<uint8_t>& descriptor) {
    DE::DiscoverySnapshot snapshot;
    snapshot.complete = true;
    DE::SubunitContents contents;
    contents.id = SubunitId{SubunitType::kMusic, 0};
    auto parsed = D::MusicSubunitDescriptorParser::ParseStatusDescriptor(descriptor);
    EXPECT_TRUE(parsed.has_value());
    if (parsed) contents.music = std::move(*parsed);
    snapshot.contents.push_back(std::move(contents));
    return snapshot;
}

} // namespace

// ---------------------------------------------------------------------------
// The contract
// ---------------------------------------------------------------------------

TEST(AvcNamesTests, AKnownValueIsNameAndRawValue) {
    EXPECT_EQ(Describe(CommandType::kStatus), "STATUS(0x1)");
    EXPECT_EQ(Describe(ResponseCode::kImplementedStable), "IMPLEMENTED/STABLE(0xc)");
    EXPECT_EQ(Describe(Opcode::kSignalSource), "SIGNAL SOURCE(0x1a)");
    EXPECT_EQ(Describe(SubunitType::kMusic), "Music(0x0c)");
    EXPECT_EQ(Describe(AvcErrorKind::kRefused), "refused by command filter(0x0d)");
}

TEST(AvcNamesTests, AnUnnamedValueIsFlaggedNeverDroppedOrGuessed) {
    EXPECT_EQ(Describe(static_cast<CommandType>(0x0B)), "UNKNOWN(ctype:0xb)");
    EXPECT_EQ(Describe(static_cast<ResponseCode>(0x01)), "UNKNOWN(response:0x1)");
    EXPECT_EQ(Describe(static_cast<Opcode>(0x77)), "UNKNOWN(opcode:0x77)");
    EXPECT_EQ(Describe(static_cast<SubunitType>(0x0D)), "UNKNOWN(subunit_type:0x0d)");
    EXPECT_EQ(Describe(static_cast<AvcErrorKind>(0xEE)), "UNKNOWN(error:0xee)");
}

TEST(AvcNamesTests, ASubunitAddressNamesTypeInstanceAndByte) {
    EXPECT_EQ(Describe(SubunitAddress::Unit()), "Unit(0x1f) (0xff)");
    EXPECT_EQ(Describe(SubunitAddress::Of(SubunitType::kMusic, 0)), "Music(0x0c)#0 (0x60)");
    EXPECT_EQ(Describe(SubunitAddress::Of(SubunitType::kAudio, 2)), "Audio(0x01)#2 (0x0a)");
}

TEST(AvcNamesTests, NoTableHoldsTwoNamesForOneValue) {
    const auto unique = [](std::span<const NameEntry> table) {
        std::set<uint32_t> seen;
        for (const auto& e : table) {
            if (!seen.insert(e.value).second) return false;
        }
        return true;
    };
    EXPECT_TRUE(unique(names::kCommandTypes));
    EXPECT_TRUE(unique(names::kResponseCodes));
    EXPECT_TRUE(unique(names::kSubunitTypes));
    EXPECT_TRUE(unique(names::kOpcodes));
    EXPECT_TRUE(unique(names::kErrorKinds));
    EXPECT_TRUE(unique(Cmd::names::kAm824Formats));
    EXPECT_TRUE(unique(Cmd::names::kOutputStatuses));
    EXPECT_TRUE(unique(Cmd::names::kFeatureControls));
    EXPECT_TRUE(unique(Cmd::names::kControlAttributes));
    EXPECT_TRUE(unique(D::names::kInfoBlockTypes));
    EXPECT_TRUE(unique(D::names::kMusicPortTypes));
    EXPECT_TRUE(unique(D::names::kMusicPlugUsages));
    EXPECT_TRUE(unique(D::names::kMusicPlugTypes));
}

TEST(AvcNamesTests, TheUnitHoldsTheSixTAWordsForStreamFormatCodes) {
    // TA 2001002 Table 5.5 plus the three codes the draft adds: all must be named.
    for (const uint8_t code : {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x40}) {
        EXPECT_EQ(Describe(static_cast<Cmd::Am824Format>(code)).find("UNKNOWN"), std::string::npos)
            << "AM824 format " << int{code} << " has no name";
    }
    EXPECT_NE(Describe(static_cast<Cmd::Am824Format>(0x55)).find("UNKNOWN(am824_format:0x55)"), std::string::npos);
}

// ---------------------------------------------------------------------------
// CCM
// ---------------------------------------------------------------------------

TEST(CcmNamesTests, TheStatusByteIsDecodedFieldByField) {
    EXPECT_EQ(Cmd::Describe(Cmd::SignalSourceStatusField::FromRaw(0x70)),
              "output_status=ready(0x3) conv=can change format(1) signal_status=identical(0x0)");
    EXPECT_EQ(Cmd::Describe(Cmd::SignalSourceStatusField::Make(0, false, 0x8 | 0x2)),
              "output_status=effective(0x0) conv=fixed format(0) signal_status=processed+converted(0xa)");
}

TEST(CcmNamesTests, AReservedOutputStatusIsFlaggedUnknown) {
    // Table 7.7 defines 0-4; 5-7 are reserved. A device that sends one must be visible.
    EXPECT_EQ(Cmd::Describe(Cmd::SignalSourceStatusField::Make(5, false, 0)),
              "output_status=UNKNOWN(output_status:0x5) conv=fixed format(0) signal_status=identical(0x0)");
}

TEST(CcmNamesTests, ASignalAddressIsNamedByItsRole) {
    using Cmd::SignalAddress;
    using Cmd::SignalRole;
    EXPECT_EQ(Cmd::Describe(SignalAddress::UnitIsochronousPlug(0), SignalRole::kSource), "iPCR[0] [ff 00]");
    EXPECT_EQ(Cmd::Describe(SignalAddress::UnitIsochronousPlug(1), SignalRole::kDestination), "oPCR[1] [ff 01]");
    EXPECT_EQ(Cmd::Describe(SignalAddress::UnitExternalPlug(0), SignalRole::kSource), "external input plug 0 [ff 80]");
    EXPECT_EQ(Cmd::Describe(SignalAddress::UnitExternalPlug(0), SignalRole::kDestination), "external output plug 0 [ff 80]");
    EXPECT_EQ(Cmd::Describe(SignalAddress::NoSignal(), SignalRole::kSource), "no signal source [ff fe]");
    EXPECT_EQ(Cmd::Describe(SignalAddress::SubunitPlug(SubunitAddress::Of(SubunitType::kMusic, 0), 5), SignalRole::kDestination),
              "Music(0x0c)#0 (0x60) destination plug 5 [60 05]");
    EXPECT_EQ(Cmd::Describe(SignalAddress::AnyAvailableSubunitPlug(SubunitAddress::Of(SubunitType::kAudio, 0)), SignalRole::kSource),
              "Audio(0x01)#0 (0x08) source plug any available [08 ff]");
}

TEST(CcmNamesTests, ADeviationFromTheSpecIsWrittenOut) {
    const auto plain = Cmd::StatusDeviations{};
    EXPECT_EQ(Cmd::Describe(plain), "none");
    Cmd::StatusDeviations bad;
    bad.outputStatusNotAllowedForPlug = true;
    bad.convSetOnNonSerialBusPlug = true;
    const auto text = Cmd::Describe(bad);
    EXPECT_NE(text.find("output_status beyond effective/not effective"), std::string::npos);
    EXPECT_NE(text.find("conv set on a plug that is not an oPCR"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Descriptor codes
// ---------------------------------------------------------------------------

TEST(DescriptorNamesTests, KnownAndUnknownInfoBlockTypes) {
    EXPECT_EQ(D::DescribeInfoBlockType(D::kMusicInfoBlockOutputPlugStatusArea), "music output plug status area(0x8101)");
    EXPECT_EQ(D::DescribeInfoBlockType(D::kMusicInfoBlockRoutingStatus), "routing status(0x8108)");
    EXPECT_EQ(D::DescribeInfoBlockType(0x81FE), "UNKNOWN(info_block_type:0x81fe)");
}

TEST(DescriptorNamesTests, KnownAndUnknownMusicPlugCodes) {
    EXPECT_EQ(D::DescribeMusicPortType(D::kMusicPortTypeMidi), "MIDI(0x0a)");
    EXPECT_EQ(D::DescribeMusicPortType(0x42), "UNKNOWN(music_port_type:0x42)");
    EXPECT_EQ(D::DescribeMusicPlugUsage(D::kMusicPlugUsageSync), "sync(0x03)");
    EXPECT_EQ(D::DescribeMusicPlugUsage(0x77), "UNKNOWN(music_plug_usage:0x77)");
    EXPECT_EQ(D::DescribeMusicPlugType(D::kMusicPlugTypeSync), "sync(0x80)");
    EXPECT_EQ(D::DescribeMusicPlugType(0x09), "UNKNOWN(music_plug_type:0x09)");
    EXPECT_EQ(D::DescribeMusicRoutingSupport(D::kMusicRoutingSupportFlexible), "flexible(0x02)");
}

// ---------------------------------------------------------------------------
// The formatter, end to end
// ---------------------------------------------------------------------------

TEST(DiscoveryLogTests, AnUnrecognisedTopLevelBlockIsNamedUnknownAndItsSizeIsLogged) {
    // A general status block, then a block type nobody has defined.
    std::vector<uint8_t> body = InfoBlock(D::kMusicInfoBlockGeneralStatusArea, {0x01, 0x01, 0xFF, 0xFF, 0xFF, 0xFF});
    const auto mystery = InfoBlock(0x81AA, {0x01, 0x02, 0x03});
    body.insert(body.end(), mystery.begin(), mystery.end());

    const auto lines = DE::DescribeDiscovery(SnapshotWithMusic(Descriptor(body)), nullptr);

    EXPECT_TRUE(Contains(lines, "top_level_block[0] general music subunit status area(0x8100)"));
    EXPECT_TRUE(Contains(lines, "top_level_block[1] UNKNOWN(info_block_type:0x81aa) bytes=9 primary=3"));
}

TEST(DiscoveryLogTests, AnUnnamedPlugUsageAndPortTypeAreFlaggedOnTheirLines) {
    const auto name = InfoBlock(D::kInfoBlockName, {0x00, 0x00, 0xFF, 0xFF}, InfoBlock(D::kInfoBlockRawText, {'X'}));
    // cluster: stream format 0x06, port type 0x42 (unnamed), 1 signal; the signal is music plug 0 at position 0.
    const auto cluster = InfoBlock(D::kMusicInfoBlockClusterInfo, {0x06, 0x42, 0x01, 0x00, 0x00, 0x00, 0x00}, name);
    // subunit plug: id 0, signal format 90 02, usage 0x77 (unnamed; primary byte 3), then the cluster.
    const auto plug = InfoBlock(D::kMusicInfoBlockSubunitPlugInfo, {0x00, 0x90, 0x02, 0x77, 0x00, 0x00, 0x00, 0x00}, cluster);
    std::vector<uint8_t> body = InfoBlock(D::kMusicInfoBlockRoutingStatus, {0x01, 0x00}, plug);

    const auto lines = DE::DescribeDiscovery(SnapshotWithMusic(Descriptor(body)), nullptr);

    EXPECT_TRUE(Contains(lines, "usage=UNKNOWN(music_plug_usage:0x77)")) << "usage not flagged";
    EXPECT_TRUE(Contains(lines, "port_type=UNKNOWN(music_port_type:0x42)")) << "port type not flagged";
}

TEST(DiscoveryLogTests, EveryLineCarriesTheTagAndFitsTheRingMessage) {
    // A name far longer than a ring message: it is wrapped into labelled continuation lines, never cut.
    const std::string longName(400, 'n');
    const auto raw = InfoBlock(D::kInfoBlockRawText, std::vector<uint8_t>(longName.begin(), longName.end()));
    const auto name = InfoBlock(D::kInfoBlockName, {0x00, 0x00, 0xFF, 0xFF}, raw);
    const auto plug = InfoBlock(D::kMusicInfoBlockMusicPlugInfo,
                                {0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x00, 0xFF, 0x00, 0x00, 0xF1, 0x00, 0xFF, 0x00, 0x00}, name);
    std::vector<uint8_t> body = InfoBlock(D::kMusicInfoBlockRoutingStatus, {0x01, 0x01}, plug);

    const auto lines = DE::DescribeDiscovery(SnapshotWithMusic(Descriptor(body)), nullptr);
    ASSERT_FALSE(lines.empty());
    for (const auto& line : lines) {
        EXPECT_EQ(line.rfind(DE::kDiscoveryLogTag, 0), 0u) << line;
        EXPECT_LE(line.size(), DE::kMaxDiscoveryLogLine) << line;
    }
    EXPECT_TRUE(Contains(lines, "(cont 2)")) << "a 400-character name must be wrapped";
    size_t carried = 0;
    for (const auto& line : lines) carried += static_cast<size_t>(std::ranges::count(line, 'n'));
    // 'n' also appears in field names; the name itself contributes 400.
    EXPECT_GE(carried, 400u) << "wrapping lost characters of the name";
}

TEST(DiscoveryLogTests, ARouteIsLoggedWithItsDecodedStatusAndDeviations) {
    DE::DiscoverySnapshot snapshot;
    snapshot.complete = true;
    DE::PlugContents plug;
    plug.address = SubunitAddress::Of(SubunitType::kMusic, 0);
    plug.direction = Cmd::PlugDirection::kInput;
    plug.id = DE::PlugId{0};
    Cmd::SignalSource route;
    route.first = Cmd::SignalSourceFirstOperand::FromRaw(0x70);
    route.source = Cmd::SignalAddress::UnitIsochronousPlug(0);
    route.destination = Cmd::SignalAddress::SubunitPlug(plug.address, 0);
    plug.route = route;
    snapshot.plugs.push_back(plug);

    const auto lines = DE::DescribeDiscovery(snapshot, nullptr);

    EXPECT_TRUE(Contains(lines, "route source=iPCR[0] [ff 00] destination=Music(0x0c)#0 (0x60) destination plug 0 [60 00]"));
    EXPECT_TRUE(Contains(lines, "route_status first_operand=0x70 output_status=ready(0x3) conv=can change format(1)"));
    EXPECT_TRUE(Contains(lines, "route_check destination_kind=subunit destination plug deviations=output_status beyond"));
}

TEST(DiscoveryLogTests, AFailedDiscoveryNamesItsTerminalError) {
    DE::DiscoverySnapshot snapshot;
    snapshot.complete = false;
    snapshot.terminalError = AvcError::Of(AvcErrorKind::kTimeout);
    const auto lines = DE::DescribeDiscovery(snapshot, nullptr);
    ASSERT_FALSE(lines.empty());
    EXPECT_NE(lines.front().find("complete=no"), std::string::npos);
    EXPECT_NE(lines.front().find("terminal_error=timeout(0x0a)"), std::string::npos);
}
