// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfwStreamFormatsTests.cpp - two-tier OXFW stream-format discovery (FW-139).

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/Oxford/OxfwStreamFormats.hpp"

#include "AvcTestRig.hpp"

#include <optional>
#include <span>
#include <vector>

namespace {

using ASFW::Testing::AvcReply;
using ASFW::Testing::AvcTestRig;
namespace Oxford = ASFW::Audio::Oxford;

constexpr uint8_t kStreamFormatOpcode = 0xBF;
constexpr uint8_t kSubfuncCurrent = 0xC0;
constexpr uint8_t kSubfuncSupported = 0xC1;
constexpr uint8_t kInputPlugSignalFormat = 0x19;
constexpr uint8_t kOutputPlugSignalFormat = 0x18;
constexpr uint8_t kCtypeInquiry = 0x02;

/// AV/C stream-format frequency codes (TA 2001002). Deliberately spelled out
/// rather than reused from production: a test that shares the table under test
/// cannot catch the table being wrong.
constexpr uint8_t kRateCode44100 = 0x03;
constexpr uint8_t kRateCode48000 = 0x04;
constexpr uint8_t kRateCode96000 = 0x05;

/// FDF/SFC codes used by INPUT/OUTPUT PLUG SIGNAL FORMAT — a *different* table
/// from the one above, which is exactly the confusion FW-139 has to avoid.
constexpr uint8_t kFdf44100 = 0x01;
constexpr uint8_t kFdf48000 = 0x02;
constexpr uint8_t kFdf96000 = 0x04;

/// A compound AM824 block: [0x90][0x40][rate][flags][infoCount] then
/// [channels][formatCode] pairs. MBLA (0x06) is PCM, 0x0D is MIDI.
std::vector<uint8_t> CompoundAm824Block(uint8_t rateCode, uint8_t pcmChannels,
                                        uint8_t midiSlots) {
    std::vector<uint8_t> block{0x90, 0x40, rateCode, 0x00};
    std::vector<uint8_t> infos;
    uint8_t count = 0;
    if (pcmChannels != 0) {
        infos.push_back(pcmChannels);
        infos.push_back(0x06);
        ++count;
    }
    if (midiSlots != 0) {
        infos.push_back(midiSlots);
        infos.push_back(0x0D);
        ++count;
    }
    block.push_back(count);
    block.insert(block.end(), infos.begin(), infos.end());
    return block;
}

/// The response header the parser skips before the format block: 8 operands for
/// a LIST reply, 7 for a CURRENT reply.
std::vector<uint8_t> FormatResponse(uint8_t subfunction, uint8_t listIndex,
                                    const std::vector<uint8_t>& block) {
    std::vector<uint8_t> operands;
    if (subfunction == kSubfuncSupported) {
        operands = {kSubfuncSupported, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, listIndex};
    } else {
        operands = {kSubfuncCurrent, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF};
    }
    operands.insert(operands.end(), block.begin(), block.end());
    return operands;
}

/// A format response is *longer* than the command that asked for it, so it
/// cannot be expressed by patching the echoed command frame — the extra bytes
/// would fall off the end. Build the whole frame:
/// [response_code][subunit][opcode][operands...].
[[nodiscard]] AvcReply FormatReply(uint8_t subfunction, uint8_t listIndex,
                                   const std::vector<uint8_t>& block) {
    constexpr uint8_t kImplementedStable = 0x0C;
    std::vector<uint8_t> frame{kImplementedStable, 0xFF, kStreamFormatOpcode};
    const auto operands = FormatResponse(subfunction, listIndex, block);
    frame.insert(frame.end(), operands.begin(), operands.end());
    return AvcReply::RawBytes(std::move(frame));
}

[[nodiscard]] bool IsListQuery(std::span<const uint8_t> command) {
    return command.size() >= 4 && command[2] == kStreamFormatOpcode &&
           command[3] == kSubfuncSupported;
}

[[nodiscard]] bool IsCurrentQuery(std::span<const uint8_t> command) {
    return command.size() >= 4 && command[2] == kStreamFormatOpcode &&
           command[3] == kSubfuncCurrent;
}

[[nodiscard]] bool IsRateInquiry(std::span<const uint8_t> command) {
    return command.size() >= 6 && command[0] == kCtypeInquiry &&
           (command[2] == kInputPlugSignalFormat || command[2] == kOutputPlugSignalFormat);
}

/// Operand layout is [plugID][format][frequency]; the frequency byte sits at
/// command[5] once the 3-byte AV/C header is accounted for.
[[nodiscard]] uint8_t InquiredFdf(std::span<const uint8_t> command) { return command[5]; }

[[nodiscard]] uint8_t ListIndexOf(std::span<const uint8_t> command) {
    return command.size() >= 11 ? command[10] : 0xFF;
}

struct DetectOutcome {
    bool fired{false};
    IOReturn status{kIOReturnNotReady};
    Oxford::StreamFormatSet set{};
};

void RunDetect(AvcTestRig& rig, DetectOutcome& outcome, bool isOutput = false) {
    Oxford::DetectStreamFormats(*rig.Transport(), isOutput,
                                [&outcome](IOReturn status,
                                           const Oxford::StreamFormatSet& set) {
                                    outcome.fired = true;
                                    outcome.status = status;
                                    outcome.set = set;
                                });
    rig.Drain();
}

} // namespace

