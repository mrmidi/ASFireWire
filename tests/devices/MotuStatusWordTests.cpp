// Pins the MOTU vendor status word decode.
//
// This quadlet arrives through the DICE notification mailbox but is NOT a DICE
// notification: MOTU reuses the transport, not the encoding. Decoding it with
// DICE names is what hid a device-reported buffer fault behind the label
// "ExtStatus" across five investigation sessions, so the two maps are kept
// apart and both are asserted here against the words actually captured on
// hardware.
//
// Bit map source: the vendor driver (FWA_BoxWithFxDSP::WriteCallback, the class
// FWA_Box828mk3 derives from), cross-checked against the device firmware, which
// assembles exactly these bits.

#include <gtest/gtest.h>

#include "Audio/Protocols/DICE/Core/DICETypes.hpp"
#include "Audio/Protocols/MOTU/MotuStatusWord.hpp"

#include <cstdint>
#include <string>

namespace ASFW::Audio::MOTU {
namespace {

using DICE::FormatNotification;
namespace Notify = DICE::Notify;

// Words observed in steady streaming on an 828 Mk3, runs 2026-08-14T083641Z and
// 090931Z. Each landed ~0.7 s before an audible misframe.
constexpr uint32_t kMisframeWord = 0x010000c2;
// Startup-phase words from the same captures: clock locked, no buffer fault.
constexpr uint32_t kStartupCleanWord = 0x01000002;
constexpr uint32_t kStartupCleanWordAlt = 0x41000002;
// Startup words that DO carry the fault bits, while buffers are being set up.
constexpr uint32_t kStartupFaultWord = 0x410000c2;

TEST(MotuStatusWordTests, MisframeWordCarriesBothBufferFaultBits) {
    EXPECT_TRUE(MotuStatus::HasBufferFault(kMisframeWord));
    EXPECT_TRUE((kMisframeWord & MotuStatus::kBufferUnderflow) != 0);
    EXPECT_TRUE((kMisframeWord & MotuStatus::kBufferOverflow) != 0);
}

TEST(MotuStatusWordTests, MisframeWordAlsoReportsClockLocked) {
    // The device raises the fault WITH its clock locked. Any recovery path that
    // first asks "is the clock healthy?" would therefore suppress every single
    // occurrence -- which is why the buffer-fault restart goes to the session
    // directly and never through the DICE backend's clock-health probe.
    EXPECT_TRUE((kMisframeWord & MotuStatus::kClockLocked) != 0);
}

TEST(MotuStatusWordTests, CleanStartupWordsAreNotBufferFaults) {
    EXPECT_FALSE(MotuStatus::HasBufferFault(kStartupCleanWord));
    EXPECT_FALSE(MotuStatus::HasBufferFault(kStartupCleanWordAlt));
}

TEST(MotuStatusWordTests, StartupFaultWordIsStillAFault) {
    EXPECT_TRUE(MotuStatus::HasBufferFault(kStartupFaultWord));
}

TEST(MotuStatusWordTests, TopBitDiscardsTheWholeWord) {
    // The vendor handler returns before any other test when bit 31 is set, so a
    // word that would otherwise read as a fault must not trigger recovery.
    const uint32_t ignored = kMisframeWord | MotuStatus::kIgnoreWord;
    EXPECT_FALSE(MotuStatus::HasBufferFault(ignored));
}

TEST(MotuStatusWordTests, EitherFaultBitAloneCounts) {
    EXPECT_TRUE(MotuStatus::HasBufferFault(MotuStatus::kBufferUnderflow));
    EXPECT_TRUE(MotuStatus::HasBufferFault(MotuStatus::kBufferOverflow));
}

TEST(MotuStatusWordTests, DiceNamesDoNotDescribeThisWord) {
    // Regression pin for the mislabel itself. Under the DICE map the misframe
    // word reads as TxCfgChg|ExtStatus, which says nothing about a buffer and
    // is what let the fault go unnoticed. Assert the two decodes really differ
    // so nobody "simplifies" the MOTU path back onto FormatNotification.
    char diceBuf[128];
    char motuBuf[128];
    const std::string dice = FormatNotification(kMisframeWord, diceBuf, sizeof(diceBuf));
    const std::string motu = FormatMotuStatus(kMisframeWord, motuBuf, sizeof(motuBuf));

    EXPECT_NE(dice, motu);
    EXPECT_EQ(dice.find("Buffer"), std::string::npos);
    EXPECT_NE(motu.find("BufferUNDERFLOW"), std::string::npos);
    EXPECT_NE(motu.find("BufferOVERFLOW"), std::string::npos);
}

TEST(MotuStatusWordTests, DiceLockChangeBitIsNeverSetByThisDevice) {
    // MOTU signals clock lock in bit 0x02, not DICE's 0x10. Every word captured
    // from the 828 Mk3 leaves 0x10 clear, which is why a gate keyed on
    // Notify::kLockChange can never fire for it.
    for (const uint32_t word : {kMisframeWord, kStartupCleanWord,
                                kStartupCleanWordAlt, kStartupFaultWord}) {
        EXPECT_EQ(word & Notify::kLockChange, 0u) << "word=" << std::hex << word;
    }
}

TEST(MotuStatusWordTests, DecodeNamesEveryKnownBitAndKeepsUnknownsVisible) {
    char buf[160];
    const std::string decoded =
        FormatMotuStatus(MotuStatus::kClockLocked | MotuStatus::kTimestampsValid |
                             MotuStatus::kPedal | MotuStatus::kFactoryReset | 0x00000400,
                         buf, sizeof(buf));

    EXPECT_NE(decoded.find("ClockLocked"), std::string::npos);
    EXPECT_NE(decoded.find("TimestampsValid"), std::string::npos);
    EXPECT_NE(decoded.find("Pedal"), std::string::npos);
    EXPECT_NE(decoded.find("FactoryReset"), std::string::npos);
    // Unnamed bits must survive as hex rather than being silently dropped.
    EXPECT_NE(decoded.find("0x400"), std::string::npos);
}

TEST(MotuStatusWordTests, EmptyWordDecodesToNone) {
    char buf[32];
    EXPECT_EQ(std::string(FormatMotuStatus(0, buf, sizeof(buf))), "none");
}

}  // namespace
}  // namespace ASFW::Audio::MOTU
