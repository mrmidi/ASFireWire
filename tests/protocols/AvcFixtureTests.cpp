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

    auto info = Cmd::UnitInfoOperands::Read(resp->operands);
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
    auto sub0 = Cmd::SubunitInfoOperands::Read(resp0->operands);
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
    auto ops1 = OperandsIf(*resp1, ResponseCode::kImplementedStable);
    ASSERT_FALSE(ops1.has_value());
    EXPECT_EQ(ops1.error().kind, AvcErrorKind::kUnexpectedResponse);
    EXPECT_EQ(ops1.error().response, ResponseCode::kNotImplemented);
}

TEST(AvcFixtureTests, Phase88PlugInfoParsesCorrectly) {
    auto respUnit0 = ParseResponse(Testing::Phase88Data::kResp_3_plug_info_unit_00);
    ASSERT_TRUE(respUnit0.has_value());
    auto plugsIsoExt = Cmd::UnitPlugInfoIsoExtOperands::Read(respUnit0->operands);
    ASSERT_TRUE(plugsIsoExt.has_value());
    EXPECT_EQ(plugsIsoExt->isochronousInputs, 2);
    EXPECT_EQ(plugsIsoExt->isochronousOutputs, 2);
    EXPECT_EQ(plugsIsoExt->externalInputs, 8);
    EXPECT_EQ(plugsIsoExt->externalOutputs, 7);

    auto respUnit1 = ParseResponse(Testing::Phase88Data::kResp_4_plug_info_unit_01);
    ASSERT_TRUE(respUnit1.has_value());
    auto plugsAsync = Cmd::UnitPlugInfoAsyncOperands::Read(respUnit1->operands);
    ASSERT_TRUE(plugsAsync.has_value());
    EXPECT_EQ(plugsAsync->asynchronousInputs, 0);
    EXPECT_EQ(plugsAsync->asynchronousOutputs, 0);

    auto respAudio = ParseResponse(Testing::Phase88Data::kResp_5_plug_info_audio_0);
    ASSERT_TRUE(respAudio.has_value());
    auto audioPlugs = Cmd::SubunitPlugInfoOperands::Read(respAudio->operands);
    ASSERT_TRUE(audioPlugs.has_value());
    EXPECT_EQ(audioPlugs->destinationPlugs, 8);
    EXPECT_EQ(audioPlugs->sourcePlugs, 11);

    auto respMusic = ParseResponse(Testing::Phase88Data::kResp_6_plug_info_music_0);
    ASSERT_TRUE(respMusic.has_value());
    auto musicPlugs = Cmd::SubunitPlugInfoOperands::Read(respMusic->operands);
    ASSERT_TRUE(musicPlugs.has_value());
    EXPECT_EQ(musicPlugs->destinationPlugs, 10);
    EXPECT_EQ(musicPlugs->sourcePlugs, 6);
}

TEST(AvcFixtureTests, Phase88PlugSignalFormatParsesCorrectly) {
    auto resp = ParseResponse(Testing::Phase88Data::kResp_7_plug_signal_format_in_0_all_wildcard);
    ASSERT_TRUE(resp.has_value());
    auto fmt = Cmd::PlugSignalFormatOperands::Read(resp->operands);
    ASSERT_TRUE(fmt.has_value());
    EXPECT_EQ(fmt->plugId, 0);
    EXPECT_EQ(fmt->fmt, 0x90);  // AM824
    EXPECT_EQ(fmt->fdf[0], 0x02);  // 48 kHz
}

