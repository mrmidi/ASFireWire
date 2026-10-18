// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>
#include "ASFWDriver/Audio/Protocols/Prism/Orpheus/OrpheusVendorCodec.hpp"
#include <array>
#include <vector>

using namespace ASFW::Audio::Prism::Orpheus;

namespace {

std::vector<uint8_t> MakeResponse(uint8_t ctype,
                                  OrpheusCommand command,
                                  std::span<const uint8_t> payload,
                                  size_t length = Wire::kFrameLength) {
    std::vector<uint8_t> bytes(length, 0);
    bytes[0] = ctype;
    bytes[1] = Wire::kSubunitAudio;
    bytes[2] = Wire::kOpcodeVendorDependent;
    bytes[3] = Wire::kCompanyId[0];
    bytes[4] = Wire::kCompanyId[1];
    bytes[5] = Wire::kCompanyId[2];
    bytes[Wire::kCommandOffset] = static_cast<uint8_t>(command);
    for (size_t i = 0; i < payload.size(); ++i) {
        bytes[Wire::kPayloadOffset + i] = payload[i];
    }
    return bytes;
}

} // namespace

TEST(OrpheusVendorCodecTests, StatusFrameIsFifteenBytesWithPrismHeader) {
    const auto frame = OrpheusVendorCodec::BuildUnitStateStatus();
    ASSERT_EQ(frame.size(), 15U);
    const std::vector<uint8_t> expectedPrefix{
        0x01, 0x08, 0x00, 0x00, 0x11, 0x98, 0xBF
    };
    EXPECT_TRUE(std::equal(expectedPrefix.begin(), expectedPrefix.end(), frame.begin()));
    for (size_t i = 7; i < 15; ++i) {
        EXPECT_EQ(frame[i], 0U);
    }
}

TEST(OrpheusVendorCodecTests, MasterVolumeIsBigEndianSigned) {
    const auto frame = OrpheusVendorCodec::BuildMasterVolumeControl(static_cast<int16_t>(-0x0A80));
    ASSERT_EQ(frame.size(), 15U);
    EXPECT_EQ(frame[0], 0x00); // Control
    EXPECT_EQ(frame[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::MasterVolume));
    EXPECT_EQ(frame[7], 0xF5);
    EXPECT_EQ(frame[8], 0x80);
}

TEST(OrpheusVendorCodecTests, VolumeSelectSendsLowByteFirst) {
    const auto frame = OrpheusVendorCodec::BuildVolumeSelectControl(0x0301);
    ASSERT_EQ(frame.size(), 15U);
    EXPECT_EQ(frame[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::VolumeSelect));
    EXPECT_EQ(frame[7], 0x01); // low byte
    EXPECT_EQ(frame[8], 0x03); // high byte
}

TEST(OrpheusVendorCodecTests, AnalogControlCarriesChannelThenValue) {
    const auto frame = OrpheusVendorCodec::BuildAnalogControl(OrpheusCommand::MicGain, 3, 42);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ((*frame)[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::MicGain));
    EXPECT_EQ((*frame)[7], 3);
    EXPECT_EQ((*frame)[8], 42);

    // Channel 8 is invalid (Orpheus has 8 channels: 0..7)
    const auto invalid = OrpheusVendorCodec::BuildAnalogControl(OrpheusCommand::MicGain, 8, 42);
    EXPECT_FALSE(invalid.has_value());
}

TEST(OrpheusVendorCodecTests, MixFramesUseBusInputAndBigEndianGain) {
    const auto gain = OrpheusVendorCodec::BuildMixInputGainControl(5, 11, -256);
    ASSERT_TRUE(gain.has_value());
    EXPECT_EQ((*gain)[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::MixInputGain));
    EXPECT_EQ((*gain)[7], 5);
    EXPECT_EQ((*gain)[8], 11);
    EXPECT_EQ((*gain)[9], 0xFF);
    EXPECT_EQ((*gain)[10], 0x00);

    const auto solo = OrpheusVendorCodec::BuildMixBusSoloControl(1, 0x0201);
    ASSERT_TRUE(solo.has_value());
    EXPECT_EQ((*solo)[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::MixSolo));
    EXPECT_EQ((*solo)[7], 1);
    EXPECT_EQ((*solo)[8], 0x02);
    EXPECT_EQ((*solo)[9], 0x01);

    const auto balance = OrpheusVendorCodec::BuildMixInputPositionControl(0, 2, -7, true);
    ASSERT_TRUE(balance.has_value());
    EXPECT_EQ((*balance)[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::MixInputBalance));
    EXPECT_EQ((*balance)[7], 0);
    EXPECT_EQ((*balance)[8], 2);
    EXPECT_EQ((*balance)[9], static_cast<uint8_t>(static_cast<int8_t>(-7)));

    const auto pan = OrpheusVendorCodec::BuildMixInputPositionControl(0, 3, 20, false);
    ASSERT_TRUE(pan.has_value());
    EXPECT_EQ((*pan)[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::MixInputPan));
    EXPECT_EQ((*pan)[7], 0);
    EXPECT_EQ((*pan)[8], 3);
    EXPECT_EQ((*pan)[9], 20);

    EXPECT_FALSE(OrpheusVendorCodec::BuildMixBusGainControl(6, 0).has_value()); // bus 6 invalid (0..5)
    EXPECT_FALSE(OrpheusVendorCodec::BuildMixInputMuteControl(0, 12, true).has_value()); // input 12 invalid (0..11)
}

