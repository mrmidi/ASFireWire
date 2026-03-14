#include <gtest/gtest.h>

#include "Protocols/Audio/DICE/Core/DICENotificationMailbox.hpp"
#include "Protocols/Audio/DICE/Core/DICETransaction.hpp"
#include "Protocols/Audio/DICE/Core/DICETypes.hpp"
#include "Protocols/Audio/DICE/Focusrite/SaffireproCommon.hpp"
#include "Protocols/Audio/DICE/Focusrite/SPro24DspRouting.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace {

using ASFW::Audio::DICE::DICETransaction;
using ASFW::Audio::DICE::ExtensionSections;
using ASFW::Audio::DICE::Focusrite::InputParams;
using ASFW::Audio::DICE::Focusrite::LineInputLevel;
using ASFW::Audio::DICE::Focusrite::MicInputLevel;
namespace NotificationMailbox = ASFW::Audio::DICE::NotificationMailbox;
using ASFW::Audio::DICE::Focusrite::OutputGroupState;
namespace Routing = ASFW::Audio::DICE::Focusrite::SPro24DspRouting;
using ASFW::Audio::DICE::GeneralSections;

void PutBe32(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dst[3] = static_cast<uint8_t>(value & 0xFF);
}

void ExpectQuadlet(const uint8_t* raw, size_t offset, uint32_t expected) {
    EXPECT_EQ(DICETransaction::QuadletFromWire(raw + offset), expected);
}

} // namespace

TEST(DiceFocusriteSerializationTests, GeneralAndExtensionSectionsRemainByteBased) {
    std::array<uint8_t, GeneralSections::kWireSize> generalRaw{};
    PutBe32(generalRaw.data() + 0x00, 0x0040);
    PutBe32(generalRaw.data() + 0x04, 0x001C);
    PutBe32(generalRaw.data() + 0x08, 0x0080);
    PutBe32(generalRaw.data() + 0x0C, 0x0044);
    PutBe32(generalRaw.data() + 0x10, 0x00C0);
    PutBe32(generalRaw.data() + 0x14, 0x0044);

    const auto general = GeneralSections::FromWire(generalRaw.data());
    EXPECT_EQ(general.global.offset, 0x0100U);
    EXPECT_EQ(general.global.size, 0x0070U);
    EXPECT_EQ(general.txStreamFormat.offset, 0x0200U);
    EXPECT_EQ(general.txStreamFormat.size, 0x0110U);
    EXPECT_EQ(general.rxStreamFormat.offset, 0x0300U);
    EXPECT_EQ(general.rxStreamFormat.size, 0x0110U);

    std::array<uint8_t, ExtensionSections::kWireSize> extensionRaw{};
    PutBe32(extensionRaw.data() + 0x08, 0x0002);   // cmd = 0x0008 bytes
    PutBe32(extensionRaw.data() + 0x0C, 0x0002);
    PutBe32(extensionRaw.data() + 0x20, 0x0020);   // router = 0x0080 bytes
    PutBe32(extensionRaw.data() + 0x24, 0x0022);
    PutBe32(extensionRaw.data() + 0x30, 0x0080);   // current_config = 0x0200 bytes
    PutBe32(extensionRaw.data() + 0x34, 0x1800);   // size 0x6000 bytes
    PutBe32(extensionRaw.data() + 0x40, 0x1B75);   // application = 0x6dd4 bytes
    PutBe32(extensionRaw.data() + 0x44, 0x0180);   // size 0x600 bytes

    const auto extension = ExtensionSections::FromWire(extensionRaw.data());
    EXPECT_EQ(extension.command.offset, 0x0008U);
    EXPECT_EQ(extension.command.size, 0x0008U);
    EXPECT_EQ(extension.router.offset, 0x0080U);
    EXPECT_EQ(extension.router.size, 0x0088U);
    EXPECT_EQ(extension.currentConfig.offset, 0x0200U);
    EXPECT_EQ(extension.currentConfig.size, 0x6000U);
    EXPECT_EQ(extension.application.offset, 0x6DD4U);
    EXPECT_EQ(extension.application.size, 0x0600U);
}