TEST(AvcFixtureTests, Phase88StreamFormatListHighLevelParsing) {
    // Phase 88 supports 5 rates per direction (indices 0..4), all 10 PCM + 1 MIDI.
    const StreamFormatRate expectedRates[] = {
        StreamFormatRate::k32000,
        StreamFormatRate::k44100,
        StreamFormatRate::k48000,
        StreamFormatRate::k88200,
        StreamFormatRate::k96000,
    };

    size_t inListFound = 0;
    size_t outListFound = 0;

    for (const auto& rec : Testing::kPhase88.records) {
        std::string_view name{rec.name};
        if (name.starts_with("stream_format_0x2F_list_unit_iso_in_0_idx_")) {
            uint8_t idx = static_cast<uint8_t>(name.back() - '0');
            if (idx <= 4) {
                auto resp = ParseResponse(rec.response);
                ASSERT_TRUE(resp.has_value());
                auto entry = Cmd::StreamFormatListOperands::Read(resp->operands, idx);
                ASSERT_TRUE(entry.has_value()) << "Failed to parse in list entry " << int(idx);
                EXPECT_EQ(entry->index, idx);
                EXPECT_EQ(entry->format.kind, Cmd::StreamFormat::Kind::kCompoundAm824);
                EXPECT_EQ(entry->format.compound.rate, expectedRates[idx]);
                EXPECT_EQ(entry->format.compound.PcmChannels(), 10U);
                EXPECT_EQ(entry->format.compound.MidiChannels(), 1U);
                EXPECT_TRUE(entry->format.compound.OnlyPcmAndMidi());
                inListFound++;
            }
        } else if (name.starts_with("stream_format_0x2F_list_unit_iso_out_0_idx_")) {
            uint8_t idx = static_cast<uint8_t>(name.back() - '0');
            if (idx <= 4) {
                auto resp = ParseResponse(rec.response);
                ASSERT_TRUE(resp.has_value());
                auto entry = Cmd::StreamFormatListOperands::Read(resp->operands, idx);
                ASSERT_TRUE(entry.has_value()) << "Failed to parse out list entry " << int(idx);
                EXPECT_EQ(entry->index, idx);
                EXPECT_EQ(entry->format.kind, Cmd::StreamFormat::Kind::kCompoundAm824);
                EXPECT_EQ(entry->format.compound.rate, expectedRates[idx]);
                EXPECT_EQ(entry->format.compound.PcmChannels(), 10U);
                EXPECT_EQ(entry->format.compound.MidiChannels(), 1U);
                EXPECT_TRUE(entry->format.compound.OnlyPcmAndMidi());
                outListFound++;
            }
        } else if (name == "stream_format_0x2F_single_unit_iso_in_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto single = Cmd::StreamFormatSingleOperands::Read(resp->operands);
            ASSERT_TRUE(single.has_value());
            EXPECT_EQ(single->format.kind, Cmd::StreamFormat::Kind::kCompoundAm824);
            EXPECT_EQ(single->format.compound.rate, StreamFormatRate::k48000);
            EXPECT_EQ(single->format.compound.PcmChannels(), 10U);
            EXPECT_EQ(single->format.compound.MidiChannels(), 1U);
            EXPECT_TRUE(single->format.compound.OnlyPcmAndMidi());
        }
    }

    EXPECT_EQ(inListFound, 5U);
    EXPECT_EQ(outListFound, 5U);
}

