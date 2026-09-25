// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SessionCharacterizationTests.cpp - Golden traces of the audio session layer.
//
// For every start/stop recipe the device catalog resolves, these record what
// the session does: host transport calls (IRM reservation, DMA
// prepare/start/stop), device stages, and for DICE the bus traffic underneath.
// They were first recorded against AudioDuplexCoordinator; stage S2 of
// documentation/AUDIO_SESSION_REDESIGN.md replaced it with SessionScheduler,
// which reproduced every trace except three declared deltas (double-start,
// fault-after-stop, stop-after-refused-start). Like the S0 DICE goldens, they
// characterize; they do not judge.
//
// Regenerate after an intended change: ASFW_UPDATE_GOLDEN=1, then review the diff.

#include <gtest/gtest.h>

#include "SessionTestSupport.hpp"
#include "WireTrace.hpp"

#include "Audio/Session/AudioSessions.hpp"
#include "DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace {

using namespace ASFW::Testing::DICE;
using namespace ASFW::Testing::Session;
using ASFW::Audio::AudioRuntimeRegistry;
using ASFW::Audio::DuplexRestartReason;
using ASFW::DeviceProfiles::Audio::StreamStartShape;
using ASFW::Driver::HardwareInterface;
using ASFW::Driver::Register32;
using ASFW::Testing::FakeDiceWaitClock;
using ASFW::Testing::FakeTimerScheduler;
namespace Ids = ASFW::DeviceProfiles::Audio;

struct ShapeCase {
    SessionShape shape;
    StreamStartShape expectedRecipe;
};

const ShapeCase kShapes[] = {
    {{"pro24dsp", Ids::kFocusriteVendorId, Ids::kSPro24DspModelId, Ids::kFocusriteVendorId,
      kDiceUnitVersion, &DiceDeviceImages::kSaffirePro24Dsp, true},
     StreamStartShape::Default},
    {{"venice-f32", Ids::kMidasVendorId, Ids::kMidasVeniceModelId, Ids::kMidasVendorId,
      kDiceUnitVersion, &DiceDeviceImages::kVeniceF32, false},
     StreamStartShape::Default},
    {{"weiss-int202", Ids::kWeissVendorId, Ids::kWeissInt202ModelId, Ids::kWeissVendorId,
      kDiceUnitVersion},
     StreamStartShape::TransmitFirst},
    {{"apogee-duet", Ids::kApogeeVendorId, Ids::kApogeeDuetModelId, kAvcUnitSpecifier,
      kAvcUnitVersion},
     StreamStartShape::ApogeeInterleaved},
    {{"phase88", Ids::kTerraTecVendorId, Ids::kPhase88RackFwModelId, kAvcUnitSpecifier,
      kAvcUnitVersion},
     StreamStartShape::CmpReceiveThenTransmit},
    {{"maudio-1814", Ids::kMAudioVendorId, Ids::kMAudioFireWire1814ModelId, kAvcUnitSpecifier,
      kAvcUnitVersion},
     StreamStartShape::MAudioSpecial},
};

struct Scenario {
    const char* key;
    void (*run)(SessionRig&);
};

