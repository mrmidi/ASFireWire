// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The allowlist that bounds what may be sent to firmware which hangs on AV/C it
// does not implement. Evidence for every row is AVC_DEVICE_HAZARDS.md H1.

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/AVCCommandFilter.hpp"
#include "ASFWDriver/Protocols/AVC/AVCSignalFormatProbe.hpp"

#include <array>
#include <cstdint>
#include <vector>

using namespace ASFW::Protocols::AVC;
using ASFW::Discovery::AvcCommandFilterId;

namespace {

std::vector<uint8_t> Frame(std::initializer_list<uint8_t> bytes) {
    return std::vector<uint8_t>{bytes};
}

std::span<const FCPPermittedFrame> MAudioTable() {
    return PermittedFramesFor(AvcCommandFilterId::MAudioSpecialBeBoB);
}

} // namespace

TEST(AVCCommandFilterTests, UnrestrictedIsTheDefaultAndAdmitsEverything) {
    // Every ordinary device must be unaffected by this mechanism. An empty
    // table admits even the shapes that are refused on filtered firmware.
    const auto table = PermittedFramesFor(AvcCommandFilterId::Unrestricted);
    EXPECT_TRUE(table.empty());
    EXPECT_TRUE(FrameIsPermitted(table, Frame({0x01, 0xFF, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})));
    EXPECT_TRUE(FrameIsPermitted(table, Frame({0x01, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00})));
}

TEST(AVCCommandFilterTests, AdmitsExactlyTheFrameTheProbeBuilds) {
    // The builder and the filter must agree, or the probe cannot be sent at all.
    for (const auto direction : {SignalFormatPlugDirection::Input,
                                 SignalFormatPlugDirection::Output}) {
        const auto built = BuildSignalFormatProbe(direction, 0);
        EXPECT_TRUE(FrameIsPermitted(MAudioTable(), built))
            << "direction " << static_cast<int>(direction);
    }
}

TEST(AVCCommandFilterTests, PlugIdIsFreeAcrossItsWholeRange) {
    // plugId is the one caller-supplied byte in the probe; it addresses a plug
    // and can never become a command code, so every value must pass.
    for (uint32_t plug = 0; plug <= 0xFF; ++plug) {
        const auto built =
            BuildSignalFormatProbe(SignalFormatPlugDirection::Input,
                                   static_cast<uint8_t>(plug));
        EXPECT_TRUE(FrameIsPermitted(MAudioTable(), built)) << "plug " << plug;
    }
}

TEST(AVCCommandFilterTests, RefusesTheFourFreezeCapableShapes) {
    // AVC_DEVICE_HAZARDS.md H1: UNIT_INFO, SUBUNIT_INFO, PLUG_INFO and the
    // BridgeCo extended stream format commands hang this firmware.
    EXPECT_FALSE(FrameIsPermitted(MAudioTable(),
                                  Frame({0x01, 0xFF, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})))
        << "UNIT_INFO";
    EXPECT_FALSE(FrameIsPermitted(MAudioTable(),
                                  Frame({0x01, 0x08, 0x31, 0x07, 0xFF, 0xFF, 0xFF, 0xFF})))
        << "SUBUNIT_INFO";
    EXPECT_FALSE(FrameIsPermitted(MAudioTable(),
                                  Frame({0x01, 0xFF, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00})))
        << "PLUG_INFO";
    EXPECT_FALSE(FrameIsPermitted(MAudioTable(),
                                  Frame({0x01, 0xFF, 0x2F, 0xC1, 0x00, 0x00, 0x00, 0x00})))
        << "BridgeCo extended stream format";
}

