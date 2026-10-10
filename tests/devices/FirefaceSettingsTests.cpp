// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Fireface settings: flash block -> typed settings -> configuration register.
// Expected quadlets are worked out by hand from the bit table documented in
// FirefaceSettings.cpp, not produced by the code under test.

#include "Audio/Protocols/RME/FirefaceSettings.hpp"

#include <gtest/gtest.h>

namespace {

using namespace ASFW::Audio::RME;
namespace F = FlashField;

// A valid image: every field the encoder reads holds a defined value.
FlashSettings BaseImage(FirefaceModel model) {
    FlashSettings q{};
    q.fill(0);
    q[F::kSpdifInputMode] = 1;    // coaxial
    q[F::kClockMode] = 0;         // master
    q[F::kSyncReference] = 3;     // word clock
    q[F::kSpdifOutputMode] = 0;   // coaxial
    q[F::kInputLevel] = 0;        // low gain
    q[F::kOutputLevel] = 2;       // high gain
    q[F::kSampleRate] = 48000;
    if (model == FirefaceModel::kFF800) {
        q[F::kPlugSelect0] = 1;       // input 7 front
        q[F::kPlugSelect1] = 0;       // input 8 rear
        q[F::kInstrumentPlugSelect] = 1;  // input 1 front
    }
    return q;
}

ConfigWords Encode(FirefaceModel model, const FlashSettings& image) {
    const auto settings = DecodeFlashSettings(model, image);
    EXPECT_TRUE(settings.has_value());
    return settings ? EncodeConfig(model, *settings) : ConfigWords{};
}

TEST(FirefaceSettingsTests, FF800BaseImageEncodesToHandDerivedQuadlets) {
    const auto config = Encode(FirefaceModel::kFF800, BaseImage(FirefaceModel::kFF800));
    // q0: input low gain 0x8, output high gain 0x400.
    EXPECT_EQ(config[0], 0x00000408U);
    // q1: output high gain 0x10, input 7 front 0x20, input 8 rear 0x100,
    // input 1 front without filter 0x800, drive off 0x200.
    EXPECT_EQ(config[1], 0x00000B30U);
    // q2: master 0x1, word clock 0x1000, rates 0x1e, drop-and-stop 0x80000000.
    EXPECT_EQ(config[2], 0x8000101FU);
}

TEST(FirefaceSettingsTests, FF800PhantomPowerBitsPerInput) {
    const std::pair<uint32_t, uint32_t> cases[] = {
        {F::kPhantom0, 0x001U},  // input 7
        {F::kPhantom1, 0x080U},  // input 8
        {F::kPhantom2, 0x002U},  // input 9
        {F::kPhantom3, 0x100U},  // input 10
    };
    const uint32_t base = Encode(FirefaceModel::kFF800, BaseImage(FirefaceModel::kFF800))[0];
    for (const auto& [field, bit] : cases) {
        auto image = BaseImage(FirefaceModel::kFF800);
        image[field] = 1;
        EXPECT_EQ(Encode(FirefaceModel::kFF800, image)[0], base | bit) << "field " << field;
    }
}

TEST(FirefaceSettingsTests, FF400SettingsEncodeToHandDerivedQuadlets) {
    auto q = BaseImage(FirefaceModel::kFF400);
    q[F::kSpdifInputMode] = 0;       // optical
    q[F::kSpdifOutputEmphasis] = 1;
    q[F::kSpdifOutputProfessional] = 1;
    q[F::kClockMode] = 1;            // autosync
    q[F::kSpdifOutputNonAudio] = 1;
    q[F::kSyncReference] = 0;        // ADAT1
    q[F::kSpdifOutputMode] = 1;      // optical
    q[F::kInputLevel] = 2;           // +4 dBu
    q[F::kOutputLevel] = 0;          // -10 dBV
    q[F::kPlugSelect0] = 2;          // FF400: phones -10 dBV
    q[F::kPhantom0] = 1;             // input 1
    q[F::kPhantom2] = 1;             // FF400: input 3 pad
    q[F::kFuzz] = 1;                 // FF400: input 3 instrument
    q[F::kFilter] = 1;               // FF400: input 4 instrument
    q[F::kWordClockSingleSpeed] = 1;
    const auto config = Encode(FirefaceModel::kFF400, q);
    // q0: phantom 1 0x1, pad 3 0x100, phones -10 dBV 0x10000, input +4 dBu 0x10,
    // output -10 dBV 0x1000, instrument 4 0x4, instrument 3 0x200.
    EXPECT_EQ(config[0], 0x00011315U);
    // q1: input +4 dBu 0x2, output -10 dBV 0x8. Nothing else on an FF400.
    EXPECT_EQ(config[1], 0x0000000AU);
    // q2: S/PDIF pro 0x20, emphasis 0x40, non-audio 0x80, optical out 0x100,
    // optical in 0x200, word clock 1x 0x2000, rates 0x1e, drop-and-stop
    // 0x80000000, MIDI to address 0 0x04000000. Autosync: no master bit.
    EXPECT_EQ(config[2], 0x840023FEU);
}

TEST(FirefaceSettingsTests, WordClockSingleSpeedFollowsTheSetting) {
    auto image = BaseImage(FirefaceModel::kFF800);
    EXPECT_EQ(Encode(FirefaceModel::kFF800, image)[2] & 0x2000U, 0U)
        << "FFADO sets this bit unconditionally (assignment for comparison); the RME driver does not";
    image[F::kWordClockSingleSpeed] = 1;
    EXPECT_EQ(Encode(FirefaceModel::kFF800, image)[2] & 0x2000U, 0x2000U);
}

TEST(FirefaceSettingsTests, SyncReferenceBitsFollowTheVendorDriver) {
    const std::pair<uint32_t, uint32_t> cases[] = {
        {0, 0x0000U}, {1, 0x0400U}, {2, 0x0C00U}, {3, 0x1000U}, {4, 0x1400U}};
    for (const auto model : {FirefaceModel::kFF400, FirefaceModel::kFF800}) {
        for (const auto& [code, bits] : cases) {
            auto image = BaseImage(model);
            image[F::kSyncReference] = code;
            EXPECT_EQ(Encode(model, image)[2] & 0x1C00U, bits) << "code " << code;
        }
    }
}

TEST(FirefaceSettingsTests, FF800Input1JackAndSpeakerEmulation) {
    struct Case { uint32_t jack; uint32_t filter; uint32_t q1Bits; uint32_t q0Bit; };
    const Case cases[] = {
        {1, 0, 0x800, 0x0},  // front
        {1, 1, 0x400, 0x4},  // front, speaker emulation
        {0, 0, 0x004, 0x0},  // rear
        {2, 1, 0x404, 0x4},  // front and rear, speaker emulation
    };
    for (const auto& c : cases) {
        auto image = BaseImage(FirefaceModel::kFF800);
        image[F::kInstrumentPlugSelect] = c.jack;
        image[F::kFilter] = c.filter;
        const auto config = Encode(FirefaceModel::kFF800, image);
        EXPECT_EQ(config[1] & 0xC04U, c.q1Bits) << "jack " << c.jack << " filter " << c.filter;
        EXPECT_EQ(config[0] & 0x4U, c.q0Bit);
    }
}

TEST(FirefaceSettingsTests, FF800LimiterIsDisabledOnlyWithTheFrontInstrumentInput) {
    auto image = BaseImage(FirefaceModel::kFF800);
    image[F::kLimiterOff] = 1;
    EXPECT_EQ(Encode(FirefaceModel::kFF800, image)[2] & 0x10000U, 0x10000U);
    image[F::kInstrumentPlugSelect] = 0;  // rear
    EXPECT_EQ(Encode(FirefaceModel::kFF800, image)[2] & 0x10000U, 0U);
    image = BaseImage(FirefaceModel::kFF800);  // limiter on, front
    EXPECT_EQ(Encode(FirefaceModel::kFF800, image)[2] & 0x10000U, 0U);
}

TEST(FirefaceSettingsTests, FF800DriveSelectsTheFpgaOrTheCpldBit) {
    auto image = BaseImage(FirefaceModel::kFF800);
    image[F::kFuzz] = 1;
    const auto on = Encode(FirefaceModel::kFF800, image);
    EXPECT_EQ(on[0] & 0x200U, 0x200U);
    EXPECT_EQ(on[1] & 0x200U, 0U);
    const auto off = Encode(FirefaceModel::kFF800, BaseImage(FirefaceModel::kFF800));
    EXPECT_EQ(off[0] & 0x200U, 0U);
    EXPECT_EQ(off[1] & 0x200U, 0x200U);
}

TEST(FirefaceSettingsTests, FF400QuadletOneCarriesOnlyLevelBits) {
    auto image = BaseImage(FirefaceModel::kFF400);
    image[F::kFuzz] = 0;
    image[F::kFilter] = 0;
    image[F::kInstrumentPlugSelect] = 2;  // meaningless on an FF400; must not leak
    for (const uint32_t in : {0U, 1U, 2U}) {
        for (const uint32_t out : {0U, 1U, 2U}) {
            image[F::kInputLevel] = in;
            image[F::kOutputLevel] = out;
            EXPECT_EQ(Encode(FirefaceModel::kFF400, image)[1] & ~0x1BU, 0U);
        }
    }
}

TEST(FirefaceSettingsTests, LevelCodesMapToBothControlBits) {
    // Flash codes: input low 0 / +4 dBu 2 / -10 dBV 1; output high 2 / +4 dBu 1 / -10 dBV 0.
    struct Case { uint32_t field; uint32_t code; uint32_t q0; uint32_t q1; };
    const Case cases[] = {
        {F::kInputLevel, 0, 0x008, 0x0}, {F::kInputLevel, 2, 0x010, 0x2},
        {F::kInputLevel, 1, 0x020, 0x3},
        {F::kOutputLevel, 2, 0x400, 0x10}, {F::kOutputLevel, 1, 0x800, 0x18},
        {F::kOutputLevel, 0, 0x1000, 0x08},
    };
    for (const auto& c : cases) {
        auto image = BaseImage(FirefaceModel::kFF400);
        image[c.field] = c.code;
        const auto config = Encode(FirefaceModel::kFF400, image);
        const uint32_t q0Mask = c.field == F::kInputLevel ? 0x38U : 0x1C00U;
        const uint32_t q1Mask = c.field == F::kInputLevel ? 0x03U : 0x18U;
        EXPECT_EQ(config[0] & q0Mask, c.q0) << "field " << c.field << " code " << c.code;
        EXPECT_EQ(config[1] & q1Mask, c.q1) << "field " << c.field << " code " << c.code;
    }
}

TEST(FirefaceSettingsTests, DecodeRefusesUnsetOrUnknownValues) {
    struct Case { FirefaceModel model; uint32_t field; uint32_t value; };
    const Case cases[] = {
        {FirefaceModel::kFF800, F::kPhantom0, 0xffffffffU},  // not set in flash
        {FirefaceModel::kFF800, F::kPhantom3, 2U},           // phantom must be exactly 0 or 1
        {FirefaceModel::kFF400, F::kPhantom2, 7U},           // FF400 pad slot
        {FirefaceModel::kFF800, F::kSyncReference, 5U},
        {FirefaceModel::kFF800, F::kPlugSelect1, 3U},
        {FirefaceModel::kFF400, F::kPlugSelect0, 3U},        // FF400 phones level
        {FirefaceModel::kFF400, F::kInputLevel, 3U},
    };
    for (const auto& c : cases) {
        auto image = BaseImage(c.model);
        image[c.field] = c.value;
        const auto decoded = DecodeFlashSettings(c.model, image);
        ASSERT_FALSE(decoded.has_value()) << "field " << c.field;
        EXPECT_EQ(decoded.error().quadlet, c.field);
        EXPECT_EQ(decoded.error().value, c.value);
    }
}

TEST(FirefaceSettingsTests, FF400IgnoresFF800OnlyFields) {
    auto image = BaseImage(FirefaceModel::kFF400);
    image[F::kInstrumentPlugSelect] = 0xffffffffU;
    image[F::kPlugSelect1] = 0xffffffffU;
    image[F::kLimiterOff] = 0xffffffffU;
    EXPECT_TRUE(DecodeFlashSettings(FirefaceModel::kFF400, image).has_value());
}

TEST(FirefaceSettingsTests, ChannelLimitOutOfRangeMeansAllChannels) {
    // FFADO read_device_flash_settings coerces it (fireface_flash.cpp:294-297).
    auto image = BaseImage(FirefaceModel::kFF800);
    image[F::kChannelLimit] = 9;
    const auto decoded = DecodeFlashSettings(FirefaceModel::kFF800, image);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->channelLimit, ChannelLimit::kAllChannels);
    image[F::kChannelLimit] = 1;
    EXPECT_EQ(DecodeFlashSettings(FirefaceModel::kFF800, image)->channelLimit, ChannelLimit::kNoAdat2);
}