// ============================================================================
// Tier 1 — the device enumerates its formats
// ============================================================================

TEST(OxfwStreamFormats, EnumeratesTheReportedListAndMarksItNotAssumed) {
    AvcTestRig rig;
    rig.Target().SetDeviceModel(
        [](std::span<const uint8_t> command) -> std::optional<AvcReply> {
            if (!IsListQuery(command)) {
                return AvcReply::NotImplemented();
            }
            switch (ListIndexOf(command)) {
                case 0:
                    return FormatReply(kSubfuncSupported, 0,
                                       CompoundAm824Block(kRateCode44100, 2, 1));
                case 1:
                    return FormatReply(kSubfuncSupported, 1,
                                       CompoundAm824Block(kRateCode48000, 2, 1));
                case 2:
                    return FormatReply(kSubfuncSupported, 2,
                                       CompoundAm824Block(kRateCode96000, 2, 1));
                default:
                    // Walking off the end is how the list terminates.
                    return AvcReply::Rejected();
            }
        });

    DetectOutcome outcome;
    RunDetect(rig, outcome);

    ASSERT_TRUE(outcome.fired);
    EXPECT_EQ(outcome.status, kIOReturnSuccess);
    EXPECT_FALSE(outcome.set.assumed) << "the device reported these, we did not infer them";
    EXPECT_EQ(outcome.set.Rates(), (std::vector<uint32_t>{44100, 48000, 96000}));
    ASSERT_EQ(outcome.set.entries.size(), 3U);
    EXPECT_EQ(outcome.set.entries[0].pcmChannels, 2);
    EXPECT_EQ(outcome.set.entries[0].midiSlots, 1);
}

TEST(OxfwStreamFormats, SingleEntryListIsStillAReportedList) {
    AvcTestRig rig;
    rig.Target().SetDeviceModel(
        [](std::span<const uint8_t> command) -> std::optional<AvcReply> {
            if (IsListQuery(command) && ListIndexOf(command) == 0) {
                return FormatReply(kSubfuncSupported, 0,
                                   CompoundAm824Block(kRateCode48000, 2, 1));
            }
            return AvcReply::Rejected();
        });

    DetectOutcome outcome;
    RunDetect(rig, outcome);

    EXPECT_EQ(outcome.status, kIOReturnSuccess);
    EXPECT_FALSE(outcome.set.assumed);
    EXPECT_EQ(outcome.set.Rates(), (std::vector<uint32_t>{48000}));
}

// ============================================================================
// Tier 2 — the device does not implement the list, so rates are probed
// ============================================================================

TEST(OxfwStreamFormats, FallsBackToProbingAndKeepsOnlyAcceptedRates) {
    AvcTestRig rig;
    rig.Target().SetDeviceModel(
        [](std::span<const uint8_t> command) -> std::optional<AvcReply> {
            if (IsListQuery(command)) {
                return AvcReply::NotImplemented();  // no list support at all
            }
            if (IsCurrentQuery(command)) {
                return FormatReply(kSubfuncCurrent, 0,
                                   CompoundAm824Block(kRateCode48000, 2, 1));
            }
            if (IsRateInquiry(command)) {
                const uint8_t fdf = InquiredFdf(command);
                const bool supported =
                    fdf == kFdf44100 || fdf == kFdf48000 || fdf == kFdf96000;
                return supported ? AvcReply::ImplementedStable() : AvcReply::Rejected();
            }
            return AvcReply::NotImplemented();
        });

    DetectOutcome outcome;
    RunDetect(rig, outcome);

    ASSERT_TRUE(outcome.fired);
    EXPECT_EQ(outcome.status, kIOReturnSuccess);
    EXPECT_TRUE(outcome.set.assumed) << "these rates were inferred, not advertised";
    EXPECT_EQ(outcome.set.Rates(), (std::vector<uint32_t>{44100, 48000, 96000}));
    // The channel layout is inherited from the one format the device admitted.
    ASSERT_FALSE(outcome.set.entries.empty());
    EXPECT_EQ(outcome.set.entries[0].pcmChannels, 2);
    EXPECT_EQ(outcome.set.entries[0].midiSlots, 1);
}