const Scenario kScenarios[] = {
    {"cold-start", [](SessionRig& r) { r.Start(); }},
    {"stop", [](SessionRig& r) { r.Start(); r.Stop(); }},
    {"double-stop", [](SessionRig& r) { r.Start(); r.Stop(); r.Stop(); }},
    // Nothing to change: the coordinator used to re-run the whole start over
    // the running streams.
    {"double-start", [](SessionRig& r) { r.Start(); r.Start(); }},
    // A runtime fault queued during StopIO's teardown and run after it. Nothing
    // should run, so nothing is recovered; the coordinator used to restart the
    // streams CoreAudio had just stopped.
    {"fault-after-stop", [](SessionRig& r) {
         r.Start();
         r.Stop();
         r.Recover("timing-loss", DuplexRestartReason::kRecoverAfterTimingLoss);
     }},
    {"clock-change-running", [](SessionRig& r) { r.Start(); r.Clock(44100); }},
    {"idle-clock-then-start", [](SessionRig& r) { r.Clock(44100); r.Start(); }},
    {"recover-bus-reset", [](SessionRig& r) {
         r.Start();
         r.BusReset();
         r.Recover("bus-reset", DuplexRestartReason::kBusResetRebind);
     }},
    {"recover-timing-loss", [](SessionRig& r) {
         r.Start();
         r.Recover("timing-loss", DuplexRestartReason::kRecoverAfterTimingLoss);
     }},
    {"stale-fault", [](SessionRig& r) {
         r.Start();
         const uint64_t firstRun = r.CurrentRun();
         r.Recover("timing-loss", DuplexRestartReason::kRecoverAfterTimingLoss);
         r.Recover("timing-loss from the first run", DuplexRestartReason::kRecoverAfterTimingLoss,
                   firstRun);
     }},
    {"fail-prepare", [](SessionRig& r) { r.FailDevice("prepare"); r.Start(); }},
    {"fail-reserve-capture", [](SessionRig& r) {
         r.failures["host.reserve_capture"] = kIOReturnNoResources;
         r.Start();
     }},
    {"fail-program-rx", [](SessionRig& r) { r.FailDevice("program_rx"); r.Start(); }},
    {"fail-program-tx", [](SessionRig& r) { r.FailDevice("program_tx"); r.Start(); }},
    {"fail-host-start-rx", [](SessionRig& r) {
         r.failures["host.start_receive"] = kIOReturnError;
         r.Start();
     }},
    {"fail-confirm", [](SessionRig& r) { r.FailDevice("confirm"); r.Start(); }},
    {"restart-after-failure", [](SessionRig& r) {
         r.FailDevice("confirm");
         r.Start();
         if (r.IsDice()) {
             r.bus.Device().SetAchievedClock(ClockRateIndex::k48000, true);
         }
         r.Start();
     }},
    {"teardown-mid-start", [](SessionRig& r) {
         r.hooks["host.prepare_transmit"] = [&r] { r.cancel.store(true, std::memory_order_release); };
         r.Start();
         r.Stop();
     }},
    {"rebind-mid-prepare", [](SessionRig& r) {
         r.hooks["host.begin"] = [&r] { r.BusReset(); };
         r.Start();
     }},
    // A bus reset during the geometry read refuses the start; the stop then has
    // nothing to stop. The coordinator used to ask for an illegal Idle ->
    // Stopping transition here, which asserted. Only a scripted device exposes
    // this point in the sequence.
    {"stop-after-refused-start", [](SessionRig& r) {
         r.hooks["device.geometry"] = [&r] { r.BusReset(); };
         r.Start();
         r.Stop();
     }},
};

class SessionCharacterization
    : public ::testing::TestWithParam<std::tuple<ShapeCase, Scenario>> {};

TEST_P(SessionCharacterization, MatchesGolden) {
    const auto& [shapeCase, scenario] = GetParam();
    SessionRig rig(shapeCase.shape);
    scenario.run(rig);
    ExpectMatchesGolden(rig.bus.Trace(), std::string("session/") + shapeCase.shape.key + "/" +
                                             scenario.key + ".trace");
}

INSTANTIATE_TEST_SUITE_P(
    Recorded, SessionCharacterization,
    ::testing::Combine(::testing::ValuesIn(kShapes), ::testing::ValuesIn(kScenarios)),
    [](const auto& info) {
        std::string name = std::string(std::get<0>(info.param).shape.key) + "_" +
                           std::get<1>(info.param).key;
        for (char& c : name) {
            if (c == '-') {
                c = '_';
            }
        }
        return name;
    });

// The catalog resolves the recipe each shape is meant to exercise. If a
// catalog change moves a device to another recipe, its goldens stop covering
// the recipe named here.
class SessionShapes : public ::testing::TestWithParam<ShapeCase> {};

TEST_P(SessionShapes, ResolvesTheIntendedRecipe) {
    const auto& shapeCase = GetParam();
    SessionRig rig(shapeCase.shape);
    const auto record = rig.registry.SnapshotByGuid(rig.guid);
    ASSERT_TRUE(record.has_value());
    const auto* policy = Ids::CurrentAudioPolicy(*record);
    ASSERT_NE(policy, nullptr);
    EXPECT_EQ(policy->plan.support, Ids::SupportDisposition::Supported);
    EXPECT_EQ(policy->plan.streamTraits.start.startShape, shapeCase.expectedRecipe);
}

INSTANTIATE_TEST_SUITE_P(Recorded, SessionShapes, ::testing::ValuesIn(kShapes),
                         [](const auto& info) {
                             std::string name = info.param.shape.key;
                             for (char& c : name) {
                                 if (c == '-') {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

} // namespace
