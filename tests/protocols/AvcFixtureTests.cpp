// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcFixtureTests.cpp - Tests validating the new AV/C codecs against real
// hardware captures recorded in tests/support/AvcDeviceImages.inc.

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Commands/FunctionBlockCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/GeneralCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/SignalSourceCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/StreamFormatCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcError.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcFrame.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcTypes.hpp"
#include "ASFWDriver/Protocols/AVC/Extensions/BridgeCoPlugInfo.hpp"

#include "../support/AvcDeviceImages.inc"

#include <cstring>
#include <string_view>

namespace ASFW::AVC::Test {

// ===========================================================================
// Phase 88 Hardware Fixture Tests
// ===========================================================================

TEST(AvcFixtureTests, Phase88AllRecordsParseConsistently) {
    const auto& device = Testing::kPhase88;
    EXPECT_STREQ(device.key, "phase88");
    EXPECT_EQ(device.guid, 0x000AAC0300B1D1F7ULL);
    EXPECT_GT(device.records.size(), 300U);

    size_t stableCount = 0;
    size_t notImplementedCount = 0;
    size_t rejectedCount = 0;

    for (const auto& rec : device.records) {
        ASSERT_TRUE(rec.ok) << "Failed exchange: " << rec.name;
        ASSERT_GE(rec.response.size(), 3U) << "Response too short: " << rec.name;

        auto respRes = ParseResponse(rec.response);
        ASSERT_TRUE(respRes.has_value()) << "ParseResponse failed for: " << rec.name;
        const auto& resp = *respRes;

        if (rec.responseCode == static_cast<uint8_t>(ResponseCode::kImplementedStable)) {
            stableCount++;
            EXPECT_EQ(resp.code, ResponseCode::kImplementedStable) << "Record: " << rec.name;
            auto ops = OperandsIf(resp, ResponseCode::kImplementedStable);
            EXPECT_TRUE(ops.has_value()) << "OperandsIf failed on stable record: " << rec.name;
        } else if (rec.responseCode == static_cast<uint8_t>(ResponseCode::kNotImplemented)) {
            notImplementedCount++;
            EXPECT_EQ(resp.code, ResponseCode::kNotImplemented) << "Record: " << rec.name;
            auto ops = OperandsIf(resp, ResponseCode::kImplementedStable);
            ASSERT_FALSE(ops.has_value()) << "OperandsIf should fail on NOT_IMPLEMENTED: " << rec.name;
            EXPECT_EQ(ops.error().kind, AvcErrorKind::kUnexpectedResponse);
            EXPECT_EQ(ops.error().response, ResponseCode::kNotImplemented);
        } else if (rec.responseCode == static_cast<uint8_t>(ResponseCode::kRejected)) {
            rejectedCount++;
            EXPECT_EQ(resp.code, ResponseCode::kRejected) << "Record: " << rec.name;
            auto ops = OperandsIf(resp, ResponseCode::kImplementedStable);
            ASSERT_FALSE(ops.has_value()) << "OperandsIf should fail on REJECTED: " << rec.name;
            EXPECT_EQ(ops.error().kind, AvcErrorKind::kUnexpectedResponse);
            EXPECT_EQ(ops.error().response, ResponseCode::kRejected);
        }
    }

    EXPECT_GT(stableCount, 100U);
    EXPECT_GT(notImplementedCount, 10U);
}

TEST(AvcFixtureTests, Phase88UnitInfoParsesCorrectly) {
    auto resp = ParseResponse(Testing::Phase88Data::kResp_0_unit_info);
    ASSERT_TRUE(resp.has_value());

    auto info = Cmd::ParseUnitInfo(*resp);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->unitType, SubunitType::kAudio);
    EXPECT_EQ(info->unitId, 0x07);
    EXPECT_EQ(info->companyId[0], 0x00);
    EXPECT_EQ(info->companyId[1], 0x0A);
    EXPECT_EQ(info->companyId[2], 0xAC);
}

TEST(AvcFixtureTests, Phase88SubunitInfoParsesCorrectly) {
    auto resp0 = ParseResponse(Testing::Phase88Data::kResp_1_subunit_info_page_0);
    ASSERT_TRUE(resp0.has_value());
    auto sub0 = Cmd::ParseSubunitInfo(*resp0);
    ASSERT_TRUE(sub0.has_value());
    EXPECT_EQ(sub0->page, 0);
    EXPECT_EQ(sub0->entryCount, 2U);
    EXPECT_EQ(sub0->entries[0].type, SubunitType::kAudio);
    EXPECT_EQ(sub0->entries[0].maximumId, 0);
    EXPECT_EQ(sub0->entries[1].type, SubunitType::kMusic);
    EXPECT_EQ(sub0->entries[1].maximumId, 0);

    auto resp1 = ParseResponse(Testing::Phase88Data::kResp_2_subunit_info_page_1);
    ASSERT_TRUE(resp1.has_value());
    EXPECT_EQ(resp1->code, ResponseCode::kNotImplemented);
    auto sub1 = Cmd::ParseSubunitInfo(*resp1);
    ASSERT_FALSE(sub1.has_value());
    EXPECT_EQ(sub1.error().kind, AvcErrorKind::kUnexpectedResponse);
    EXPECT_EQ(sub1.error().response, ResponseCode::kNotImplemented);
}