TEST(FirefaceSettingsTests, StatusComparisonMapsTheTimecodeReference) {
    auto image = BaseImage(FirefaceModel::kFF800);
    image[F::kSyncReference] = 4;  // TCO: config 0x1400, status 0x1800
    const auto config = Encode(FirefaceModel::kFF800, image);
    EXPECT_TRUE(CompareWithStatus(FirefaceModel::kFF800, config, 0x00001807U).Matches());
    EXPECT_FALSE(CompareWithStatus(FirefaceModel::kFF800, config, 0x00001407U).Matches());

    auto ff400 = BaseImage(FirefaceModel::kFF400);
    ff400[F::kSyncReference] = 4;  // LTC: 0x1400 in both registers
    EXPECT_TRUE(CompareWithStatus(FirefaceModel::kFF400,
                                  Encode(FirefaceModel::kFF400, ff400), 0x00001407U).Matches());
}

TEST(FirefaceSettingsTests, StatusComparisonCoversTheMirroredFieldsOnly) {
    const auto config = Encode(FirefaceModel::kFF800, BaseImage(FirefaceModel::kFF800));
    // Master + word clock in status; rate bits 0x1e and unrelated bits are ignored.
    EXPECT_TRUE(CompareWithStatus(FirefaceModel::kFF800, config, 0x00001007U).Matches());
    EXPECT_TRUE(CompareWithStatus(FirefaceModel::kFF800, config, 0x0040101FU).Matches());
    const auto emphasis = CompareWithStatus(FirefaceModel::kFF800, config, 0x00001047U);
    EXPECT_FALSE(emphasis.Matches());
    EXPECT_EQ(emphasis.expected ^ emphasis.reported, 0x40U);
}


