// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SessionCharacterizationTests.cpp - Golden traces of today's duplex session layer.
//
// Stage S2 of documentation/AUDIO_SESSION_REDESIGN.md replaces
// AudioDuplexCoordinator. Before that, these tests record what it does today
// for every start/stop recipe the device catalog resolves: host transport calls
// (IRM reservation, DMA prepare/start/stop), device stages, and for DICE the
// bus traffic underneath. The rewrite must reproduce these traces except for
// deltas it declares. Like the S0 DICE goldens, they characterize; they do not
// judge.
//
// Regenerate after an intended change: ASFW_UPDATE_GOLDEN=1, then review the diff.

#include <gtest/gtest.h>

#include "SessionTestSupport.hpp"
#include "WireTrace.hpp"

#include "Audio/Protocols/Backends/AudioDuplexCoordinator.hpp"
#include "DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace {

using namespace ASFW::Testing::DICE;
using namespace ASFW::Testing::Session;
using ASFW::Audio::AudioDuplexCoordinator;
using ASFW::Audio::AudioRuntimeRegistry;
using ASFW::Audio::DuplexRestartReason;
using ASFW::DeviceProfiles::Audio::StreamStartShape;
using ASFW::Driver::HardwareInterface;
using ASFW::Driver::Register32;
using ASFW::Testing::FakeDiceWaitClock;
using ASFW::Testing::FakeTimerScheduler;
namespace Ids = ASFW::DeviceProfiles::Audio;

// Plain caps for the scripted families: two streams' worth of AM824 slots in
// one stream per direction, the shape every AV/C unit here uses.
constexpr AudioStreamRuntimeCaps kScriptedCaps{
    .hostInputPcmChannels = 8,
    .hostOutputPcmChannels = 8,
    .deviceToHostAm824Slots = 9,
    .hostToDeviceAm824Slots = 9,
    .sampleRateHz = 48000,
};

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

// One device, the session layer above it, and one trace for everything.
struct SessionRig {
    explicit SessionRig(const SessionShape& s)
        : shape(s),
          guid(SessionGuid(s)),
          bus(s.diceImage != nullptr ? *s.diceImage : DiceDeviceImages::kSaffirePro24Dsp),
          irm(nullBus),
          host(bus.Trace(), hooks, failures),
          coordinator(registry, runtime, host, hardware, &cancel,
                      [this](uint64_t) -> ASFW::Audio::Runtime::IDirectAudioBindingSource* {
                          return &binding;
                      }) {
        NotificationMailbox::Reset();
        hardware.SetTestRegister(Register32::kNodeID, 0);
        bus.Device().ResetToIdle();
        Install(Generation{1});
        const auto route = *registry.CurrentRoute(guid);
        if (s.diceImage == nullptr) {
            protocol = std::make_shared<ScriptedDeviceControl>(bus.Trace(), hooks, failures, irm,
                                                               kScriptedCaps);
        } else if (s.spro24Dsp) {
            protocol = std::make_shared<ASFW::Audio::DICE::Focusrite::SPro24DspProtocol>(
                bus, bus, registry, route, &irm, waitClock);
        } else {
            protocol = std::make_shared<ASFW::Audio::DICE::TCAT::DICETcatProtocol>(
                bus, bus, registry, route, &irm, waitClock);
        }
        EXPECT_EQ(protocol->Initialize(), kIOReturnSuccess);
        protocol->AsDuplexDeviceControl()->SetTeardownCancelToken(&cancel);
        runtime.Insert(guid, protocol);
        bus.Trace().Clear();
    }

    ~SessionRig() { NotificationMailbox::Reset(); }

    void Install(Generation gen) {
        (void)registry.UpsertFromROM(
            MakeSessionRom(shape, gen),
            ASFW::Discovery::LinkPolicy{.localToNode = FwSpeed::S400, .isochToNode = FwSpeed::S400});
    }

    [[nodiscard]] bool IsDice() const noexcept { return shape.diceImage != nullptr; }

    void Mark(const std::string& line) { bus.Trace().Add(line); }

    template <typename Fn>
    IOReturn Call(const std::string& what, Fn&& fn) {
        Mark("## " + what);
        const IOReturn status = fn();
        Mark("## -> " + Status(status) +
             " streaming=" + std::to_string(coordinator.IsStreaming(guid) ? 1 : 0));
        return status;
    }