TEST(AvcFixtureTests, Phase88BridgeCoExtensionsParseCorrectly) {
    // Find bridgeco plug type response in records
    for (const auto& rec : Testing::kPhase88.records) {
        if (std::string_view(rec.name) == "bridgeco_plug_info_plug_type_in_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto reply = BridgeCo::ExtendedPlugInfoOperands::Read(resp->operands, BridgeCo::InfoType::kPlugType);
            ASSERT_TRUE(reply.has_value());
            auto pt = reply->AsPlugType();
            ASSERT_TRUE(pt.has_value());
            EXPECT_EQ(*pt, BridgeCo::PlugType::kIsochronousStream);
        } else if (std::string_view(rec.name) == "bridgeco_plug_info_channel_count_in_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto reply = BridgeCo::ExtendedPlugInfoOperands::Read(resp->operands, BridgeCo::InfoType::kChannelCount);
            ASSERT_TRUE(reply.has_value());
            auto cc = reply->AsChannelCount();
            ASSERT_TRUE(cc.has_value());
            EXPECT_EQ(*cc, 11);
        } else if (std::string_view(rec.name) == "bridgeco_plug_info_channel_positions_in_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto reply = BridgeCo::ExtendedPlugInfoOperands::Read(resp->operands, BridgeCo::InfoType::kChannelPositions);
            ASSERT_TRUE(reply.has_value());
            auto pos = reply->AsChannelPositions();
            ASSERT_TRUE(pos.has_value());
            EXPECT_EQ(pos->sectionCount, 3);
        } else if (std::string_view(rec.name) == "bridgeco_cluster_info_in_0_sec_1") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto reply = BridgeCo::ExtendedPlugInfoOperands::Read(resp->operands, BridgeCo::InfoType::kClusterInfo);
            ASSERT_TRUE(reply.has_value());
            auto pt = reply->AsClusterPortType(1);
            ASSERT_TRUE(pt.has_value());
            EXPECT_EQ(*pt, BridgeCo::PortType::kLine);
        } else if (std::string_view(rec.name) == "bridgeco_cluster_info_in_0_sec_2") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto reply = BridgeCo::ExtendedPlugInfoOperands::Read(resp->operands, BridgeCo::InfoType::kClusterInfo);
            ASSERT_TRUE(reply.has_value());
            auto pt = reply->AsClusterPortType(2);
            ASSERT_TRUE(pt.has_value());
            EXPECT_EQ(*pt, BridgeCo::PortType::kSpdif);
        } else if (std::string_view(rec.name) == "bridgeco_cluster_info_in_0_sec_3") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto reply = BridgeCo::ExtendedPlugInfoOperands::Read(resp->operands, BridgeCo::InfoType::kClusterInfo);
            ASSERT_TRUE(reply.has_value());
            auto pt = reply->AsClusterPortType(3);
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
            auto sel = Cmd::SelectorOperands::Read(resp->operands);
            ASSERT_TRUE(sel.has_value());
            EXPECT_EQ(sel->functionBlockId, 1);
            EXPECT_EQ(sel->inputPlug, 0);
        } else if (std::string_view(rec.name) == "function_block_feature_mute_status_fb_1") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto mute = Cmd::FeatureOperands::Read(resp->operands);
            ASSERT_TRUE(mute.has_value());
            EXPECT_FALSE(mute->AsMute());  // unmuted (0x60 = false)
        } else if (std::string_view(rec.name) == "function_block_feature_volume_current_fb_1") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto vol = Cmd::FeatureOperands::Read(resp->operands);
            ASSERT_TRUE(vol.has_value());
            EXPECT_EQ(vol->AsVolume(), AvcVolume::FromRaw(0x0000));  // 0 dB
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
    size_t acceptedCount = 0;
    size_t csrCount = 0;

    for (const auto& rec : device.records) {
        ASSERT_TRUE(rec.ok) << "Failed exchange: " << rec.name;

        // Raw IEEE 1394 CSR transactions (Oxford ASIC registers / DSP meters)
        if (!rec.command.empty() && rec.command[0] == 0xF0) {
            csrCount++;
            EXPECT_GE(rec.response.size(), 4U) << "CSR response too short: " << rec.name;
            continue;
        }

        ASSERT_GE(rec.response.size(), 3U) << "Response too short: " << rec.name;

        auto respRes = ParseResponse(rec.response);
        ASSERT_TRUE(respRes.has_value()) << "ParseResponse failed for: " << rec.name;
        const auto& resp = *respRes;

        if (rec.responseCode == static_cast<uint8_t>(ResponseCode::kImplementedStable)) {
            stableCount++;
            EXPECT_EQ(resp.code, ResponseCode::kImplementedStable) << "Record: " << rec.name;
            auto ops = OperandsIf(resp, ResponseCode::kImplementedStable);
            EXPECT_TRUE(ops.has_value()) << "OperandsIf failed on stable record: " << rec.name;
        } else if (rec.responseCode == static_cast<uint8_t>(ResponseCode::kAccepted)) {
            acceptedCount++;
            EXPECT_EQ(resp.code, ResponseCode::kAccepted) << "Record: " << rec.name;
            auto ops = OperandsIf(resp, ResponseCode::kAccepted);
            EXPECT_TRUE(ops.has_value()) << "OperandsIf failed on accepted record: " << rec.name;
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
    EXPECT_GT(acceptedCount, 5U);
    EXPECT_EQ(csrCount, 4U);
}

TEST(AvcFixtureTests, DuetUnitInfoParsesCorrectly) {
    auto resp = ParseResponse(Testing::DuetData::kResp_0_unit_info);
    ASSERT_TRUE(resp.has_value());

    auto info = Cmd::UnitInfoOperands::Read(resp->operands);
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
    auto plugsIsoExt = Cmd::UnitPlugInfoIsoExtOperands::Read(respUnit0->operands);
    ASSERT_TRUE(plugsIsoExt.has_value());
    EXPECT_EQ(plugsIsoExt->isochronousInputs, 1);
    EXPECT_EQ(plugsIsoExt->isochronousOutputs, 1);
    EXPECT_EQ(plugsIsoExt->externalInputs, 1);
    EXPECT_EQ(plugsIsoExt->externalOutputs, 1);

    auto respAudio = ParseResponse(Testing::DuetData::kResp_5_plug_info_audio_0);
    ASSERT_TRUE(respAudio.has_value());
    auto audioPlugs = Cmd::SubunitPlugInfoOperands::Read(respAudio->operands);
    ASSERT_TRUE(audioPlugs.has_value());
    EXPECT_EQ(audioPlugs->destinationPlugs, 1);
    EXPECT_EQ(audioPlugs->sourcePlugs, 1);

    auto respMusic = ParseResponse(Testing::DuetData::kResp_6_plug_info_music_0);
    ASSERT_TRUE(respMusic.has_value());
    auto musicPlugs = Cmd::SubunitPlugInfoOperands::Read(respMusic->operands);
    ASSERT_TRUE(musicPlugs.has_value());
    EXPECT_EQ(musicPlugs->destinationPlugs, 3);
    EXPECT_EQ(musicPlugs->sourcePlugs, 3);
}

TEST(AvcFixtureTests, DuetExtendedStreamFormatParsesCorrectly) {
    // Duet supports draft 0xBF stream format single
    bool inParsed = false;
    bool outParsed = false;

    for (const auto& rec : Testing::kDuet.records) {
        std::string_view name{rec.name};
        if (name == "stream_format_0xBF_single_unit_iso_in_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->opcode, static_cast<Opcode>(0xBF));
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);

            auto single = Cmd::StreamFormatSingleOperands::Read(resp->operands);
            ASSERT_TRUE(single.has_value());
            EXPECT_EQ(single->format.kind, Cmd::StreamFormat::Kind::kCompoundAm824);
            EXPECT_EQ(single->format.compound.rate, StreamFormatRate::k48000);
            EXPECT_EQ(single->format.compound.PcmChannels(), 2U);
            EXPECT_EQ(single->format.compound.MidiChannels(), 0U);
            inParsed = true;
        } else if (name == "stream_format_0xBF_single_unit_iso_out_0") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->opcode, static_cast<Opcode>(0xBF));
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);

            auto single = Cmd::StreamFormatSingleOperands::Read(resp->operands);
            ASSERT_TRUE(single.has_value());
            EXPECT_EQ(single->format.kind, Cmd::StreamFormat::Kind::kCompoundAm824);
            EXPECT_EQ(single->format.compound.rate, StreamFormatRate::k48000);
            EXPECT_EQ(single->format.compound.PcmChannels(), 2U);
            EXPECT_EQ(single->format.compound.MidiChannels(), 0U);
            outParsed = true;
        }
    }

    EXPECT_TRUE(inParsed);
    EXPECT_TRUE(outParsed);
}