TEST(AvcFixtureTests, Phase88PlugInfoParsesCorrectly) {
    auto respUnit0 = ParseResponse(Testing::Phase88Data::kResp_3_plug_info_unit_00);
    ASSERT_TRUE(respUnit0.has_value());
    auto plugsIsoExt = Cmd::ParseUnitIsochronousExternalPlugs(*respUnit0);
    ASSERT_TRUE(plugsIsoExt.has_value());
    EXPECT_EQ(plugsIsoExt->isochronousInputs, 2);
    EXPECT_EQ(plugsIsoExt->isochronousOutputs, 2);
    EXPECT_EQ(plugsIsoExt->externalInputs, 8);
    EXPECT_EQ(plugsIsoExt->externalOutputs, 7);

    auto respUnit1 = ParseResponse(Testing::Phase88Data::kResp_4_plug_info_unit_01);
    ASSERT_TRUE(respUnit1.has_value());
    auto plugsAsync = Cmd::ParseUnitAsynchronousPlugs(*respUnit1);
    ASSERT_TRUE(plugsAsync.has_value());
    EXPECT_EQ(plugsAsync->asynchronousInputs, 0);
    EXPECT_EQ(plugsAsync->asynchronousOutputs, 0);

    auto respAudio = ParseResponse(Testing::Phase88Data::kResp_5_plug_info_audio_0);
    ASSERT_TRUE(respAudio.has_value());
    auto audioPlugs = Cmd::ParseSubunitPlugs(*respAudio);
    ASSERT_TRUE(audioPlugs.has_value());
    EXPECT_EQ(audioPlugs->destinationPlugs, 8);
    EXPECT_EQ(audioPlugs->sourcePlugs, 11);

    auto respMusic = ParseResponse(Testing::Phase88Data::kResp_6_plug_info_music_0);
    ASSERT_TRUE(respMusic.has_value());
    auto musicPlugs = Cmd::ParseSubunitPlugs(*respMusic);
    ASSERT_TRUE(musicPlugs.has_value());
    EXPECT_EQ(musicPlugs->destinationPlugs, 10);
    EXPECT_EQ(musicPlugs->sourcePlugs, 6);
}

TEST(AvcFixtureTests, Phase88PlugSignalFormatParsesCorrectly) {
    auto resp = ParseResponse(Testing::Phase88Data::kResp_7_plug_signal_format_in_0_all_wildcard);
    ASSERT_TRUE(resp.has_value());
    auto fmt = Cmd::ParsePlugSignalFormat(*resp, ResponseCode::kImplementedStable);
    ASSERT_TRUE(fmt.has_value());
    EXPECT_EQ(fmt->plugId, 0);
    EXPECT_EQ(fmt->fmt, 0x90);  // AM824
    EXPECT_EQ(fmt->fdf[0], 0x02);  // 48 kHz
}

TEST(AvcFixtureTests, Phase88BridgeCoExtensionsParseCorrectly) {
    // Find bridgeco plug type response in records
    for (const auto& rec : Testing::kPhase88.records) {
        if (std::string_view(rec.name) == "bridgeco_plug_info_plug_type_in_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto pt = BridgeCo::ParsePlugType(*resp);
            ASSERT_TRUE(pt.has_value());
            EXPECT_EQ(*pt, BridgeCo::PlugType::kIsochronousStream);
        } else if (std::string_view(rec.name) == "bridgeco_plug_info_channel_count_in_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto cc = BridgeCo::ParseChannelCount(*resp);
            ASSERT_TRUE(cc.has_value());
            EXPECT_EQ(*cc, 11);
        } else if (std::string_view(rec.name) == "bridgeco_plug_info_channel_positions_in_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto pos = BridgeCo::ParseChannelPositions(*resp);
            ASSERT_TRUE(pos.has_value());
            EXPECT_EQ(pos->sectionCount, 3);
        } else if (std::string_view(rec.name) == "bridgeco_cluster_info_in_0_sec_1") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto pt = BridgeCo::ParseClusterPortType(*resp, 1);
            ASSERT_TRUE(pt.has_value());
            EXPECT_EQ(*pt, BridgeCo::PortType::kLine);
        } else if (std::string_view(rec.name) == "bridgeco_cluster_info_in_0_sec_2") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto pt = BridgeCo::ParseClusterPortType(*resp, 2);
            ASSERT_TRUE(pt.has_value());
            EXPECT_EQ(*pt, BridgeCo::PortType::kSpdif);
        } else if (std::string_view(rec.name) == "bridgeco_cluster_info_in_0_sec_3") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto pt = BridgeCo::ParseClusterPortType(*resp, 3);
            ASSERT_TRUE(pt.has_value());
            EXPECT_EQ(*pt, BridgeCo::PortType::kMidi);
        }
    }
}