TEST(OxfwStreamFormats, ProbesWithInquiryNeverWithControl) {
    // Probing with CONTROL would retune the device once per candidate rate.
    // Discovery must not have that side effect.
    AvcTestRig rig;
    size_t controlWrites = 0;
    rig.Target().SetDeviceModel(
        [&controlWrites](std::span<const uint8_t> command) -> std::optional<AvcReply> {
            if (IsListQuery(command)) {
                return AvcReply::NotImplemented();
            }
            if (IsCurrentQuery(command)) {
                return FormatReply(kSubfuncCurrent, 0,
                                   CompoundAm824Block(kRateCode48000, 2, 1));
            }
            if (command.size() >= 3 && (command[2] == kInputPlugSignalFormat ||
                                        command[2] == kOutputPlugSignalFormat)) {
                if (command[0] == 0x00) {
                    ++controlWrites;
                }
                return AvcReply::ImplementedStable();
            }
            return AvcReply::NotImplemented();
        });

    DetectOutcome outcome;
    RunDetect(rig, outcome);

    EXPECT_EQ(outcome.status, kIOReturnSuccess);
    EXPECT_EQ(controlWrites, 0U) << "discovery must not issue a CONTROL set while probing";
}

TEST(OxfwStreamFormats, DeviceRejectingEveryRateIsAnErrorNotAnEmptySuccess) {
    // An empty format set is not a device with no formats — it is a failed
    // probe, and reporting success would push an empty rate list downstream.
    AvcTestRig rig;
    rig.Target().SetDeviceModel(
        [](std::span<const uint8_t> command) -> std::optional<AvcReply> {
            if (IsCurrentQuery(command)) {
                return FormatReply(kSubfuncCurrent, 0,
                                   CompoundAm824Block(kRateCode48000, 2, 1));
            }
            return AvcReply::Rejected();
        });

    DetectOutcome outcome;
    RunDetect(rig, outcome);

    ASSERT_TRUE(outcome.fired);
    EXPECT_NE(outcome.status, kIOReturnSuccess);
    EXPECT_TRUE(outcome.set.entries.empty());
}

TEST(OxfwStreamFormats, CurrentFormatQueryFailureAbortsRatherThanGuessing) {
    AvcTestRig rig;
    rig.Target().SetDefaultReply(AvcReply::NotImplemented());

    DetectOutcome outcome;
    RunDetect(rig, outcome);

    ASSERT_TRUE(outcome.fired);
    EXPECT_EQ(outcome.status, kIOReturnNotResponding);
    EXPECT_TRUE(outcome.set.entries.empty());
}

// ============================================================================
// Rate-table separation — the defect this layer is most likely to reintroduce
// ============================================================================

TEST(OxfwStreamFormats, StreamFormatRateCodesAreNotFdfCodes) {
    // 48 kHz is 0x04 in the stream-format table and 0x02 in FDF/SFC. A tier-1
    // list reporting 0x04 must decode as 48000, not as the 96000 that the FDF
    // table assigns to 0x04.
    AvcTestRig rig;
    rig.Target().SetDeviceModel(
        [](std::span<const uint8_t> command) -> std::optional<AvcReply> {
            if (IsListQuery(command) && ListIndexOf(command) == 0) {
                return FormatReply(kSubfuncSupported, 0,
                                   CompoundAm824Block(kRateCode48000, 2, 1));
            }
            return AvcReply::Rejected();
        });

    DetectOutcome outcome;
    RunDetect(rig, outcome);

    ASSERT_EQ(outcome.set.entries.size(), 1U);
    EXPECT_EQ(outcome.set.entries[0].sampleRateHz, 48000U);
    EXPECT_TRUE(outcome.set.SupportsRate(48000));
    EXPECT_FALSE(outcome.set.SupportsRate(96000));
}