TEST(AvcFixtureTests, DuetFunctionBlocksAndInquiryValidate) {
    for (const auto& rec : Testing::kDuet.records) {
        if (std::string_view(rec.name) == "function_block_feature_mute_status_fb_1") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto mute = Cmd::FeatureOperands::Read(resp->operands);
            ASSERT_TRUE(mute.has_value());
            EXPECT_TRUE(mute->AsMute());  // muted (0x70 = true)
        } else if (std::string_view(rec.name) == "function_block_feature_volume_current_fb_1") {
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            auto vol = Cmd::FeatureOperands::Read(resp->operands);
            ASSERT_TRUE(vol.has_value());
            EXPECT_EQ(vol->AsVolume(), AvcVolume::FromRaw(0x0000));
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

TEST(AvcFixtureTests, DuetOxfordAsicRegistersAndDspMetersValidate) {
    bool foundFwId = false;
    bool foundHwId = false;
    bool foundInMeters = false;
    bool foundMixMeters = false;

    for (const auto& rec : Testing::kDuet.records) {
        if (std::string_view(rec.name) == "oxford_firmware_id") {
            foundFwId = true;
            ASSERT_EQ(rec.response.size(), 4U);
            EXPECT_EQ(rec.response[0], 0x97);
            EXPECT_EQ(rec.response[1], 0x10);
            EXPECT_EQ(rec.response[2], 0x01);
            EXPECT_EQ(rec.response[3], 0x05);
        } else if (std::string_view(rec.name) == "oxford_hardware_id") {
            foundHwId = true;
            ASSERT_EQ(rec.response.size(), 4U);
            // "971\0" = 0x39, 0x37, 0x31, 0x00
            EXPECT_EQ(rec.response[0], '9');
            EXPECT_EQ(rec.response[1], '7');
            EXPECT_EQ(rec.response[2], '1');
            EXPECT_EQ(rec.response[3], '\0');
        } else if (std::string_view(rec.name) == "apogee_dsp_input_meters") {
            foundInMeters = true;
            EXPECT_EQ(rec.response.size(), 8U);
        } else if (std::string_view(rec.name) == "apogee_dsp_mixer_meters") {
            foundMixMeters = true;
            EXPECT_EQ(rec.response.size(), 16U);
        }
    }

    EXPECT_TRUE(foundFwId);
    EXPECT_TRUE(foundHwId);
    EXPECT_TRUE(foundInMeters);
    EXPECT_TRUE(foundMixMeters);
}

TEST(AvcFixtureTests, DuetApogeeVendorCommandsValidate) {
    bool foundHwState = false;
    bool foundGain = false;
    bool foundPhantom = false;
    bool foundMute = false;
    bool foundInquiryPhantom = false;
    bool foundMixer = false;
    bool foundFwVersion = false;
    bool foundMicsGrouped = false;
    bool foundIdentify = false;
    bool foundInputsMuted = false;
    bool foundLimitedGain = false;
    bool foundInquiryMicsGrouped = false;

    for (const auto& rec : Testing::kDuet.records) {
        if (std::string_view(rec.name) == "apogee_hw_state") {
            foundHwState = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
            EXPECT_TRUE(resp->address.IsUnit());
            EXPECT_EQ(resp->opcode, Opcode::kVendorDependent);
            // 6-byte prefix (OUI 00:03:DB + 'PCM') + 3 bytes header + 11 bytes status = 20 operands (23 frame bytes)
            ASSERT_EQ(resp->operands.size(), 20U);
            EXPECT_EQ(resp->operands[0], 0x00);
            EXPECT_EQ(resp->operands[1], 0x03);
            EXPECT_EQ(resp->operands[2], 0xDB);
            EXPECT_EQ(resp->operands[3], 'P');
            EXPECT_EQ(resp->operands[4], 'C');
            EXPECT_EQ(resp->operands[5], 'M');
            EXPECT_EQ(resp->operands[6], 0x07); // HwState cmd
        } else if (std::string_view(rec.name) == "apogee_firmware_version") {
            foundFwVersion = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
            ASSERT_GE(resp->operands.size(), 13U);
            EXPECT_EQ(resp->operands[6], 0x0A); // Version cmd (0x0A)
            EXPECT_EQ(resp->operands[9], 0x01); // Major 1
            EXPECT_EQ(resp->operands[10], 0x02); // Minor 2
            EXPECT_EQ(resp->operands[11], 0x00); // Rev 0
            EXPECT_EQ(resp->operands[12], 0x04); // Build 4 (v1.2.0.4)
        } else if (std::string_view(rec.name) == "apogee_in_gain_ch0") {
            foundGain = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
            ASSERT_GE(resp->operands.size(), 10U);
            EXPECT_EQ(resp->operands[6], 0x05); // InGain cmd
            EXPECT_EQ(resp->operands[9], 0x0A); // 10 dB
        } else if (std::string_view(rec.name) == "apogee_mics_grouped") {
            foundMicsGrouped = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
            ASSERT_GE(resp->operands.size(), 10U);
            EXPECT_EQ(resp->operands[6], 0x08); // MicsGrouped cmd
            EXPECT_EQ(resp->operands[9], 0x60); // Ungrouped (0x60)
        } else if (std::string_view(rec.name) == "apogee_mic_phantom_ch0") {
            foundPhantom = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
            ASSERT_GE(resp->operands.size(), 10U);
            EXPECT_EQ(resp->operands[6], 0x03); // Phantom cmd
            EXPECT_EQ(resp->operands[9], 0x60); // Off (0x60)
        } else if (std::string_view(rec.name) == "apogee_out_mute") {
            foundMute = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
            ASSERT_GE(resp->operands.size(), 10U);
            EXPECT_EQ(resp->operands[6], 0x09); // OutMute cmd
            EXPECT_EQ(resp->operands[9], 0x70); // Muted (0x70)
        } else if (std::string_view(rec.name) == "apogee_identify") {
            foundIdentify = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
            ASSERT_GE(resp->operands.size(), 10U);
            EXPECT_EQ(resp->operands[6], 0x12); // Identify cmd
            EXPECT_EQ(resp->operands[9], 0x60); // Off/idle (0x60)
        } else if (std::string_view(rec.name) == "apogee_inputs_muted") {
            foundInputsMuted = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
            ASSERT_GE(resp->operands.size(), 10U);
            EXPECT_EQ(resp->operands[6], 0x23); // InputsMuted cmd
            EXPECT_EQ(resp->operands[9], 0x70); // Muted (0x70)
        } else if (std::string_view(rec.name) == "apogee_limited_gain_range") {
            foundLimitedGain = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
            ASSERT_GE(resp->operands.size(), 10U);
            EXPECT_EQ(resp->operands[6], 0x1E); // LimitedGainRange cmd
            EXPECT_EQ(resp->operands[9], 0x60); // Normal range (0x60)
        } else if (std::string_view(rec.name) == "inquiry_apogee_mic_phantom_ch0_on") {
            foundInquiryPhantom = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kAccepted);
        } else if (std::string_view(rec.name) == "inquiry_apogee_mics_grouped_on") {
            foundInquiryMicsGrouped = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kAccepted);
        } else if (std::string_view(rec.name) == "apogee_mixer_src_0_dst_0") {
            foundMixer = true;
            auto resp = ParseResponse(rec.response);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->code, ResponseCode::kImplementedStable);
            EXPECT_EQ(resp->opcode, Opcode::kVendorDependent);
            ASSERT_GE(resp->operands.size(), 9U);
            EXPECT_EQ(resp->operands[6], 0x10); // Mixer cmd
        }
    }

    EXPECT_TRUE(foundHwState);
    EXPECT_TRUE(foundFwVersion);
    EXPECT_TRUE(foundGain);
    EXPECT_TRUE(foundMicsGrouped);
    EXPECT_TRUE(foundPhantom);
    EXPECT_TRUE(foundMute);
    EXPECT_TRUE(foundIdentify);
    EXPECT_TRUE(foundInputsMuted);
    EXPECT_TRUE(foundLimitedGain);
    EXPECT_TRUE(foundInquiryPhantom);
    EXPECT_TRUE(foundInquiryMicsGrouped);
    EXPECT_TRUE(foundMixer);
}

} // namespace ASFW::AVC::Test

