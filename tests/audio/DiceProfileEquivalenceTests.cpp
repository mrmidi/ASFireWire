// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceProfileEquivalenceTests.cpp - What every DICE profile answers, recorded.
//
// Stage C of documentation/DICE_TCAT_ARCHITECTURE.md §4.2 collapses the DICE
// profile classes into one builder. This dumps every profile accessor that
// still has a caller, for each DICE builder the catalog names and for the
// registry's generic fallback, at every rate the driver can publish, into
// tests/golden/dice-profiles/<builder>.txt. The files were first recorded from
// the per-model classes; the single builder must reproduce them except for the
// deltas stage C declares (§4.4: "a generated equivalence check across all 26
// rows" -- rows share builders, and a row without one gets no profile).
//
// Regenerate after an intended change: ASFW_UPDATE_GOLDEN=1, then review the diff.

#include <gtest/gtest.h>

#include "WireTrace.hpp"

#include "Audio/DriverKit/Config/AudioProfileRegistry.hpp"
#include "Audio/DriverKit/Config/AudioStreamProfile.hpp"
#include "Audio/DriverKit/Config/DICE/DiceDeviceProfile.hpp"
#include "Audio/DriverKit/Config/DICE/DiceProfile.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"

#include <cstdio>
#include <string>

namespace {

using ASFW::DeviceProfiles::Audio::ProfileBuilderId;
using ASFW::Isoch::Audio::AudioProfileRegistry;
using ASFW::Isoch::Audio::AudioStreamConfig;
using ASFW::Isoch::Audio::IAudioStreamProfile;
using ASFW::Isoch::Audio::TxClockSource;

constexpr uint32_t kRates[] = {32000U, 44100U, 48000U};

std::string Format(const char* fmt, auto... args) {
    char line[256];
    std::snprintf(line, sizeof(line), fmt, args...);
    return line;
}

std::string Config(const char* label, const AudioStreamConfig& c) {
    return Format("%s dir=%u rate=%u mode=%u sid=%u pcm=%u dbs=%u midi=%u fpp=%u fdf=0x%02x "
                  "fmt=0x%02x offset=%u",
                  label, static_cast<unsigned>(c.direction), c.sampleRate,
                  static_cast<unsigned>(c.streamMode), c.sid, c.pcmChannels, c.dbs, c.midiSlots,
                  c.framesPerDataPacket, c.fdf, c.fmt, c.sourceChannelOffset);
}

void Dump(const IAudioStreamProfile& p, ASFW::Testing::WireTrace& out) {
    out.Add(Format("name=%s", p.Name()));
    // Channel counts a Venice or any range profile may be named by.
    constexpr uint32_t kGrid[][2] = {{16, 16}, {24, 24}, {32, 32}, {16, 8}, {8, 16},
                                     {2, 2},   {0, 2},   {12, 8}, {20, 20}};
    for (const auto& g : kGrid) {
        out.Add(Format("nameForGeometry in=%u out=%u -> %s", g[0], g[1],
                       p.NameForGeometry(g[0], g[1])));
    }
    out.Add(Format("wire tx=%u rx=%u", static_cast<unsigned>(p.TxWireFormat()),
                   static_cast<unsigned>(p.RxWireFormat())));
    out.Add(Format("channels tx=%u rx=%u midi tx=%u rx=%u dbs tx=%u rx=%u", p.TxChannelCount(),
                   p.RxChannelCount(), p.TxMidiSlots(), p.RxMidiSlots(), p.TxDbs(), p.RxDbs()));
    out.Add(Format("streams tx=%u rx=%u", p.TxStreamCount(), p.RxStreamCount()));
    if (const auto* dice = dynamic_cast<const ASFW::Isoch::Audio::DICE::DiceProfile*>(&p)) {
        out.Add(Format("assertedPlaybackStreams=%u", dice->AssertedPlaybackStreams()));
    }
    const auto policy = p.TxStreamPolicy();
    out.Add(Format("txPolicy enc=%u variableDbs=%u nonAudioWord=0x%08x initNonAudio=%u "
                   "preserveFdf=%u emptyIdle=%u cadenceData=%u cadenceWord=0x%08x dbcEnd=%u",
                   static_cast<unsigned>(policy.hostToDevicePcmEncoding), policy.variableDbs,
                   policy.defaultNonAudioSlotWord, policy.initializeNonAudioSlots,
                   policy.preserveFdfInNoDataPackets, policy.emptyPacketsDuringIdle,
                   policy.cadencePacketsCarryDataBlocks, policy.cadenceSlotWord,
                   policy.dbcIsEndEvent));
    out.Add(Format("txClock=%s anchorTimeoutMs=%u",
                   p.TransmitClockSource() == TxClockSource::kInternalCadence ? "internal" : "rx-replay",
                   p.InitialClockAnchorTimeoutMs()));
    AudioStreamConfig config{};
    out.Add(p.BuildDefaultTxStreamConfig(config) ? Config("defaultTx", config) : "defaultTx none");
    config = {};
    out.Add(p.BuildDefaultRxStreamConfig(config) ? Config("defaultRx", config) : "defaultRx none");
    for (uint32_t i = 0; i < 3; ++i) {
        config = {};
        out.Add(p.BuildTxStreamConfig(i, config) ? Config(Format("tx[%u]", i).c_str(), config)
                                                 : Format("tx[%u] none", i));
        config = {};
        out.Add(p.BuildRxStreamConfig(i, config) ? Config(Format("rx[%u]", i).c_str(), config)
                                                 : Format("rx[%u] none", i));
    }
    for (const uint32_t rate : kRates) {
        const double hz = rate;
        // Transfer delay is no longer a profile answer: it derives from the
        // wire rate (AppliedTransferDelayTicks, FW-182).
        out.Add(Format("@%u safety tx=%u rx=%u latency tx=%u rx=%u", rate,
                       p.TxSafetyOffsetFrames(hz), p.RxSafetyOffsetFrames(hz),
                       p.TxReportedLatencyFrames(hz), p.RxReportedLatencyFrames(hz)));
    }
    std::string rates = "rates";
    for (const uint32_t rate : p.SupportedSampleRates()) {
        rates += " " + std::to_string(rate);
    }
    out.Add(rates);
}

TEST(DiceProfileEquivalence, EveryDiceBuilderAnswersAsRecorded) {
    uint32_t dumped = 0;
    for (uint32_t id = 1; id <= static_cast<uint32_t>(ProfileBuilderId::kLastValid); ++id) {
        const auto* profile = AudioProfileRegistry::DiceProfileForBuilderId(id);
        if (profile == nullptr) {
            continue;
        }
        ASFW::Testing::WireTrace trace;
        Dump(*profile, trace);
        ASFW::Testing::ExpectMatchesGolden(trace, "dice-profiles/builder-" + std::to_string(id) + ".txt");
        ++dumped;
    }
    // The ten DICE builders the catalog names today.
    EXPECT_EQ(dumped, 10U);
}

TEST(DiceProfileEquivalence, TheGenericFallbackAnswersAsRecorded) {
    // Reached only for a device whose builder did not travel with its nub.
    const auto* profile = dynamic_cast<const IAudioStreamProfile*>(
        AudioProfileRegistry::FindProfile(0, 0, 0, 0));
    ASSERT_NE(profile, nullptr);
    ASFW::Testing::WireTrace trace;
    Dump(*profile, trace);
    ASFW::Testing::ExpectMatchesGolden(trace, "dice-profiles/generic-fallback.txt");
}

} // namespace