TEST(AvcFixtureTests, Phase88FunctionBlocksParseCorrectly) {
    // FB 0: selector & feature must be NOT IMPLEMENTED
    for (const auto& rec : Testing::kPhase88.records) {
        if (std::string_view(rec.name) == "function_block_selector_status_fb_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kNotImplemented);
        } else if (std::string_view(rec.name) == "function_block_feature_mute_status_fb_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kNotImplemented);
        } else if (std::string_view(rec.name) == "function_block_selector_status_fb_1") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto sel = Cmd::ParseSelector(*resp, ResponseCode::kImplementedStable);
            ASSERT_TRUE(sel.has_value());
            EXPECT_EQ(sel->functionBlockId, 1);
            EXPECT_EQ(sel->inputPlug, 0);
        } else if (std::string_view(rec.name) == "function_block_feature_mute_status_fb_1") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto mute = Cmd::ParseFeatureMute(*resp, ResponseCode::kImplementedStable);
            ASSERT_TRUE(mute.has_value());
            EXPECT_FALSE(*mute);  // unmuted (0x60 = false)
        } else if (std::string_view(rec.name) == "function_block_feature_volume_current_fb_1") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto vol = Cmd::ParseFeatureVolume(*resp, ResponseCode::kImplementedStable);
            ASSERT_TRUE(vol.has_value());
            EXPECT_EQ(*vol, 0x0000);  // 0 dB
        }
    }
}

// ===========================================================================
// Apogee Duet Hardware Fixture Tests
// ===========================================================================

TEST(AvcFixtureTests, DuetAllRecordsParseConsistently) {
    const auto& device = Testing::kDuet;
    EXPECT_STREQ(device.key, "duet");
    EXPECT_EQ(device.guid, 0x0003DB0A0000D112ULL);
    EXPECT_GT(device.records.size(), 150U);

    size_t stableCount = 0;
    size_t notImplementedCount = 0;
    size_t rejectedCount = 0;

    for (const auto& rec : device.records) {
        ASSERT_TRUE(rec.ok) << "Failed exchange: " << rec.name;
        ASSERT_GE(rec.response.size(), 3U) << "Response too short: " << rec.name;

        auto respRes = ParseResponse(rec.response);
        ASSERT_TRUE(respRes.has_value()) << "ParseResponse failed for: " << rec.name;
        const auto& resp = *respRes;

        if (rec.responseCode == static_cast<uint8_t>(ResponseCode::kImplementedStable)) {
            stableCount++;
            EXPECT_EQ(resp.code, ResponseCode::kImplementedStable) << "Record: " << rec.name;
            auto ops = OperandsIf(resp, ResponseCode::kImplementedStable);
            EXPECT_TRUE(ops.has_value()) << "OperandsIf failed on stable record: " << rec.name;
        } else if (rec.responseCode == static_cast<uint8_t>(ResponseCode::kNotImplemented)) {
            notImplementedCount++;
            EXPECT_EQ(resp.code, ResponseCode::kNotImplemented) << "Record: " << rec.name;
            auto ops = OperandsIf(resp, ResponseCode::kImplementedStable);
            ASSERT_FALSE(ops.has_value()) << "OperandsIf should fail on NOT_IMPLEMENTED: " << rec.name;
            EXPECT_EQ(ops.error().kind, AvcErrorKind::kUnexpectedResponse);
            EXPECT_EQ(ops.error().response, ResponseCode::kNotImplemented);
        } else if (rec.responseCode == static_cast<uint8_t>(ResponseCode::kRejected)) {
            rejectedCount++;
            EXPECT_EQ(resp.code, ResponseCode::kRejected) << "Record: " << rec.name;
            auto ops = OperandsIf(resp, ResponseCode::kImplementedStable);
            ASSERT_FALSE(ops.has_value()) << "OperandsIf should fail on REJECTED: " << rec.name;
            EXPECT_EQ(ops.error().kind, AvcErrorKind::kUnexpectedResponse);
            EXPECT_EQ(ops.error().response, ResponseCode::kRejected);
        }
    }

    EXPECT_GT(stableCount, 50U);
    EXPECT_GT(notImplementedCount, 10U);
}