// A real FF800 settings block, read by the driver from 0x3_000f0000 in a field
// log (2026-10-10). Non-zero: q4 q9 q29 q30 q36 q38 q43.
FlashSettings FieldFF800Image() {
    FlashSettings q{};
    q.fill(0);
    q[4] = 0x00000001U;
    q[9] = 0x00000003U;
    q[29] = 0x00000002U;
    q[30] = 0x00000001U;
    q[36] = 0x00000001U;
    q[38] = 0x00000001U;
    q[43] = 0x0000ac44U;
    return q;
}

// The inverse of the decoder, written from FFADO write_device_flash_settings
// (fireface_flash.cpp): settings copy back to their quadlets with the flash
// codes of fireface_def.h:409-441, everything else stays zero (memset). Test
// only: the driver never writes the flash.
FlashSettings FfadoFlashFromSettings(const FirefaceSettings& s) {
    FlashSettings q{};
    q.fill(0);
    q[F::kPhantom0] = s.phantom[0];
    q[F::kPhantom1] = s.phantom[1];
    q[F::kPhantom2] = s.phantom[2];
    q[F::kPhantom3] = s.phantom[3];
    q[F::kSpdifInputMode] = s.spdifInputOptical ? 0 : 1;          // COAX 1, OPTICAL 0
    q[F::kSpdifOutputEmphasis] = s.spdifOutputEmphasis;
    q[F::kSpdifOutputProfessional] = s.spdifOutputProfessional;
    q[F::kClockMode] = s.clockMaster ? 0 : 1;                     // MASTER 0, AUTOSYNC 1
    q[F::kSpdifOutputNonAudio] = s.spdifOutputNonAudio;
    q[F::kSyncReference] = static_cast<uint32_t>(s.syncReference);  // ADAT1 0 .. WORDCLOCK 3, TCO 4
    q[F::kSpdifOutputMode] = s.spdifOutputOptical ? 1 : 0;
    q[F::kChannelLimit] = static_cast<uint32_t>(s.channelLimit);
    q[F::kInputLevel] = s.inputLevel == InputLevel::kLowGain ? 0 : s.inputLevel == InputLevel::kPlus4dBu ? 2 : 1;
    q[F::kOutputLevel] = s.outputLevel == OutputLevel::kHighGain ? 2 : s.outputLevel == OutputLevel::kPlus4dBu ? 1 : 0;
    q[F::kFilter] = s.ff800SpeakerEmulation;
    q[F::kFuzz] = s.ff800Drive;
    // p12db_an[0]: set only when the limiter is off and input 1 is the front jack.
    q[F::kLimiterOff] = (!s.ff800Limiter && s.ff800Input1 == JackSelect::kFront) ? 1 : 0;
    q[F::kSampleRate] = s.sampleRateHz;
    q[F::kWordClockSingleSpeed] = s.wordClockSingleSpeed;
    const auto plug = [](JackSelect j) { return j == JackSelect::kRear ? 0U : j == JackSelect::kFront ? 1U : 2U; };
    q[F::kInstrumentPlugSelect] = plug(s.ff800Input1);
    q[F::kPlugSelect0] = plug(s.ff800Input7);
    q[F::kPlugSelect1] = plug(s.ff800Input8);
    return q;
}