TEST(OrpheusVendorCodecTests, MixBusStatusFrameIsSeventyFourBytes) {
    const auto frame = OrpheusVendorCodec::BuildMixBusStatus(4);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->size(), 74U);
    EXPECT_EQ((*frame)[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::MixBusState));
    EXPECT_EQ((*frame)[7], 4);
}

TEST(OrpheusVendorCodecTests, MixDefeatStatusAddressesAllBuses) {
    const auto frame = OrpheusVendorCodec::BuildMixDefeatStatus();
    ASSERT_EQ(frame.size(), 15U);
    EXPECT_EQ(frame[Wire::kCommandOffset], static_cast<uint8_t>(OrpheusCommand::MixDefeat));
    EXPECT_EQ(frame[7], Wire::kAllMixBuses); // 7 addresses all buses
}

TEST(OrpheusVendorCodecTests, ResponseValidationRequiresCtypeCompanyAndCommand) {
    const uint8_t payload[] = {0x01, 0x02};
    const auto ok = MakeResponse(Wire::kResponseStable, OrpheusCommand::UnitState, payload);

    EXPECT_TRUE(OrpheusVendorCodec::ValidateResponse(ok, OrpheusCommand::UnitState, false).has_value());
    EXPECT_FALSE(OrpheusVendorCodec::ValidateResponse(ok, OrpheusCommand::UnitState, true).has_value()); // expected control
    EXPECT_FALSE(OrpheusVendorCodec::ValidateResponse(ok, OrpheusCommand::DigitalState, false).has_value()); // wrong command

    auto wrongCompany = ok;
    wrongCompany[5] = 0x99;
    EXPECT_FALSE(OrpheusVendorCodec::ValidateResponse(wrongCompany, OrpheusCommand::UnitState, false).has_value());

    const auto rejected = MakeResponse(Wire::kResponseRejected, OrpheusCommand::Identify, payload);
    EXPECT_FALSE(OrpheusVendorCodec::ValidateResponse(rejected, OrpheusCommand::Identify, true).has_value());

    const auto accepted = MakeResponse(Wire::kResponseAccepted, OrpheusCommand::Identify, payload);
    EXPECT_TRUE(OrpheusVendorCodec::ValidateResponse(accepted, OrpheusCommand::Identify, true).has_value());
}

TEST(OrpheusVendorCodecTests, FirmwareVersionThresholds) {
    const uint8_t payloadLegacy[] = {1, 2};
    const auto respLegacy = MakeResponse(Wire::kResponseStable, OrpheusCommand::FirmwareVersion, payloadLegacy);
    const auto legacy = OrpheusVendorCodec::ParseFirmwareVersion(respLegacy);
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ(legacy->major, 1);
    EXPECT_EQ(legacy->minor, 2);
    EXPECT_FALSE(legacy->HasMeterStatusQuadlet());
    EXPECT_EQ(legacy->MeterReadLength(), 200U);
    EXPECT_TRUE(legacy->SupportsAdatDirect());

    const uint8_t payloadV13[] = {1, 3};
    const auto respV13 = MakeResponse(Wire::kResponseStable, OrpheusCommand::FirmwareVersion, payloadV13);
    const auto current = OrpheusVendorCodec::ParseFirmwareVersion(respV13);
    ASSERT_TRUE(current.has_value());
    EXPECT_TRUE(current->HasMeterStatusQuadlet());
    EXPECT_EQ(current->MeterReadLength(), 204U);

    const OrpheusFirmwareVersion v11{1, 1};
    EXPECT_FALSE(v11.SupportsAdatDirect());

    const OrpheusFirmwareVersion v09{0, 9};
    EXPECT_FALSE(v09.SupportsAdatDirect());
}