TEST(AVCCommandFilterTests, OpcodeAloneIsNotTheSafetyBoundary) {
    // The reason this filter matches a masked prefix rather than an opcode.
    //
    // The M-Audio clock command and the BridgeCo extensions that freeze these
    // devices are both vendor-dependent. An opcode-granular allowlist admitting
    // 0x00 would admit any vendor command; one admitting 0x2F would admit the
    // extension. The distinguishing bytes are the company ID at 3..5 and the
    // extension marker at byte 3 — neither is the opcode.
    //
    //   references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:186-198
    //   references/linux-sound-firewire-stack/firewire/bebob/bebob_command.c:301-303
    const auto maudioClock =
        Frame({0x00, 0xFF, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    const auto bridgeCoVendor =
        Frame({0x00, 0xFF, 0x00, 0x00, 0x07, 0xF5, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

    // Identical ctype and identical opcode. Only bytes 3..5 differ.
    EXPECT_EQ(maudioClock[0], bridgeCoVendor[0]);
    EXPECT_EQ(maudioClock[2], bridgeCoVendor[2]);

    // One is the command M-Audio's own driver sends on every boot; the other is
    // the extension family that hangs this firmware. An opcode-granular
    // allowlist admitting 0x00 would admit both, and one refusing 0x00 would
    // block the only route to this device's geometry.
    EXPECT_TRUE(FrameIsPermitted(MAudioTable(), maudioClock));
    EXPECT_FALSE(FrameIsPermitted(MAudioTable(), bridgeCoVendor));
}

TEST(AVCCommandFilterTests, VendorClockOperandsAreFreeButItsPaddingIsNot) {
    // clk_src / dig_in_fmt / dig_out_fmt / clk_lock are caller-chosen; the two
    // bytes 10..11 are set to zero by Linux; the vendor kext adds four more
    // zero bytes and sends the full 16-byte frame seen by FireBug. All six are
    // pinned so another vendor command cannot ride behind this row.
    for (uint32_t operand = 0; operand <= 0xFF; ++operand) {
        const auto byte = static_cast<uint8_t>(operand);
        EXPECT_TRUE(FrameIsPermitted(
            MAudioTable(),
            Frame({0x00, 0xFF, 0x00, 0x04, 0x00, 0x04, byte, byte,
                   byte, byte, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})))
            << "operand " << operand;
    }
    EXPECT_FALSE(FrameIsPermitted(
        MAudioTable(),
        Frame({0x00, 0xFF, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00,
               0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00})));

    EXPECT_FALSE(FrameIsPermitted(
        MAudioTable(),
        Frame({0x00, 0xFF, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00})))
        << "Linux's shorter frame is not the selected vendor-kext policy";
}

TEST(AVCCommandFilterTests, AdmitsOnlyTheExactBlankSlateInputSelector) {
    // The original kext emits this as the second half of
    // SetBlankSlateClockSource. The three trailing bytes are AVCCdb's quadlet
    // padding and are intentionally part of the filter boundary.
    const auto exact = Frame({0x00, 0x08, 0xB8, 0x80, 0x04, 0x10,
                              0x02, 0x00, 0x01, 0x00, 0x00, 0x00});
    EXPECT_TRUE(FrameIsPermitted(MAudioTable(), exact));

    // Do not turn this into an Audio-subunit or FUNCTION-BLOCK-wide escape
    // hatch: this firmware freezes on commands outside its tiny known surface.
    for (const size_t index : {size_t{1}, size_t{3}, size_t{4}, size_t{7},
                               size_t{8}, size_t{11}}) {
        auto mutated = exact;
        mutated[index] ^= 0x01U;
        EXPECT_FALSE(FrameIsPermitted(MAudioTable(), mutated))
            << "byte " << index;
    }
    EXPECT_FALSE(FrameIsPermitted(
        MAudioTable(),
        Frame({0x00, 0x08, 0xB8, 0x81, 0x04, 0x10,
               0x02, 0x00, 0x01, 0x00, 0x00, 0x00})))
        << "feature block";
}

TEST(AVCCommandFilterTests, AdmitsControlSignalFormatOnBothPlugs) {
    // Rate setting, and the post-DMA-start re-send the 1814 needs (H8).
    // Attested by Linux special_set_rate() and by the vendor kext binding blind
    // 0x18/0x19 CONTROL into this model's own vtable.
    EXPECT_TRUE(FrameIsPermitted(MAudioTable(),
                                 Frame({0x00, 0xFF, 0x19, 0x00, 0x90, 0x02, 0xFF, 0xFF})));
    EXPECT_TRUE(FrameIsPermitted(MAudioTable(),
                                 Frame({0x00, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF})));
}

TEST(AVCCommandFilterTests, TableHoldsOnlyWhatSomethingActuallySends) {
    // H1 lists one more safe command than this table carries: the 03 00 01 LED
    // vendor command. Nothing in the driver sends it, so it is not admitted.
    // H1 is research; the table is authorization, and rows arrive with callers.
    const auto ledCommand =
        Frame({0x00, 0xFF, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_FALSE(FrameIsPermitted(MAudioTable(), ledCommand));
}

TEST(AVCCommandFilterTests, ShortFrameCannotMatchALongerRow) {
    // A frame truncated before the pinned bytes must not slip through on the
    // bytes it does carry.
    EXPECT_FALSE(FrameIsPermitted(MAudioTable(), Frame({0x01, 0xFF, 0x19})));
    EXPECT_FALSE(FrameIsPermitted(MAudioTable(), Frame({0x01, 0xFF, 0x19, 0x00, 0x90})));
}

TEST(AVCCommandFilterTests, AppendedOperandsCannotRideBehindAPermittedFrame) {
    auto selector = Frame({0x00, 0x08, 0xB8, 0x80, 0x04, 0x10,
                           0x02, 0x00, 0x01, 0x00, 0x00, 0x00});
    ASSERT_TRUE(FrameIsPermitted(MAudioTable(), selector));
    selector.push_back(0x00);
    EXPECT_FALSE(FrameIsPermitted(MAudioTable(), selector));

    auto signalFormat =
        Frame({0x00, 0xFF, 0x19, 0x00, 0x90, 0x02, 0xFF, 0xFF});
    ASSERT_TRUE(FrameIsPermitted(MAudioTable(), signalFormat));
    signalFormat.push_back(0x00);
    EXPECT_FALSE(FrameIsPermitted(MAudioTable(), signalFormat));
}

TEST(AVCCommandFilterTests, PinnedBytesAreEnforcedIndividually) {
    // Each 0xFF care byte must actually be load-bearing; a row that pinned
    // nothing would admit everything of the right length.
    const auto good = BuildSignalFormatProbe(SignalFormatPlugDirection::Input, 0);
    for (const size_t index : {size_t{0}, size_t{1}, size_t{2}, size_t{4}}) {
        std::vector<uint8_t> mutated(good.begin(), good.end());
        mutated[index] ^= 0xFFU;
        EXPECT_FALSE(FrameIsPermitted(MAudioTable(), mutated))
            << "byte " << index << " was not enforced";
    }
}

TEST(AVCCommandFilterTests, CareMaskSupportsPartialBytes) {
    // AV/C subunit addresses encode as 0x08 | id. This synthetic row proves the
    // mask primitive independently; the real M-Audio selector row intentionally
    // pins subunit id 0 as part of its narrower safety boundary.
    constexpr FCPPermittedFrame subunitRow{
        .prefix = {0x00, 0x08, 0xB8},
        .care = {0xFF, 0xF8, 0xFF},
        .length = 3,
        .name = "audio subunit function block",
    };
    const std::array<FCPPermittedFrame, 1> table{subunitRow};
    for (uint8_t id = 0; id < 0x08U; ++id) {
        EXPECT_TRUE(FrameIsPermitted(
            table, Frame({0x00, static_cast<uint8_t>(0x08U | id), 0xB8})))
            << "subunit id " << static_cast<int>(id);
    }
    EXPECT_FALSE(FrameIsPermitted(table, Frame({0x00, 0x10, 0xB8})));
}