TEST(FirefaceSettingsTests, FieldFF800ImageDecodesToItsSavedSettings) {
    // Expected values read off the FFADO flash codes, not the decoder.
    const auto s = DecodeFlashSettings(FirefaceModel::kFF800, FieldFF800Image());
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->clockMaster);                                   // q7 0
    EXPECT_EQ(s->syncReference, SyncReference::kWordClock);        // q9 3
    EXPECT_FALSE(s->spdifInputOptical);                            // q4 1 = coaxial
    EXPECT_FALSE(s->spdifOutputOptical);                           // q10 0 = coaxial
    EXPECT_EQ(s->inputLevel, InputLevel::kPlus4dBu);               // q29 2
    EXPECT_EQ(s->outputLevel, OutputLevel::kPlus4dBu);             // q30 1
    EXPECT_EQ(s->phantom, (std::array<bool, 4>{false, false, false, true}));  // q36: input 10
    EXPECT_TRUE(s->ff800SpeakerEmulation);                         // q38 1
    EXPECT_FALSE(s->ff800Drive);                                   // q39 0
    EXPECT_TRUE(s->ff800Limiter);                                  // q49 0
    EXPECT_EQ(s->ff800Input1, JackSelect::kRear);                  // q37 0
    EXPECT_EQ(s->ff800Input7, JackSelect::kRear);                  // q31 0
    EXPECT_EQ(s->ff800Input8, JackSelect::kRear);                  // q32 0
    EXPECT_FALSE(s->wordClockSingleSpeed);                         // q46 0
    EXPECT_EQ(s->sampleRateHz, 44100U);                            // q43 0xac44
}

