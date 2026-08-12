// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The M-Audio special firmware freezes on ordinary AV/C discovery, so this is
// the only command we may point at it. A wrong byte here is not a failed read —
// it is an unsupported command sent to firmware known to hang on those. Every
// expectation below is pinned to the Linux reference rather than derived.

#include "ASFWDriver/Protocols/AVC/AVCSignalFormatProbe.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace {

using namespace ASFW::Protocols::AVC;

TEST(AVCSignalFormatProbeTests, InputRequestIsByteForByteTheLinuxFrame) {
    // fcp.c:96-106 with dir = AVC_GENERAL_PLUG_DIR_IN, pid = 0.
    const auto frame = BuildSignalFormatProbe(SignalFormatPlugDirection::Input, 0);
    const std::array<uint8_t, 8> expected{
        0x01,  // AV/C STATUS
        0xFF,  // unit
        0x19,  // INPUT PLUG SIGNAL FORMAT
        0x00,  // plug id
        0x90,  // EOH_1, Form_1, FMT AM824
        0xFF, 0xFF, 0xFF,
    };
    EXPECT_EQ(std::vector<uint8_t>(frame.begin(), frame.end()),
              std::vector<uint8_t>(expected.begin(), expected.end()));
}

TEST(AVCSignalFormatProbeTests, OutputRequestOnlyChangesTheOpcode) {
    const auto in = BuildSignalFormatProbe(SignalFormatPlugDirection::Input, 0);
    const auto out = BuildSignalFormatProbe(SignalFormatPlugDirection::Output, 0);
    EXPECT_EQ(out[2], 0x18);
    for (size_t i = 0; i < in.size(); ++i) {
        if (i == 2) continue;
        EXPECT_EQ(in[i], out[i]) << "byte " << i << " must not vary with direction";
    }
}

TEST(AVCSignalFormatProbeTests, ByteFourIsNotAQueryWildcard) {
    // Linux sends 0x90 literally and requires it echoed. Sending 0xFF here — the
    // usual AV/C "query this field" convention, and what the pre-existing
    // AVCOutputPlugSignalFormatCommand does — is a different request.
    const auto frame = BuildSignalFormatProbe(SignalFormatPlugDirection::Input, 0);
    EXPECT_EQ(frame[4], 0x90);
    EXPECT_NE(frame[4], 0xFF);
}

TEST(AVCSignalFormatProbeTests, PlugIdIsTheOnlyCallerControlledByte) {
    for (const uint8_t plug : {uint8_t{0}, uint8_t{1}, uint8_t{7}, uint8_t{0xFE}}) {
        const auto frame = BuildSignalFormatProbe(SignalFormatPlugDirection::Input, plug);
        EXPECT_EQ(frame[3], plug);
        // The command code is unreachable from the caller's value.
        EXPECT_EQ(frame[0], 0x01);
        EXPECT_EQ(frame[2], 0x19);
    }
}

[[nodiscard]] std::vector<uint8_t> ReplyTo(std::span<const uint8_t> request,
                                           uint8_t status, uint8_t fdfHi) {
    std::vector<uint8_t> reply(request.begin(), request.end());
    reply[0] = status;
    reply[5] = fdfHi;
    return reply;
}

TEST(AVCSignalFormatProbeTests, DecodesEverySfcToTheAmdtpRateTable) {
    // amdtp-stream.c:149-157.
    const std::array<uint32_t, 7> table{32000, 44100, 48000, 88200, 96000, 176400, 192000};
    const auto request = BuildSignalFormatProbe(SignalFormatPlugDirection::Input, 0);
    for (uint8_t sfc = 0; sfc < table.size(); ++sfc) {
        const auto reply = ReplyTo(request, 0x0C, sfc);
        const auto result = DecodeSignalFormatProbe(request, reply);
        EXPECT_EQ(result.outcome, SignalFormatProbeOutcome::Decoded) << "sfc " << int(sfc);
        EXPECT_EQ(result.sampleRateHz, table[sfc]) << "sfc " << int(sfc);
    }
}

TEST(AVCSignalFormatProbeTests, OnlyTheLowThreeBitsOfFdfHiAreTheSfc) {
    const auto request = BuildSignalFormatProbe(SignalFormatPlugDirection::Input, 0);
    // 0xFA -> low three bits are 2 -> 48 kHz. The upper bits must be ignored.
    const auto result = DecodeSignalFormatProbe(request, ReplyTo(request, 0x0C, 0xFA));
    EXPECT_EQ(result.outcome, SignalFormatProbeOutcome::Decoded);
    EXPECT_EQ(result.sampleRateHz, 48000U);
}

TEST(AVCSignalFormatProbeTests, ReservedSfcIsInTransitionNotUnsupported) {
    // fcp.c:125-128 retries on a reserved sfc rather than failing: it means
    // "ask again", and reporting it as unsupported would be a wrong conclusion
    // about the device.
    const auto request = BuildSignalFormatProbe(SignalFormatPlugDirection::Input, 0);
    const auto result = DecodeSignalFormatProbe(request, ReplyTo(request, 0x0C, 0x07));
    EXPECT_EQ(result.outcome, SignalFormatProbeOutcome::InTransition);
    EXPECT_EQ(result.sfc, 0x07);
}

TEST(AVCSignalFormatProbeTests, SeparatesTheThreeAvcRefusals) {
    const auto request = BuildSignalFormatProbe(SignalFormatPlugDirection::Input, 0);
    EXPECT_EQ(DecodeSignalFormatProbe(request, ReplyTo(request, 0x08, 0x02)).outcome,
              SignalFormatProbeOutcome::NotImplemented);
    EXPECT_EQ(DecodeSignalFormatProbe(request, ReplyTo(request, 0x0A, 0x02)).outcome,
              SignalFormatProbeOutcome::Rejected);
    EXPECT_EQ(DecodeSignalFormatProbe(request, ReplyTo(request, 0x0B, 0x02)).outcome,
              SignalFormatProbeOutcome::InTransition);
}

TEST(AVCSignalFormatProbeTests, RejectsAReplyThatDidNotEchoTheRequest) {
    // Without this check a reply to some other command can be read as this
    // command's answer, which on this hardware would mean inventing a rate.
    const auto request = BuildSignalFormatProbe(SignalFormatPlugDirection::Input, 0);
    for (const size_t corrupt : {size_t{1}, size_t{2}, size_t{3}, size_t{4}}) {
        auto reply = ReplyTo(request, 0x0C, 0x02);
        reply[corrupt] = static_cast<uint8_t>(reply[corrupt] ^ 0xFFU);
        EXPECT_EQ(DecodeSignalFormatProbe(request, reply).outcome,
                  SignalFormatProbeOutcome::Mismatched)
            << "echoed byte " << corrupt << " was not checked";
    }
}

TEST(AVCSignalFormatProbeTests, ShortReplyIsMalformedNotAZeroRate) {
    const auto request = BuildSignalFormatProbe(SignalFormatPlugDirection::Input, 0);
    auto reply = ReplyTo(request, 0x0C, 0x02);
    reply.resize(7);
    const auto result = DecodeSignalFormatProbe(request, reply);
    EXPECT_EQ(result.outcome, SignalFormatProbeOutcome::Malformed);
    EXPECT_EQ(result.sampleRateHz, 0U);
}

} // namespace