    IOReturn Start() {
        return Call("StartStreaming", [&] { return coordinator.StartStreaming(guid); });
    }
    IOReturn Stop() {
        return Call("StopStreaming", [&] { return coordinator.StopStreaming(guid); });
    }
    IOReturn Clock(uint32_t rateHz) {
        return Call("RequestClockConfig " + std::to_string(rateHz), [&] {
            return coordinator.RequestClockConfig(guid, AudioClockConfig{.sampleRateHz = rateHz},
                                                  DuplexRestartReason::kSampleRateChange);
        });
    }
    IOReturn Recover(const char* what, DuplexRestartReason reason, uint64_t observedRun = 0) {
        return Call(std::string("RecoverStreaming ") + what, [&] {
            return coordinator.RecoverStreaming(guid, reason, observedRun);
        });
    }
    [[nodiscard]] uint64_t CurrentRun() const {
        const auto session = coordinator.GetSession(guid);
        return session ? session->restartId : 0;
    }

    // A bus reset that rediscovers the same device on the next generation.
    void BusReset() {
        bus.BusReset();
        const Generation next{bus.GetGeneration().value};
        Install(next);
        protocol->UpdateRuntimeContext(*registry.CurrentRoute(guid), nullptr);
    }

    // Make one device stage fail. A scripted family fails the stage itself; a
    // real DICE protocol fails the bus transaction that stage depends on.
    void FailDevice(const std::string& stage) {
        if (!IsDice()) {
            failures["device." + stage] = kIOReturnError;
            return;
        }
        auto& dev = bus.Device();
        if (stage == "prepare") {
            bus.FailNext(OpKind::Lock, kDiceBaseAddressLo + dev.GlobalBase() + GlobalOffset::kOwnerHi,
                         AsyncStatus::kTimeout);
        } else if (stage == "program_rx") {
            bus.FailNext(OpKind::Write,
                         kDiceBaseAddressLo + dev.RxEntryBase(0) + RxOffset::kIsochronous,
                         AsyncStatus::kTimeout);
        } else if (stage == "program_tx") {
            bus.FailNext(OpKind::Write,
                         kDiceBaseAddressLo + dev.TxEntryBase(0) + TxOffset::kIsochronous,
                         AsyncStatus::kTimeout);
        } else if (stage == "confirm") {
            // The device loses source lock just as the host starts transmitting.
            hooks["host.start_transmit"] = [this] {
                bus.Device().SetAchievedClock(ClockRateIndex::k48000, false);
            };
        } else {
            ADD_FAILURE() << "no DICE fault for stage " << stage;
        }
    }

    SessionShape shape;
    uint64_t guid;
    RecordingFireWireBus bus;
    NullFireWireBus nullBus;
    ASFW::IRM::IRMClient irm;
    Hooks hooks;
    Failures failures;
    TracingHostTransport host;
    FakeTimerScheduler timer;
    FakeDiceWaitClock waitClock{timer};
    ASFW::Discovery::DeviceRegistry registry;
    AudioRuntimeRegistry runtime;
    HardwareInterface hardware{};
    FakeBindingSource binding;
    std::atomic<bool> cancel{false};
    std::shared_ptr<IDeviceProtocol> protocol;
    AudioDuplexCoordinator coordinator;
};

struct ScopedLifecycleAssertOff {
    ScopedLifecycleAssertOff() { ASFW::Audio::Backends::gDisableLifecycleAssertForTesting.store(true); }
    ~ScopedLifecycleAssertOff() { ASFW::Audio::Backends::gDisableLifecycleAssertForTesting.store(false); }
};

struct Scenario {
    const char* key;
    void (*run)(SessionRig&);
};

const Scenario kScenarios[] = {
    {"cold-start", [](SessionRig& r) { r.Start(); }},
    {"stop", [](SessionRig& r) { r.Start(); r.Stop(); }},
    {"double-stop", [](SessionRig& r) { r.Start(); r.Stop(); r.Stop(); }},
    {"double-start", [](SessionRig& r) { r.Start(); r.Start(); }},
    // A runtime fault queued during StopIO's teardown and run after it.
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
    // A bus reset during the geometry read refuses the start before any session
    // is stored; the next stop then asks for an illegal Idle -> Stopping
    // transition, which asserts (live in the dext: no build defines NDEBUG).
    // The trace records the refusal with the assertion disabled. Only a scripted
    // device exposes this point in the sequence.
    {"stop-after-refused-start", [](SessionRig& r) {
         r.hooks["device.geometry"] = [&r] { r.BusReset(); };
         r.Start();
         ScopedLifecycleAssertOff off;
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