TEST(OrpheusVendorCodecTests, UnitStateParsesPackedFields) {
    const uint8_t payload[] = {
        0xF6, 0x00,              // masterVolume = -0x0A00 (-2560 in 1/256 dB = -10 dB)
        0x03, 0x0F,              // volumeSelectMask = 0x030F
        0b0001'0111,             // meterMode=1 (bits 7:4), identify=1 (bit 2), masterLock=1 (bit 1), masterMute=1 (bit 0)
        0x12,                    // clockOutput=1 (bits 7:4), sampleRateConverter=2 (bits 3:0)
        0x03,                    // adatMode = 3 (Send/Return)
        0x83                     // meterBrightness=8 (bits 7:4), headphoneSource=3 (bits 3:0)
    };
    const auto resp = MakeResponse(Wire::kResponseStable, OrpheusCommand::UnitState, payload);
    const auto state = OrpheusVendorCodec::ParseUnitState(resp);
    ASSERT_TRUE(state.has_value());

    EXPECT_EQ(state->masterVolume, -0x0A00);
    EXPECT_EQ(state->volumeSelectMask, 0x030F);
    EXPECT_TRUE(state->masterMute);
    EXPECT_TRUE(state->masterLock);
    EXPECT_TRUE(state->identify);
    EXPECT_EQ(state->frontPanelMeterMode, 1);
    EXPECT_EQ(state->sampleRateConverter, 2);
    EXPECT_EQ(state->clockOutput, 1);
    EXPECT_EQ(state->adatMode, 3);
    EXPECT_TRUE(state->HasAdatInput());
    EXPECT_EQ(state->headphoneSource, 3);
    EXPECT_EQ(state->meterBrightness, 8);
}

TEST(OrpheusVendorCodecTests, AnalogChannelStateParsesFlagsAndGains) {
    const uint8_t payload[] = {
        0x02,                    // channel 2
        0x00,                    // inputType: Mic
        0b0110'0101,             // phase=1 (bit 6), phantom=1 (bit 5), overkiller=1 (bit 2), lineLevel=1 (bit 0)
        0x02,                    // filter: 75Hz
        48,                      // micGain: 48 dB
        15                       // instrumentGain: 15 dB
    };
    const auto resp = MakeResponse(Wire::kResponseStable, OrpheusCommand::AnalogChannelState, payload);

    const auto state = OrpheusVendorCodec::ParseAnalogChannelState(resp, 2);
    ASSERT_TRUE(state.has_value());

    EXPECT_EQ(state->inputType, 0);
    EXPECT_EQ(state->lineInputLevel, 1);
    EXPECT_TRUE(state->overkiller);
    EXPECT_FALSE(state->midSide);
    EXPECT_TRUE(state->phantom);
    EXPECT_TRUE(state->phase);
    EXPECT_EQ(state->filter, 2);
    EXPECT_EQ(state->micGain, 48);
}

TEST(OrpheusVendorCodecTests, SignalSourceCodecMatchesPlugs) {
    // 1. Without ADAT input
    const auto localNoAdat = OrpheusVendorCodec::BuildSignalSourceControl(OrpheusSyncSource::Local, false);
    ASSERT_TRUE(localNoAdat.has_value());
    EXPECT_EQ((*localNoAdat)[0], 0x00); // Control
    EXPECT_EQ((*localNoAdat)[1], 0xFF); // Unit
    EXPECT_EQ((*localNoAdat)[2], 0x1A); // SIGNAL SOURCE
    EXPECT_EQ((*localNoAdat)[4], 0xFF);
    EXPECT_EQ((*localNoAdat)[5], 0x87); // Local without ADAT = 0x87
    EXPECT_EQ((*localNoAdat)[6], 0x60); // Dest subunit
    EXPECT_EQ((*localNoAdat)[7], 0x07); // Dest plug without ADAT = 0x07

    // 2. With ADAT input
    const auto localWithAdat = OrpheusVendorCodec::BuildSignalSourceControl(OrpheusSyncSource::Local, true);
    ASSERT_TRUE(localWithAdat.has_value());
    EXPECT_EQ((*localWithAdat)[5], 0x88); // Local with ADAT = 0x88
    EXPECT_EQ((*localWithAdat)[7], 0x08); // Dest plug with ADAT = 0x08

    const auto adatSource = OrpheusVendorCodec::BuildSignalSourceControl(OrpheusSyncSource::Adat, true);
    ASSERT_TRUE(adatSource.has_value());
    EXPECT_EQ((*adatSource)[5], 0x86);

    // ADAT source cannot be set if hasAdatInput is false
    EXPECT_FALSE(OrpheusVendorCodec::BuildSignalSourceControl(OrpheusSyncSource::Adat, false).has_value());

    // 3. Status parse
    const std::vector<uint8_t> respAdat{Wire::kResponseStable, 0xFF, 0x1A, 0x0F, 0xFF, 0x86, 0x60, 0x08};
    const auto parsedAdat = OrpheusVendorCodec::ParseSignalSourceStatus(respAdat, true);
    ASSERT_TRUE(parsedAdat.has_value());
    EXPECT_EQ(*parsedAdat, OrpheusSyncSource::Adat);

    // When hasAdatInput is false, 0x86 parses as WordClock
    const std::vector<uint8_t> respWck{Wire::kResponseStable, 0xFF, 0x1A, 0x0F, 0xFF, 0x86, 0x60, 0x07};
    const auto parsedWck = OrpheusVendorCodec::ParseSignalSourceStatus(respWck, false);
    ASSERT_TRUE(parsedWck.has_value());
    EXPECT_EQ(*parsedWck, OrpheusSyncSource::WordClock);
}