TEST(DiceFocusriteSerializationTests, InputParamsFollowLinuxFlagWords) {
    InputParams params;
    params.micLevels = {MicInputLevel::Instrument, MicInputLevel::Instrument};
    params.lineLevels = {LineInputLevel::High, LineInputLevel::High};

    std::array<uint8_t, 8> raw{};
    params.ToWire(raw.data());

    ExpectQuadlet(raw.data(), 0x00, 0x00020002U);
    ExpectQuadlet(raw.data(), 0x04, 0x00010001U);

    const auto roundTrip = InputParams::FromWire(raw.data());
    EXPECT_EQ(roundTrip.micLevels[0], MicInputLevel::Instrument);
    EXPECT_EQ(roundTrip.micLevels[1], MicInputLevel::Instrument);
    EXPECT_EQ(roundTrip.lineLevels[0], LineInputLevel::High);
    EXPECT_EQ(roundTrip.lineLevels[1], LineInputLevel::High);
}

TEST(DiceFocusriteSerializationTests, OutputGroupStateMatchesLinuxPacking) {
    OutputGroupState state;
    state.muteEnabled = true;
    state.dimEnabled = false;
    state.volumes = {127, 120, 100, 64, 0, 1};
    state.volMutes = {false, true, true, false, false, true};
    state.volHwCtls = {true, false, false, true, false, false};
    state.muteHwCtls = {true, false, true, false, false, false};
    state.dimHwCtls = {false, true, false, true, false, false};
    state.hwKnobValue = -12;

    std::array<uint8_t, ASFW::Audio::DICE::Focusrite::kOutputGroupStateSize> raw{};
    state.ToWire(raw.data());

    ExpectQuadlet(raw.data(), 0x00, 0x00000001U);
    ExpectQuadlet(raw.data(), 0x04, 0x00000000U);
    ExpectQuadlet(raw.data(), 0x08, 0x00000700U);
    ExpectQuadlet(raw.data(), 0x0C, 0x00003F1BU);
    ExpectQuadlet(raw.data(), 0x10, 0x00007E7FU);
    ExpectQuadlet(raw.data(), 0x1C, 0x00000009U);
    ExpectQuadlet(raw.data(), 0x20, 0x00000006U);
    ExpectQuadlet(raw.data(), 0x24, 0x00000008U);
    ExpectQuadlet(raw.data(), 0x30, 0x00002805U);
    ExpectQuadlet(raw.data(), 0x48, 0xFFFFFFF4U);

    const auto roundTrip = OutputGroupState::FromWire(raw.data());
    EXPECT_TRUE(roundTrip.muteEnabled);
    EXPECT_FALSE(roundTrip.dimEnabled);
    EXPECT_EQ(roundTrip.volumes, state.volumes);
    EXPECT_EQ(roundTrip.volMutes, state.volMutes);
    EXPECT_EQ(roundTrip.volHwCtls, state.volHwCtls);
    EXPECT_EQ(roundTrip.muteHwCtls, state.muteHwCtls);
    EXPECT_EQ(roundTrip.dimHwCtls, state.dimHwCtls);
    EXPECT_EQ(roundTrip.hwKnobValue, state.hwKnobValue);
}

