// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FirefaceSettings.cpp - see FirefaceSettings.hpp.
//
// Bit sources. Three references describe the configuration register:
//   RME   RME 3.41 Fireface_InitHardware (FFAD-431 0x6728), one code path for
//         both models, fed by the Settings app's fields.
//   FFADO libffado-2.5.0 rme/fireface_hw.cpp set_hardware_params, constants in
//         rme/fireface_def.h:153-266; flash layout and codes fireface_def.h:409-
//         510, flash-to-settings mapping fireface_flash.cpp:296-369.
//   ALSA  snd-firewire-ctl-services protocols/fireface/src/former/ff400.rs:768-
//         821 and ff800.rs:520-563 (serializers ff800.rs:601-820).
// Where they disagree, the comment at the constant says which one is followed
// and why. The flash layout itself has one source, FFADO.

#include "FirefaceSettings.hpp"

#include <optional>
#include <span>

namespace ASFW::Audio::RME {
namespace {

namespace Q0 {
// Phantom power. FFADO CR0_PHANTOM_MIC0/1 and CR0_FF800_PHANTOM_MIC9/10; ALSA
// ff800 Q0_INPUT_{6,7,8,9}_POWERING, ff400 Q0_INPUT_{0,1}_POWERING; RME sets
// 0x1/0x80/0x2/0x100 from its phantom fields. All three agree.
inline constexpr uint32_t kPhantom0 = 0x00000001;  // FF800 input 7, FF400 input 1
inline constexpr uint32_t kPhantom1 = 0x00000080;  // FF800 input 8, FF400 input 2
inline constexpr uint32_t kFF800Phantom9 = 0x00000002;
inline constexpr uint32_t kFF800Phantom10 = 0x00000100;
// The FF400 uses the same two bits for its pads (FFADO CR0_FF400_CH3/4_PAD,
// ALSA ff400 Q0_INPUT_{2,3}_PAD_MASK).
inline constexpr uint32_t kFF400Pad3 = 0x00000100;
inline constexpr uint32_t kFF400Pad4 = 0x00000002;
// FFADO CR0_FF800_FILTER_FPGA / CR0_FF400_CH4_INSTR; ALSA ff800
// Q0_INPUT_0_INST_SPKR_EMU_MASK, ff400 Q0_INPUT_3_INST_MASK.
inline constexpr uint32_t kFF800SpeakerEmulation = 0x00000004;
inline constexpr uint32_t kFF400Instrument4 = 0x00000004;
// FFADO CR0_FF800_DRIVE_FPGA / CR0_FF400_CH3_INSTR; ALSA ff400
// Q0_INPUT_2_INST_MASK.
inline constexpr uint32_t kFF800DriveOn = 0x00000200;
inline constexpr uint32_t kFF400Instrument3 = 0x00000200;
// Input level. RME and FFADO pair low gain with 0x8 and +4 dBu with 0x10;
// ALSA ff400 labels those two the other way round (Q0_LINE_IN_LEVEL_LOW/PRO).
// Followed: RME + FFADO, which also agree on the matching quadlet-1 bits.
inline constexpr uint32_t kInputLowGain = 0x00000008;
inline constexpr uint32_t kInputPlus4dBu = 0x00000010;
inline constexpr uint32_t kInputMinus10dBV = 0x00000020;
// Output level. All three agree (FFADO CR0_OLEVEL_FPGA_*, ALSA Q0_LINE_OUT_*).
inline constexpr uint32_t kOutputHighGain = 0x00000400;
inline constexpr uint32_t kOutputPlus4dBu = 0x00000800;
inline constexpr uint32_t kOutputMinus10dBV = 0x00001000;
// FF400 phones level. RME (0x20000 / 0x10000) and FFADO CRO_PHLEVEL_* agree;
// ALSA ff400 Q0_HP_OUT_LEVEL_* uses 0x40000 / 0x20000. Followed: RME + FFADO.
// FFADO writes these for the FF400 only; the FF800 has no phones field in flash.
inline constexpr uint32_t kPhonesHighGain = 0x00020000;
inline constexpr uint32_t kPhonesPlus4dBu = 0x00000000;
inline constexpr uint32_t kPhonesMinus10dBV = 0x00010000;
} // namespace Q0

namespace Q1 {
// Level control bits (CPLD side). All three agree.
inline constexpr uint32_t kInputLowGain = 0x00000000;
inline constexpr uint32_t kInputPlus4dBu = 0x00000002;
inline constexpr uint32_t kInputMinus10dBV = 0x00000003;
inline constexpr uint32_t kOutputHighGain = 0x00000010;
inline constexpr uint32_t kOutputPlus4dBu = 0x00000018;
inline constexpr uint32_t kOutputMinus10dBV = 0x00000008;
// FF800 jacks. FFADO CR1_INPUT_OPT{0,1,2}_*; ALSA ff800 Q1_INPUT_{0,6,7}_*_JACK;
// RME sets the same bits. FF800 only: on the FF400 RME's shared code also sets
// 0x4/0x40/0x100 and the drive bit below from Settings-app fields, but FFADO
// and ALSA set none of them and the FF400 has no such jacks. Followed: FFADO +
// ALSA, so an FF400 quadlet 1 carries level bits only.
inline constexpr uint32_t kInput7Front = 0x00000020;
inline constexpr uint32_t kInput7Rear = 0x00000040;
inline constexpr uint32_t kInput8Front = 0x00000080;
inline constexpr uint32_t kInput8Rear = 0x00000100;
inline constexpr uint32_t kInput1Rear = 0x00000004;
inline constexpr uint32_t kInput1Front = 0x00000800;
inline constexpr uint32_t kInput1FrontWithSpeakerEmulation = 0x00000400;
// FF800 drive: on sets Q0 0x200, off sets this CPLD bit (RME and FFADO
// CR1_INSTR_DRIVE). ALSA sets both bits when on and neither when off.
// Followed: RME + FFADO.
inline constexpr uint32_t kFF800DriveOff = 0x00000200;
} // namespace Q1

namespace Q2 {
inline constexpr uint32_t kClockMaster = 0x00000001;        // all three
inline constexpr uint32_t kAvailableRates = 0x0000001e;     // FFADO CR2_FREQ0|FREQ1|DSPEED|QSSPEED, RME |0x1e; always set
inline constexpr uint32_t kSpdifOutProfessional = 0x00000020;
inline constexpr uint32_t kSpdifOutEmphasis = 0x00000040;
inline constexpr uint32_t kSpdifOutNonAudio = 0x00000080;
inline constexpr uint32_t kSpdifOutOptical = 0x00000100;    // S/PDIF out on the ADAT2 port
inline constexpr uint32_t kSpdifInOptical = 0x00000200;
// Sync reference, bits 12:10. RME (switch on its sync field) and FFADO
// CR2_SYNC_* (fireface_def.h:246-265) agree for both models: ADAT1 0, ADAT2
// 0x400, S/PDIF 0xc00, word clock 0x1000, TCO/LTC 0x1400; so does ALSA ff400
// (ff400.rs:807-812). ALSA ff800 (ff800.rs:557-563) has word clock 0x1400 and
// TCO 0x1c00, matching none of the others nor its own FF400 table. Followed:
// RME + FFADO + ALSA ff400.
inline constexpr uint32_t kSyncReferenceMask = 0x00001c00;
inline constexpr uint32_t kSyncAdat1 = 0x00000000;
inline constexpr uint32_t kSyncAdat2 = 0x00000400;
inline constexpr uint32_t kSyncSpdif = 0x00000c00;
inline constexpr uint32_t kSyncWordClock = 0x00001000;
inline constexpr uint32_t kSyncTimecode = 0x00001400;
// Word clock out at single speed. RME and ALSA follow the setting. FFADO sets
// it always: `word_clock_single_speed=FF_SWPARAM_WORD_CLOCK_1x` assigns where
// it should compare. Followed: RME + ALSA.
inline constexpr uint32_t kWordClockSingleSpeed = 0x00002000;
// FF800 limiter off: RME and FFADO set it only when the limiter is off and
// input 1 is the front jack alone. ALSA ff800 sets the bit for limiter *on*.
// Followed: RME + FFADO.
inline constexpr uint32_t kFF800LimiterOff = 0x00010000;
// Always set by RME and FFADO (CR2_DROP_AND_STOP; ALSA "continue at errors").
inline constexpr uint32_t kDropAndStop = 0x80000000;
// FF400: MIDI from the device to host address offset 0 (RME |0x04000000;
// FFADO CR2_FF400_SELECT_MIDI_TX_ADDR_1; ALSA Q2_MIDI_TX_LOW_OFFSET_0000).
inline constexpr uint32_t kFF400MidiTxOffset0 = 0x04000000;
} // namespace Q2

// The status quadlet at 0x801c0004 mirrors part of quadlet 2. Same bits except
// the timecode reference: TCO reads back as 0x1800 on the FF800 (Linux
// ff-protocol-former.c:40, ALSA ff800 Q1_CONF_CLK_SRC_TCO_FLAGS); the FF400's
// LTC reads 0x1400 (ALSA ff400 Q1_CONF_CLK_SRC_LTC_FLAG). Non-audio has no
// status bit.
inline constexpr uint32_t kStatusMirrorMask =
    Q2::kClockMaster | Q2::kSpdifOutProfessional | Q2::kSpdifOutEmphasis |
    Q2::kSpdifOutOptical | Q2::kSpdifInOptical | Q2::kWordClockSingleSpeed |
    Q2::kSyncReferenceMask;
inline constexpr uint32_t kFF800StatusTimecode = 0x00001800;

// Flash value codes (FFADO fireface_def.h:409-430).
inline constexpr uint32_t kUnset = 0xffffffff;

// Every field the encoder reads, with the largest code it may hold. A field
// outside its range, or unset, refuses the whole decode: no guessing, phantom
// power above all. Codes: FFADO fireface_def.h:409-441.
struct FieldRange {
    uint32_t field;
    uint32_t highest;
};
inline constexpr FieldRange kCommonFields[] = {
    {FlashField::kSpdifInputMode, 1},           // 1 coaxial, 0 optical
    {FlashField::kSpdifOutputEmphasis, 1},
    {FlashField::kSpdifOutputProfessional, 1},
    {FlashField::kClockMode, 1},                // 0 master, 1 autosync
    {FlashField::kSpdifOutputNonAudio, 1},
    {FlashField::kSyncReference, 4},            // ADAT1, ADAT2, S/PDIF, word clock, TCO/LTC
    {FlashField::kSpdifOutputMode, 1},          // 0 coaxial, 1 optical
    {FlashField::kInputLevel, 2},               // 0 low gain, 2 +4 dBu, 1 -10 dBV
    {FlashField::kOutputLevel, 2},              // 2 high gain, 1 +4 dBu, 0 -10 dBV
    {FlashField::kWordClockSingleSpeed, 1},
    {FlashField::kPhantom0, 1},
    {FlashField::kPhantom1, 1},
    {FlashField::kPhantom2, 1},
    {FlashField::kPhantom3, 1},
    {FlashField::kFilter, 1},
    {FlashField::kFuzz, 1},
};
inline constexpr FieldRange kFF800Fields[] = {
    {FlashField::kPlugSelect0, 2},              // 0 rear, 1 front, 2 front and rear
    {FlashField::kPlugSelect1, 2},
    {FlashField::kInstrumentPlugSelect, 2},
    {FlashField::kLimiterOff, 1},
};
inline constexpr FieldRange kFF400Fields[] = {
    {FlashField::kPlugSelect0, 2},              // FF400: phones 0 high gain, 1 +4 dBu, 2 -10 dBV
};

std::optional<FlashDecodeError> FirstInvalid(const FlashSettings& flash,
                                             std::span<const FieldRange> ranges) noexcept {
    for (const auto& range : ranges) {
        const uint32_t value = flash[range.field];
        if (value == kUnset || value > range.highest) return FlashDecodeError{range.field, value};
    }
    return std::nullopt;
}

JackSelect Jack(uint32_t code) noexcept {
    return code == 0 ? JackSelect::kRear : code == 1 ? JackSelect::kFront : JackSelect::kFrontAndRear;
}

} // namespace

std::expected<FirefaceSettings, FlashDecodeError>
DecodeFlashSettings(FirefaceModel model, const FlashSettings& flash) noexcept {
    namespace F = FlashField;
    const bool ff800 = model == FirefaceModel::kFF800;
    if (const auto invalid = FirstInvalid(flash, kCommonFields)) return std::unexpected(*invalid);
    if (const auto invalid = FirstInvalid(flash, ff800 ? std::span<const FieldRange>(kFF800Fields)
                                                       : std::span<const FieldRange>(kFF400Fields)))
        return std::unexpected(*invalid);

    FirefaceSettings s{};
    s.spdifInputOptical = flash[F::kSpdifInputMode] == 0;
    s.spdifOutputEmphasis = flash[F::kSpdifOutputEmphasis] != 0;
    s.spdifOutputProfessional = flash[F::kSpdifOutputProfessional] != 0;
    s.clockMaster = flash[F::kClockMode] == 0;
    s.spdifOutputNonAudio = flash[F::kSpdifOutputNonAudio] != 0;
    s.syncReference = static_cast<SyncReference>(flash[F::kSyncReference]);
    s.spdifOutputOptical = flash[F::kSpdifOutputMode] != 0;
    const uint32_t in = flash[F::kInputLevel];
    s.inputLevel = in == 0 ? InputLevel::kLowGain : in == 2 ? InputLevel::kPlus4dBu : InputLevel::kMinus10dBV;
    const uint32_t out = flash[F::kOutputLevel];
    s.outputLevel = out == 2 ? OutputLevel::kHighGain : out == 1 ? OutputLevel::kPlus4dBu : OutputLevel::kMinus10dBV;
    s.wordClockSingleSpeed = flash[F::kWordClockSingleSpeed] != 0;
    s.phantom[0] = flash[F::kPhantom0] != 0;
    s.phantom[1] = flash[F::kPhantom1] != 0;
    if (ff800) {
        s.phantom[2] = flash[F::kPhantom2] != 0;
        s.phantom[3] = flash[F::kPhantom3] != 0;
        s.ff800Input7 = Jack(flash[F::kPlugSelect0]);
        s.ff800Input8 = Jack(flash[F::kPlugSelect1]);
        s.ff800Input1 = Jack(flash[F::kInstrumentPlugSelect]);
        s.ff800SpeakerEmulation = flash[F::kFilter] != 0;
        s.ff800Drive = flash[F::kFuzz] != 0;
        s.ff800Limiter = flash[F::kLimiterOff] == 0;  // FFADO: limiter = (p12db_an[0] == 0)
    } else {
        // FF400 reuse (fireface_flash.cpp:303-345): phantom slots 2-3 are the
        // input 3/4 pads, fuzz/filter the input 3/4 instrument switches, plug
        // select 0 the phones level.
        s.ff400Pad = {flash[F::kPhantom2] != 0, flash[F::kPhantom3] != 0};
        s.ff400Instrument = {flash[F::kFuzz] != 0, flash[F::kFilter] != 0};
        s.ff400Phones = static_cast<PhonesLevel>(flash[F::kPlugSelect0]);
    }
    // A saved host preference; FFADO coerces unknown values to "all channels"
    // (fireface_flash.cpp:294-297).
    const uint32_t limit = flash[F::kChannelLimit];
    s.channelLimit = limit <= 3 ? static_cast<ChannelLimit>(limit) : ChannelLimit::kAllChannels;
    s.sampleRateHz = flash[F::kSampleRate];
    return s;
}

ConfigWords EncodeConfig(FirefaceModel model, const FirefaceSettings& s) noexcept {
    const bool ff800 = model == FirefaceModel::kFF800;
    ConfigWords q{};

    if (s.phantom[0]) q[0] |= Q0::kPhantom0;
    if (s.phantom[1]) q[0] |= Q0::kPhantom1;
    switch (s.inputLevel) {
        case InputLevel::kLowGain:    q[0] |= Q0::kInputLowGain;    q[1] |= Q1::kInputLowGain; break;
        case InputLevel::kPlus4dBu:   q[0] |= Q0::kInputPlus4dBu;   q[1] |= Q1::kInputPlus4dBu; break;
        case InputLevel::kMinus10dBV: q[0] |= Q0::kInputMinus10dBV; q[1] |= Q1::kInputMinus10dBV; break;
    }
    switch (s.outputLevel) {
        case OutputLevel::kHighGain:   q[0] |= Q0::kOutputHighGain;   q[1] |= Q1::kOutputHighGain; break;
        case OutputLevel::kPlus4dBu:   q[0] |= Q0::kOutputPlus4dBu;   q[1] |= Q1::kOutputPlus4dBu; break;
        case OutputLevel::kMinus10dBV: q[0] |= Q0::kOutputMinus10dBV; q[1] |= Q1::kOutputMinus10dBV; break;
    }

    if (ff800) {
        if (s.phantom[2]) q[0] |= Q0::kFF800Phantom9;
        if (s.phantom[3]) q[0] |= Q0::kFF800Phantom10;
        if (s.ff800SpeakerEmulation) q[0] |= Q0::kFF800SpeakerEmulation;
        if (s.ff800Drive) q[0] |= Q0::kFF800DriveOn;
        else q[1] |= Q1::kFF800DriveOff;
        const auto jacks = [&q](JackSelect jack, uint32_t front, uint32_t rear) {
            if (jack != JackSelect::kRear) q[1] |= front;
            if (jack != JackSelect::kFront) q[1] |= rear;
        };
        jacks(s.ff800Input7, Q1::kInput7Front, Q1::kInput7Rear);
        jacks(s.ff800Input8, Q1::kInput8Front, Q1::kInput8Rear);
        jacks(s.ff800Input1,
              s.ff800SpeakerEmulation ? Q1::kInput1FrontWithSpeakerEmulation : Q1::kInput1Front,
              Q1::kInput1Rear);
        if (!s.ff800Limiter && s.ff800Input1 == JackSelect::kFront) q[2] |= Q2::kFF800LimiterOff;
    } else {
        if (s.ff400Pad[0]) q[0] |= Q0::kFF400Pad3;
        if (s.ff400Pad[1]) q[0] |= Q0::kFF400Pad4;
        if (s.ff400Instrument[0]) q[0] |= Q0::kFF400Instrument3;
        if (s.ff400Instrument[1]) q[0] |= Q0::kFF400Instrument4;
        switch (s.ff400Phones) {
            case PhonesLevel::kHighGain:   q[0] |= Q0::kPhonesHighGain; break;
            case PhonesLevel::kPlus4dBu:   q[0] |= Q0::kPhonesPlus4dBu; break;
            case PhonesLevel::kMinus10dBV: q[0] |= Q0::kPhonesMinus10dBV; break;
        }
        q[2] |= Q2::kFF400MidiTxOffset0;
    }

    if (s.clockMaster) q[2] |= Q2::kClockMaster;
    if (s.spdifOutputProfessional) q[2] |= Q2::kSpdifOutProfessional;
    if (s.spdifOutputEmphasis) q[2] |= Q2::kSpdifOutEmphasis;
    if (s.spdifOutputNonAudio) q[2] |= Q2::kSpdifOutNonAudio;
    if (s.spdifOutputOptical) q[2] |= Q2::kSpdifOutOptical;
    if (s.spdifInputOptical) q[2] |= Q2::kSpdifInOptical;
    if (s.wordClockSingleSpeed) q[2] |= Q2::kWordClockSingleSpeed;
    switch (s.syncReference) {
        case SyncReference::kAdat1:     q[2] |= Q2::kSyncAdat1; break;
        case SyncReference::kAdat2:     q[2] |= Q2::kSyncAdat2; break;
        case SyncReference::kSpdif:     q[2] |= Q2::kSyncSpdif; break;
        case SyncReference::kWordClock: q[2] |= Q2::kSyncWordClock; break;
        case SyncReference::kTimecode:  q[2] |= Q2::kSyncTimecode; break;
    }
    q[2] |= Q2::kAvailableRates | Q2::kDropAndStop;
    return q;
}

StatusComparison CompareWithStatus(FirefaceModel model, const ConfigWords& config,
                                   uint32_t status1) noexcept {
    uint32_t expected = config[2] & kStatusMirrorMask;
    if (model == FirefaceModel::kFF800 &&
        (expected & Q2::kSyncReferenceMask) == Q2::kSyncTimecode) {
        expected = (expected & ~Q2::kSyncReferenceMask) | kFF800StatusTimecode;
    }
    return {.expected = expected, .reported = status1 & kStatusMirrorMask, .mask = kStatusMirrorMask};
}

namespace {
// Rate codes: status1 bits 4:1 (Linux parse_clock_bits, ff-protocol-former.c:20-30).
uint32_t ConfiguredRate(uint32_t status1) noexcept {
    switch (status1 & 0x1eU) {
        case 0x02U: return 32000;  case 0x00U: return 44100;  case 0x06U: return 48000;
        case 0x0aU: return 64000;  case 0x08U: return 88200;  case 0x0eU: return 96000;
        case 0x12U: return 128000; case 0x10U: return 176400; case 0x16U: return 192000;
        default: return 0;
    }
}
// status0 bits 28:25: FFADO SR0_AUTOSYNC_FREQ_* (fireface_def.h:331-340), Linux
// dump_sync_status rate_entries. Code n in 1..9 is 32k, 44.1k, 48k, ... 192k.
uint32_t SyncRate(uint32_t status0) noexcept {
    constexpr uint32_t kRates[] = {32000, 44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000};
    const uint32_t code = (status0 >> 25) & 0xfU;
    return code >= 1 && code <= 9 ? kRates[code - 1] : 0;
}
// status1 bits 12:10 when bit 0 is clear (Linux parse_clock_bits, :31-40). The
// FF800 timecode module reads 0x1800 here, though its configuration code is 0x1400.
ClockSource ConfiguredSource(uint32_t status1) noexcept {
    if (status1 & 0x1U) return ClockSource::kInternal;
    switch (status1 & 0x1c00U) {
        case 0x0000U: return ClockSource::kAdat1;
        case 0x0400U: return ClockSource::kAdat2;
        case 0x0c00U: return ClockSource::kSpdif;
        case 0x1000U: return ClockSource::kWordClock;
        case 0x1800U: return ClockSource::kTimecode;
        default: return ClockSource::kUnknown;
    }
}
// status0 bits 24:22: FFADO SR0_AUTOSYNC_SRC_* (fireface_def.h:323-329).
ClockSource SyncReferenceSource(uint32_t status0) noexcept {
    switch (status0 & 0x01c00000U) {
        case 0x00000000U: return ClockSource::kAdat1;
        case 0x00400000U: return ClockSource::kAdat2;
        case 0x00c00000U: return ClockSource::kSpdif;
        case 0x01000000U: return ClockSource::kWordClock;
        case 0x01400000U: return ClockSource::kTimecode;
        case 0x01800000U: return ClockSource::kNone;
        default: return ClockSource::kUnknown;
    }
}
// Lock and sync bits: FFADO SR0_*_LOCK/_SYNC (fireface_def.h:270-289). Linux
// dump_sync_status agrees except for S/PDIF lock, which it reads at 0x00080000
// (FFADO: SR0_OVER). Followed: FFADO.
LockState Lock(uint32_t status0, uint32_t lock, uint32_t sync) noexcept {
    if ((status0 & lock) == 0) return LockState::kNone;
    return (status0 & sync) != 0 ? LockState::kSync : LockState::kLock;
}
} // namespace

DecodedStatus DecodeStatus(uint32_t status0, uint32_t status1) noexcept {
    return DecodedStatus{
        .configured = ConfiguredSource(status1),
        .configuredRateHz = ConfiguredRate(status1),
        .syncReference = SyncReferenceSource(status0),
        .syncRateHz = SyncRate(status0),
        .wordClock = Lock(status0, 0x40000000U, 0x20000000U),
        .spdif = Lock(status0, 0x00100000U, 0x00040000U),
        .adat1 = Lock(status0, 0x00000400U, 0x00001000U),
        .adat2 = Lock(status0, 0x00000800U, 0x00002000U),
    };
}

const char* Name(ClockSource source) noexcept {
    switch (source) {
        case ClockSource::kInternal: return "internal";
        case ClockSource::kAdat1: return "adat1";
        case ClockSource::kAdat2: return "adat2";
        case ClockSource::kSpdif: return "spdif";
        case ClockSource::kWordClock: return "word-clock";
        case ClockSource::kTimecode: return "timecode";
        case ClockSource::kNone: return "none";
        case ClockSource::kUnknown: return "unknown";
    }
    return "?";
}
const char* Name(LockState state) noexcept {
    switch (state) {
        case LockState::kNone: return "none";
        case LockState::kLock: return "lock";
        case LockState::kSync: return "sync";
    }
    return "?";
}
const char* Name(InputLevel level) noexcept {
    switch (level) {
        case InputLevel::kLowGain: return "low-gain";
        case InputLevel::kPlus4dBu: return "+4dBu";
        case InputLevel::kMinus10dBV: return "-10dBV";
    }
    return "?";
}
const char* Name(OutputLevel level) noexcept {
    switch (level) {
        case OutputLevel::kHighGain: return "high-gain";
        case OutputLevel::kPlus4dBu: return "+4dBu";
        case OutputLevel::kMinus10dBV: return "-10dBV";
    }
    return "?";
}
const char* Name(PhonesLevel level) noexcept {
    switch (level) {
        case PhonesLevel::kHighGain: return "high-gain";
        case PhonesLevel::kPlus4dBu: return "+4dBu";
        case PhonesLevel::kMinus10dBV: return "-10dBV";
    }
    return "?";
}
const char* Name(SyncReference reference) noexcept {
    switch (reference) {
        case SyncReference::kAdat1: return "ADAT1";
        case SyncReference::kAdat2: return "ADAT2";
        case SyncReference::kSpdif: return "S/PDIF";
        case SyncReference::kWordClock: return "word-clock";
        case SyncReference::kTimecode: return "TCO/LTC";
    }
    return "?";
}
const char* Name(JackSelect jack) noexcept {
    switch (jack) {
        case JackSelect::kRear: return "rear";
        case JackSelect::kFront: return "front";
        case JackSelect::kFrontAndRear: return "front+rear";
    }
    return "?";
}
const char* Name(ChannelLimit limit) noexcept {
    switch (limit) {
        case ChannelLimit::kAllChannels: return "all";
        case ChannelLimit::kNoAdat2: return "no-ADAT2";
        case ChannelLimit::kAnalogAndSpdif: return "analog+S/PDIF";
        case ChannelLimit::kAnalogOnly: return "analog-only";
    }
    return "?";
}

} // namespace ASFW::Audio::RME