TEST(FirefaceSettingsTests, FieldFF800ImageRoundTripsThroughFfadoWriteRules) {
    // Every quadlet must come back: a mismatch is a field the decoder drops or
    // a value code it reads differently from FFADO.
    const auto image = FieldFF800Image();
    const auto s = DecodeFlashSettings(FirefaceModel::kFF800, image);
    ASSERT_TRUE(s.has_value());
    const auto back = FfadoFlashFromSettings(*s);
    for (uint32_t i = 0; i < kFlashSettingsQuadlets; ++i)
        EXPECT_EQ(back[i], image[i]) << "flash q" << i;
}

TEST(FirefaceSettingsTests, FieldFF800ImageConfigMatchesFfadoBitTable) {
    // Worked out from FFADO set_hardware_params and fireface_def.h:153-264:
    // q0: phantom 10 CR0_BIT08 0x100 | input +4 dBu FPGA_CTRL1 0x10 |
    //     output +4 dBu FPGA_CTRL_1 0x800 | filter CR0_BIT02 0x4         = 0x914
    // q1: input +4 dBu CPLD 0x2 | output +4 dBu CPLD 0x18 | input 7 rear 0x40 |
    //     input 8 rear 0x100 | input 1 rear 0x4 | drive off 0x200        = 0x35e
    // q2: master 0x1 | word clock REF2 0x1000 | rates 0x1e | drop-and-stop
    //     0x80000000 = 0x8000101f. FFADO itself would add 0x2000 (its
    //     word_clock_single_speed '=' bug); the card reports 0x2000 clear.
    const auto config = Encode(FirefaceModel::kFF800, FieldFF800Image());
    EXPECT_EQ(config, (ConfigWords{0x00000914U, 0x0000035eU, 0x8000101fU}));
}

TEST(FirefaceSettingsTests, FieldFF800StatusMirrorMatchesBeforeAndAfterInit) {
    const auto config = Encode(FirefaceModel::kFF800, FieldFF800Image());
    for (const uint32_t status1 : {0x88001001U, 0x88001007U, 0xa8001007U})
        EXPECT_TRUE(CompareWithStatus(FirefaceModel::kFF800, config, status1).Matches()) << std::hex << status1;
}