TEST(DiceFocusriteSerializationTests, HeadphoneFirstRoutingAddsHeadphone1MirrorWithoutDroppingMonitors) {
    std::vector<Routing::RouterEntry> entries = {
        {
            .dst = {.blockId = Routing::kDstBlkIns0, .channel = 0},
            .src = {.blockId = Routing::kSrcBlkAvs0, .channel = 0},
            .peak = 0,
        },
        {
            .dst = {.blockId = Routing::kDstBlkIns0, .channel = 1},
            .src = {.blockId = Routing::kSrcBlkAvs0, .channel = 1},
            .peak = 0,
        },
        {
            .dst = {.blockId = Routing::kDstBlkIns0, .channel = 2},
            .src = {.blockId = 0x02, .channel = 0},
            .peak = 0,
        },
        {
            .dst = {.blockId = Routing::kDstBlkIns0, .channel = 3},
            .src = {.blockId = 0x02, .channel = 1},
            .peak = 0,
        },
    };

    EXPECT_TRUE(Routing::HasStereoPlaybackMirror(entries, Routing::kMonitor12Mirror));
    EXPECT_FALSE(Routing::HasAnyHeadphonePlaybackMirror(entries));

    Routing::ApplyStereoPlaybackMirror(entries, Routing::kHeadphone1Mirror);

    EXPECT_TRUE(Routing::HasStereoPlaybackMirror(entries, Routing::kMonitor12Mirror));
    EXPECT_TRUE(Routing::HasStereoPlaybackMirror(entries, Routing::kHeadphone1Mirror));
    EXPECT_FALSE(Routing::HasStereoPlaybackMirror(entries, Routing::kHeadphone2Mirror));
}

TEST(DiceFocusriteSerializationTests, RoutingCanMirrorPlaybackToAllAnalogPairs) {
    std::vector<Routing::RouterEntry> entries;

    Routing::ApplyStereoPlaybackMirror(entries, Routing::kMonitor12Mirror);
    Routing::ApplyStereoPlaybackMirror(entries, Routing::kHeadphone1Mirror);
    Routing::ApplyStereoPlaybackMirror(entries, Routing::kHeadphone2Mirror);

    EXPECT_TRUE(Routing::HasStereoPlaybackMirror(entries, Routing::kMonitor12Mirror));
    EXPECT_TRUE(Routing::HasStereoPlaybackMirror(entries, Routing::kHeadphone1Mirror));
    EXPECT_TRUE(Routing::HasStereoPlaybackMirror(entries, Routing::kHeadphone2Mirror));
}

TEST(DiceFocusriteSerializationTests, NotificationMailboxDecodesBigEndianWireQuadlets) {
    const std::array<uint8_t, 4> clockAccepted = {0x00, 0x00, 0x00, 0x20};
    const std::array<uint8_t, 4> lockChanged = {0x00, 0x00, 0x00, 0x10};

    NotificationMailbox::Reset();
    EXPECT_EQ(NotificationMailbox::PublishWireQuadlet(clockAccepted.data()),
              ASFW::Audio::DICE::Notify::kClockAccepted);
    EXPECT_EQ(NotificationMailbox::Consume(), ASFW::Audio::DICE::Notify::kClockAccepted);

    NotificationMailbox::Reset();
    NotificationMailbox::PublishWireQuadlet(clockAccepted.data());
    NotificationMailbox::PublishWireQuadlet(lockChanged.data());
    EXPECT_EQ(NotificationMailbox::Consume(),
              ASFW::Audio::DICE::Notify::kClockAccepted |
                  ASFW::Audio::DICE::Notify::kLockChange);
}

TEST(DiceFocusriteSerializationTests, DiceStatusHelpersDecodeSourceAndArx1LockState) {
    constexpr uint32_t status =
        ASFW::Audio::DICE::StatusBits::kSourceLocked |
        (ASFW::Audio::DICE::ClockRateIndex::k48000 << ASFW::Audio::DICE::StatusBits::kNominalRateShift);
    constexpr uint32_t extStatus =
        ASFW::Audio::DICE::ExtStatusBits::kArx1Locked |
        ASFW::Audio::DICE::ExtStatusBits::kArx1Slip;

    EXPECT_TRUE(ASFW::Audio::DICE::IsSourceLocked(status));
    EXPECT_EQ(ASFW::Audio::DICE::NominalRateIndex(status),
              ASFW::Audio::DICE::ClockRateIndex::k48000);
    EXPECT_EQ(ASFW::Audio::DICE::NominalRateHz(status), 48000U);
    EXPECT_TRUE(ASFW::Audio::DICE::IsArx1Locked(extStatus));
    EXPECT_TRUE(ASFW::Audio::DICE::HasArx1Slip(extStatus));
}