TEST(AvcFixtureTests, DuetUnitInfoParsesCorrectly) {
    auto resp = ParseResponse(Testing::DuetData::kResp_0_unit_info);
    ASSERT_TRUE(resp.has_value());

    auto info = Cmd::ParseUnitInfo(*resp);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->unitType, SubunitType::kAudio);
    EXPECT_EQ(info->unitId, 0x00);
    EXPECT_EQ(info->companyId[0], 0x00);
    EXPECT_EQ(info->companyId[1], 0x03);
    EXPECT_EQ(info->companyId[2], 0xDB);
}

TEST(AvcFixtureTests, DuetPlugInfoParsesCorrectly) {
    auto respUnit0 = ParseResponse(Testing::DuetData::kResp_3_plug_info_unit_00);
    ASSERT_TRUE(respUnit0.has_value());
    auto plugsIsoExt = Cmd::ParseUnitIsochronousExternalPlugs(*respUnit0);
    ASSERT_TRUE(plugsIsoExt.has_value());
    EXPECT_EQ(plugsIsoExt->isochronousInputs, 1);
    EXPECT_EQ(plugsIsoExt->isochronousOutputs, 1);
    EXPECT_EQ(plugsIsoExt->externalInputs, 1);
    EXPECT_EQ(plugsIsoExt->externalOutputs, 1);

    auto respAudio = ParseResponse(Testing::DuetData::kResp_5_plug_info_audio_0);
    ASSERT_TRUE(respAudio.has_value());
    auto audioPlugs = Cmd::ParseSubunitPlugs(*respAudio);
    ASSERT_TRUE(audioPlugs.has_value());
    EXPECT_EQ(audioPlugs->destinationPlugs, 1);
    EXPECT_EQ(audioPlugs->sourcePlugs, 1);

    auto respMusic = ParseResponse(Testing::DuetData::kResp_6_plug_info_music_0);
    ASSERT_TRUE(respMusic.has_value());
    auto musicPlugs = Cmd::ParseSubunitPlugs(*respMusic);
    ASSERT_TRUE(musicPlugs.has_value());
    EXPECT_EQ(musicPlugs->destinationPlugs, 3);
    EXPECT_EQ(musicPlugs->sourcePlugs, 3);
}

TEST(AvcFixtureTests, DuetExtendedStreamFormatParsesCorrectly) {
    // Duet supports draft 0xBF stream format single
    for (const auto& rec : Testing::kDuet.records) {
        if (std::string_view(rec.name) == "stream_format_0xBF_single_unit_iso_in_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->opcode, static_cast<Opcode>(0xBF));
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
            // Format data starts at operand 7 (frame byte 10)
            ASSERT_GE(rec.response.size(), 17U);
            std::span<const uint8_t> fmtBytes{rec.response.data() + 10, rec.response.size() - 10};
            auto parsedFmt = Cmd::ParseStreamFormatBlock(fmtBytes);
            ASSERT_TRUE(parsedFmt.has_value());
            EXPECT_EQ(parsedFmt->kind, Cmd::StreamFormat::Kind::kCompoundAm824);
            EXPECT_EQ(parsedFmt->compound.rate, StreamFormatRate::k48000);
            EXPECT_EQ(parsedFmt->compound.entryCount, 1U);
            EXPECT_EQ(parsedFmt->compound.entries[0].count, 2U);  // 2 channels
        }
    }
}

TEST(AvcFixtureTests, DuetFunctionBlocksAndInquiryValidate) {
    for (const auto& rec : Testing::kDuet.records) {
        if (std::string_view(rec.name) == "function_block_feature_mute_status_fb_1") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto mute = Cmd::ParseFeatureMute(*resp, ResponseCode::kImplementedStable);
            ASSERT_TRUE(mute.has_value());
            EXPECT_TRUE(*mute);  // muted (0x70 = true)
        } else if (std::string_view(rec.name) == "function_block_feature_volume_current_fb_1") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto vol = Cmd::ParseFeatureVolume(*resp, ResponseCode::kImplementedStable);
            ASSERT_TRUE(vol.has_value());
            EXPECT_EQ(*vol, 0x0000);
        } else if (std::string_view(rec.name) == "inquiry_feature_mute_on_fb_1_ch_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
        } else if (std::string_view(rec.name) == "inquiry_feature_mute_off_fb_1_ch_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
        } else if (std::string_view(rec.name) == "inquiry_feature_volume_fb_1_ch_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
        }
    }
}

} // namespace ASFW::AVC::Test