// Status goldens: the card's own words from the same field log. Expected values
// follow FFADO fireface_def.h:270-351 and Linux dump_sync_status
// (ff-protocol-former.c:159-255); the front-panel LEDs have not checked them yet.
TEST(FirefaceStatusTests, FieldFF800StatusBeforeInit) {
    const auto d = DecodeStatus(0x040014b0U, 0x88001001U);
    EXPECT_EQ(d.configured, ClockSource::kInternal);
    EXPECT_EQ(d.configuredRateHz, 44100U);          // the saved rate, before our init
    EXPECT_EQ(d.syncReference, ClockSource::kAdat1);  // bits 24:22 = 0; unused by a master
    EXPECT_EQ(d.syncRateHz, 44100U);                // bits 28:25 = 0x04000000
    EXPECT_EQ(d.wordClock, LockState::kNone);
    EXPECT_EQ(d.spdif, LockState::kNone);
    EXPECT_EQ(d.adat1, LockState::kSync);           // 0x400 lock + 0x1000 sync
    EXPECT_EQ(d.adat2, LockState::kNone);
}

TEST(FirefaceStatusTests, FieldFF800StatusAfterInit) {
    for (const uint32_t status1 : {0x88001007U, 0xa8001007U}) {
        const auto d = DecodeStatus(0x060014c0U, status1);
        EXPECT_EQ(d.configured, ClockSource::kInternal) << std::hex << status1;
        EXPECT_EQ(d.configuredRateHz, 48000U);
        EXPECT_EQ(d.syncRateHz, 48000U);
        EXPECT_EQ(d.adat1, LockState::kSync);
        EXPECT_EQ(d.wordClock, LockState::kNone);
    }
}

TEST(FirefaceStatusTests, ConfiguredSourceComesFromBit0ThenBits12To10) {
    EXPECT_EQ(DecodeStatus(0, 0x1001U).configured, ClockSource::kInternal);  // bit 0 wins
    EXPECT_EQ(DecodeStatus(0, 0x0000U).configured, ClockSource::kAdat1);
    EXPECT_EQ(DecodeStatus(0, 0x0400U).configured, ClockSource::kAdat2);
    EXPECT_EQ(DecodeStatus(0, 0x0c00U).configured, ClockSource::kSpdif);
    EXPECT_EQ(DecodeStatus(0, 0x1000U).configured, ClockSource::kWordClock);
    EXPECT_EQ(DecodeStatus(0, 0x1800U).configured, ClockSource::kTimecode);
    EXPECT_EQ(DecodeStatus(0, 0x1400U).configured, ClockSource::kUnknown);  // no such status code
    EXPECT_EQ(DecodeStatus(0, 0x0006U).configuredRateHz, 48000U);
    EXPECT_EQ(DecodeStatus(0, 0x001eU).configuredRateHz, 0U);
}

TEST(FirefaceStatusTests, SyncReferenceAndInputLocks) {
    EXPECT_EQ(DecodeStatus(0x00c00000U, 0).syncReference, ClockSource::kSpdif);
    EXPECT_EQ(DecodeStatus(0x01000000U, 0).syncReference, ClockSource::kWordClock);
    EXPECT_EQ(DecodeStatus(0x01400000U, 0).syncReference, ClockSource::kTimecode);
    EXPECT_EQ(DecodeStatus(0x01800000U, 0).syncReference, ClockSource::kNone);
    EXPECT_EQ(DecodeStatus(0x01c00000U, 0).syncReference, ClockSource::kUnknown);
    EXPECT_EQ(DecodeStatus(0x40000000U, 0).wordClock, LockState::kLock);
    EXPECT_EQ(DecodeStatus(0x60000000U, 0).wordClock, LockState::kSync);
    EXPECT_EQ(DecodeStatus(0x20000000U, 0).wordClock, LockState::kNone);  // sync without lock
    EXPECT_EQ(DecodeStatus(0x00100000U, 0).spdif, LockState::kLock);
    EXPECT_EQ(DecodeStatus(0x00140000U, 0).spdif, LockState::kSync);
    EXPECT_EQ(DecodeStatus(0x00002800U, 0).adat2, LockState::kSync);
}

} // namespace
