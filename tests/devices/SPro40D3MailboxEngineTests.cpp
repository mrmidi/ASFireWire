// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3MailboxEngineTests.cpp - Stage 1 behavioral validation of the
// serialized mailbox engine against the deterministic simulated TCD3070:
// happy paths, bounded timeouts, the counter-reset ritual, stale
// notifications, counter wraparound, ownership lifecycle, bus resets, and
// provenance transitions. No hardware is touched; the sim's notification
// timing knobs script every interleaving the engine must survive.

#include "Audio/Protocols/DICE/Focusrite/SPro40D3MailboxEngine.hpp"
#include "SPro40D3SimulatedDevice.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>

namespace D3 = ASFW::Audio::DICE::Focusrite::SPro40D3;
using Sim = ASFW::Testing::DICE::SPro40D3SimulatedDevice;

namespace {

constexpr uint64_t kHostNotifyAddress = 0x0000FFFFF0B0F000ULL;

/// Engine + sim rig with an explicit clock. The sim delivers notifications
/// synchronously inside WriteBlock by default (what the firmware does), so
/// commands complete before their Send* call returns.
class EngineRig {
public:
    explicit EngineRig(Sim::Options simOptions,
                       D3::SPro40D3MailboxEngine::Options engineOptions = {})
        : simOptions_{simOptions} {
        sim_ = std::make_unique<Sim>(
            [this](std::span<const uint8_t> frame) {
                notifiedFrames_.push_back(std::vector<uint8_t>(frame.begin(), frame.end()));
                engine_->OnNotification(frame, clockMs_);
            },
            simOptions_);
        auto descriptor = D3::ReadDcpDescriptor(*sim_);
        if (!descriptor.has_value()) {
            ADD_FAILURE() << "simulated descriptor read failed";
            return;
        }
        engine_ = std::make_unique<D3::SPro40D3MailboxEngine>(*sim_, *descriptor,
                                                              engineOptions);
    }

    D3::SPro40D3MailboxEngine& engine() { return *engine_; }
    Sim& sim() { return *sim_; }
    uint64_t now() const { return clockMs_; }
    void advanceTo(uint64_t t) { clockMs_ = t; engine_->Poll(clockMs_); }
    /// Move the clock without polling (deadline-at-ingress tests).
    void setClock(uint64_t t) { clockMs_ = t; }
    /// Every notification frame the sim emitted (tests replay them late).
    [[nodiscard]] const std::vector<std::vector<uint8_t>>& notifiedFrames() const {
        return notifiedFrames_;
    }

    EngineRig() : EngineRig(Sim::Options{}) {}

    /// Registers and returns true on success.
    bool registerHost() {
        return engine().RegisterNotificationAddress(kHostNotifyAddress).has_value();
    }

private:
    Sim::Options simOptions_;
    uint64_t clockMs_{0};
    std::unique_ptr<Sim> sim_;
    std::unique_ptr<D3::SPro40D3MailboxEngine> engine_;
    std::vector<std::vector<uint8_t>> notifiedFrames_;
};

/// Captures a single completion (void flavor).
struct CompletionCapture {
    std::optional<std::expected<void, D3::MailboxError>> result{};
    D3::SPro40D3MailboxEngine::VoidCallback cb() {
        return [this](std::expected<void, D3::MailboxError> r) { result = r; };
    }
};

/// Completion capture that counts invocations (exactly-once assertions).
struct CountingCapture {
    uint32_t calls{0};
    std::optional<std::expected<void, D3::MailboxError>> result{};
    D3::SPro40D3MailboxEngine::VoidCallback cb() {
        return [this](std::expected<void, D3::MailboxError> r) {
            ++calls;
            result = r;
        };
    }
};

struct ControlCapture {
    std::optional<std::expected<uint32_t, D3::MailboxError>> result{};
    D3::SPro40D3MailboxEngine::ControlCallback cb() {
        return [this](std::expected<uint32_t, D3::MailboxError> r) { result = r; };
    }
};

struct MeterCapture {
    std::optional<std::expected<std::span<const int32_t>, D3::MailboxError>> result{};
    D3::SPro40D3MailboxEngine::MeterCallback cb() {
        return [this](std::expected<std::span<const int32_t>, D3::MailboxError> r) {
            if (r) values.assign(r->begin(), r->end());
            result = r;
        };
    }
    std::vector<int32_t> values;
};

} // namespace

// ---------------------------------------------------------------------------
// Setup / ownership lifecycle
// ---------------------------------------------------------------------------

TEST(SPro40D3Engine, DescriptorReadFromTransportMatchesStage0Capture) {
    Sim sim([](std::span<const uint8_t>) {});
    auto d = D3::ReadDcpDescriptor(sim);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->writeAddress, Sim::writeAddress());
    EXPECT_EQ(d->readAddress, Sim::readAddress());
    EXPECT_EQ(d->notificationAddress, D3::kDcpOwnerNone);
}

TEST(SPro40D3Engine, RegistrationClaimsOwnership) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kRegistered);
    EXPECT_EQ(rig.sim().Owner(), kHostNotifyAddress);
    EXPECT_EQ(rig.engine().NotificationAddress(), kHostNotifyAddress);
    // Idempotent re-register with the same address.
    EXPECT_TRUE(rig.engine().RegisterNotificationAddress(kHostNotifyAddress).has_value());
}

TEST(SPro40D3Engine, RegistrationConflictReportsForeignOwner) {
    EngineRig rig;
    // A pre-existing foreign claimant (TCAT, another host) holds the mailbox.
    constexpr uint64_t kForeign = 0x0000FFFFF0DEADBEEFULL;
    rig.sim().ForceOwner(kForeign);
    auto r = rig.engine().RegisterNotificationAddress(kHostNotifyAddress);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kOwnershipConflict);
    EXPECT_EQ(rig.engine().ForeignOwner(), kForeign);
    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kUnregistered);
    // The foreign owner is untouched: ZERO CAS operations were performed
    // (review round 3, finding A: the descriptor honestly reports the
    // foreign owner, and the engine never CASes against a known foreign
    // owner - the old expected-operand-from-descriptor read would have
    // SUCCEEDED here and stolen the mailbox).
    EXPECT_EQ(rig.sim().CasCalls(), 0u);
    EXPECT_EQ(rig.sim().Owner(), kForeign);
    EXPECT_EQ(rig.sim().RequestCount(), 0u);
}

TEST(SPro40D3Engine, OwnershipRaceBetweenReadAndCasReportsRacer) {
    EngineRig rig;
    // The mailbox reads unowned, but another host's claim commits between
    // our descriptor read and our CAS. Their claim wins; ours fails
    // atomically and the racer is reported.
    constexpr uint64_t kRacer = 0x0000FFFFF0BADCA5ULL;
    rig.sim().ArmOwnerRace(kRacer);
    auto r = rig.engine().RegisterNotificationAddress(kHostNotifyAddress);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kOwnershipConflict);
    EXPECT_EQ(rig.engine().ForeignOwner(), kRacer);
    // Exactly one CAS - the race detector, expected = the none sentinel
    // (never a value read from the device). The racer's claim stands.
    EXPECT_EQ(rig.sim().CasCalls(), 1u);
    EXPECT_EQ(rig.sim().Owner(), kRacer);
}

TEST(SPro40D3Engine, StaleSelfAddressWithoutEngineStateIsForeign) {
    EngineRig rig;
    // The device's owner field equals our intended address, but THIS engine
    // never registered: a stale value from a previous process, garbage, or
    // a foreign claimant at that address are indistinguishable. Equality
    // is NOT self-ownership (review round 3, finding A); no CAS.
    rig.sim().ForceOwner(kHostNotifyAddress);
    auto r = rig.engine().RegisterNotificationAddress(kHostNotifyAddress);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kOwnershipConflict);
    EXPECT_EQ(rig.engine().ForeignOwner(), kHostNotifyAddress);
    EXPECT_EQ(rig.sim().CasCalls(), 0u);
    EXPECT_EQ(rig.sim().Owner(), kHostNotifyAddress); // untouched
}

TEST(SPro40D3Engine, IdempotentReRegistrationRequiresDeviceCorroboration) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    // Happy path: state + device agree -> idempotent, zero additional CAS.
    ASSERT_TRUE(rig.engine().RegisterNotificationAddress(kHostNotifyAddress).has_value());
    EXPECT_EQ(rig.sim().CasCalls(), 1u);
    // The mailbox is stolen while we believe we own it: the next Register
    // discovers it (device no longer names us) and never CASes back.
    constexpr uint64_t kThief = 0x0000FFFFF0C0FFEEULL;
    rig.sim().ForceOwner(kThief);
    auto r = rig.engine().RegisterNotificationAddress(kHostNotifyAddress);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kOwnershipConflict);
    EXPECT_EQ(rig.engine().ForeignOwner(), kThief);
    EXPECT_EQ(rig.sim().CasCalls(), 1u); // no new CAS against the thief
    // Confirmed loss invalidates the registration state (round 4).
    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kUnregistered);
    CountingCapture c;
    auto send = rig.engine().SendSetVolume(0, 0, 100, rig.now(), c.cb());
    ASSERT_FALSE(send.has_value());
    EXPECT_EQ(send.error(), D3::MailboxError::kNotRegistered);
    EXPECT_EQ(c.calls, 0u);
    EXPECT_EQ(rig.sim().RequestCount(), 0u); // zero command writes
}

TEST(SPro40D3Engine, OwnershipLossInvalidatesRegistrationForSubsequentCommands) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    EXPECT_EQ(rig.sim().CasCalls(), 1u);
    EXPECT_EQ(rig.sim().RequestCount(), 0u);
    // Ownership changes externally while we believe we are registered.
    constexpr uint64_t kThief = 0x0000FFFFF0D15EA5ULL;
    rig.sim().ForceOwner(kThief);
    // Detection: the next Register observes the mismatch.
    auto r = rig.engine().RegisterNotificationAddress(kHostNotifyAddress);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kOwnershipConflict);
    EXPECT_EQ(rig.engine().ForeignOwner(), kThief); // preserved for diagnostics
    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kUnregistered);
    EXPECT_EQ(rig.sim().CasCalls(), 1u); // no reacquisition CAS, no release CAS
    // Subsequent commands fail the precondition and write nothing.
    CountingCapture c;
    auto send = rig.engine().SendSetVolume(0, 0, 100, rig.now(), c.cb());
    ASSERT_FALSE(send.has_value());
    EXPECT_EQ(send.error(), D3::MailboxError::kNotRegistered);
    EXPECT_EQ(c.calls, 0u);                 // callback never fired
    EXPECT_EQ(rig.sim().RequestCount(), 0u); // zero command writes
    auto routing = rig.engine().SendSetRouting({}, {}, {}, rig.now(), c.cb());
    ASSERT_FALSE(routing.has_value());
    EXPECT_EQ(routing.error(), D3::MailboxError::kNotRegistered);
    EXPECT_EQ(rig.sim().RequestCount(), 0u);
    EXPECT_EQ(rig.sim().CasCalls(), 1u); // still nothing but the original claim
}

TEST(SPro40D3Engine, ClearedOwnershipBehindEngineBackInvalidatesRegistration) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    // The owner field is cleared behind our back (reset we missed, or a
    // foreign host's release): observed == none while we believe we own it.
    rig.sim().ForceOwner(D3::kDcpOwnerNone);
    auto r = rig.engine().RegisterNotificationAddress(kHostNotifyAddress);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kOwnershipConflict);
    // ForeignOwner() reports the cleared state for diagnostics.
    EXPECT_EQ(rig.engine().ForeignOwner(), D3::kDcpOwnerNone);
    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kUnregistered);
    // Commands fail; nothing is written; no CAS is issued (the engine does
    // not auto-reclaim the now-unowned mailbox - that is an explicit
    // Register decision by the caller).
    CountingCapture c;
    auto send = rig.engine().SendSetVolume(0, 0, 100, rig.now(), c.cb());
    ASSERT_FALSE(send.has_value());
    EXPECT_EQ(send.error(), D3::MailboxError::kNotRegistered);
    EXPECT_EQ(c.calls, 0u);
    EXPECT_EQ(rig.sim().RequestCount(), 0u);
    EXPECT_EQ(rig.sim().CasCalls(), 1u); // the original claim only
    // Explicit re-claim after the loss: unregistered + observed none ->
    // the normal claim path succeeds (caller-driven, not automatic).
    ASSERT_TRUE(rig.engine().RegisterNotificationAddress(kHostNotifyAddress).has_value());
    EXPECT_EQ(rig.sim().CasCalls(), 2u);
    EXPECT_EQ(rig.sim().Owner(), kHostNotifyAddress);
}

TEST(SPro40D3Engine, RegistrationAfterResetReclaimsWhenDeviceDropsOwnership) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    EXPECT_EQ(rig.sim().CasCalls(), 1u);
    // Reset both sides (the firmware drops the owner - the inferred,
    // hardware-unverified behavior): re-registration reclaims cleanly.
    rig.sim().BusReset();
    rig.engine().OnBusReset();
    ASSERT_TRUE(rig.engine().RegisterNotificationAddress(kHostNotifyAddress).has_value());
    EXPECT_EQ(rig.sim().CasCalls(), 2u);
    EXPECT_EQ(rig.sim().Owner(), kHostNotifyAddress);
}

TEST(SPro40D3Engine, RegistrationAfterResetRefusesRetainedOwnership) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    // We see the reset but the firmware does NOT drop the owner (retention
    // is hardware-unverified, report §5.7): the field still names our
    // address while our state says unregistered. Conservative refusal -
    // equality without state is not self-ownership; recovery is the
    // operator's (power-cycle, or a verified ownership-dropping reset).
    rig.engine().OnBusReset();
    auto r = rig.engine().RegisterNotificationAddress(kHostNotifyAddress);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kOwnershipConflict);
    EXPECT_EQ(rig.engine().ForeignOwner(), kHostNotifyAddress);
    EXPECT_EQ(rig.sim().CasCalls(), 1u); // zero new CAS
}

TEST(SPro40D3Engine, CommandsRequireRegistration) {
    EngineRig rig;
    CompletionCapture c;
    auto r = rig.engine().SendSetVolume(0, 0, 100, rig.now(), c.cb());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kNotRegistered);
    EXPECT_FALSE(c.result.has_value()); // never launched, never completed
}

TEST(SPro40D3Engine, UnregisterReleasesOwnershipWhenEnabled) {
    D3::SPro40D3MailboxEngine::Options engineOpts;
    engineOpts.allowOwnershipRelease = true;
    EngineRig rig{Sim::Options{}, engineOpts};
    ASSERT_TRUE(rig.registerHost());
    EXPECT_TRUE(rig.engine().UnregisterNotificationAddress().has_value());
    EXPECT_EQ(rig.sim().Owner(), D3::kDcpOwnerNone);
    // The release CAS actually went to the device (register + release).
    EXPECT_EQ(rig.sim().CasCalls(), 2u);
    // Commands are rejected again.
    CompletionCapture c;
    auto r = rig.engine().SendSetVolume(0, 0, 100, rig.now(), c.cb());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kNotRegistered);
}

// ---------------------------------------------------------------------------
// Happy paths
// ---------------------------------------------------------------------------

TEST(SPro40D3Engine, SetVolumeCompletesAndCaches) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    CompletionCapture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(3, 5, 0x1abc, rig.now(), c.cb()).has_value());
    ASSERT_TRUE(c.result.has_value());
    EXPECT_TRUE(c.result->has_value());
    // Device stored the crosspoint; engine mirrored it as Cached.
    EXPECT_EQ(rig.sim().MixerVolume(3, 5), 0x1abc);
    const auto& st = rig.engine().ControlState();
    EXPECT_EQ(st.mixerVolumes[3][5].value, 0x1abc);
    EXPECT_EQ(st.mixerVolumes[3][5].provenance, D3::ValueProvenance::Cached);
    // Counter 0 was used for the first command.
    EXPECT_EQ(rig.engine().CommandCounter(), 1u);
}

TEST(SPro40D3Engine, CounterIncrementsPerMessage) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    CompletionCapture c1, c2;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 1, rig.now(), c1.cb()).has_value());
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 1, 2, rig.now(), c2.cb()).has_value());
    EXPECT_TRUE(c1.result->has_value());
    EXPECT_TRUE(c2.result->has_value());
    EXPECT_EQ(rig.engine().CommandCounter(), 2u);
}

TEST(SPro40D3Engine, SetControlChainsSoftwareNotice) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    CompletionCapture c;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 5,
                                      D3::sw_notice::kMuteDim, rig.now(), c.cb())
                    .has_value());
    ASSERT_TRUE(c.result.has_value());
    EXPECT_TRUE(c.result->has_value());
    // Device control store got the value.
    EXPECT_EQ(rig.sim().ControlStore().at(D3::control_offset::kMute), 5u);
    // The last frame on the wire was the software notice (opcode 80 80 00 02,
    // notice value at [16..20]).
    const auto& last = rig.sim().LastRequest();
    ASSERT_GE(last.size(), 20u);
    EXPECT_EQ(last[0], 0x80); EXPECT_EQ(last[1], 0x80);
    EXPECT_EQ(last[2], 0x00); EXPECT_EQ(last[3], 0x02);
    EXPECT_EQ(ASFW::FW::ReadBE32(last.data() + 16), D3::sw_notice::kMuteDim);
    // Two messages went out: set_control + notice.
    EXPECT_EQ(rig.engine().CommandCounter(), 2u);
    // Engine state: Cached.
    EXPECT_EQ(rig.engine().ControlState().mute.value, 5u);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Cached);
}

TEST(SPro40D3Engine, GetControlObserves) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    // Seed the device store directly, then read through the engine.
    // (get_control on an unset position returns 0 by sim contract.)
    ControlCapture c;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kDim, rig.now(), c.cb())
                    .has_value());
    ASSERT_TRUE(c.result.has_value());
    ASSERT_TRUE(c.result->has_value());
    EXPECT_EQ(**c.result, 0u);
    const auto& st = rig.engine().ControlState();
    EXPECT_EQ(st.dim.provenance, D3::ValueProvenance::Observed);
}

TEST(SPro40D3Engine, ProvenanceTransitionsUnknownCachedObservedUnknown) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    const auto muteProvenance = [&] { return rig.engine().ControlState().mute.provenance; };
    EXPECT_EQ(muteProvenance(), D3::ValueProvenance::Unknown);

    CompletionCapture set;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 1,
                                      D3::sw_notice::kMuteDim, rig.now(), set.cb())
                    .has_value());
    EXPECT_EQ(muteProvenance(), D3::ValueProvenance::Cached);

    ControlCapture get;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kMute, rig.now(), get.cb())
                    .has_value());
    EXPECT_EQ(muteProvenance(), D3::ValueProvenance::Observed);
    EXPECT_EQ(**get.result, 1u);

    rig.sim().BusReset();
    rig.engine().OnBusReset();
    EXPECT_EQ(muteProvenance(), D3::ValueProvenance::Unknown);
}

TEST(SPro40D3Engine, MasterMeterLowRatePosition) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    MeterCapture c;
    ASSERT_TRUE(rig.engine().SendGetMasterMeter(48000, rig.now(), c.cb()).has_value());
    ASSERT_TRUE(c.result.has_value());
    ASSERT_TRUE(c.result->has_value());
    ASSERT_EQ(c.values.size(), D3::kMasterMeterCount);
    // Sim contract: 0x1000 + pos*0x10 + i, with pos 0x3a at low rate.
    EXPECT_EQ(c.values[0], static_cast<int32_t>(0x1000 + 0x3a * 0x10));
    EXPECT_EQ(c.values[1], static_cast<int32_t>(0x1000 + 0x3a * 0x10 + 1));
    EXPECT_EQ(rig.sim().LastRequest()[19], D3::kMasterMeterPosLowRate);
}

TEST(SPro40D3Engine, MixerMeterHighRatePosition) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    MeterCapture c;
    ASSERT_TRUE(rig.engine().SendGetMixerMeter(96000, rig.now(), c.cb()).has_value());
    ASSERT_TRUE(c.result.has_value());
    ASSERT_TRUE(c.result->has_value());
    ASSERT_EQ(c.values.size(), D3::kChannelCount);
    EXPECT_EQ(rig.sim().LastRequest()[19], D3::kMixerMeterPosHighRate);
    EXPECT_EQ(rig.sim().LastRequest()[17], D3::kChannelCount);
}

TEST(SPro40D3Engine, RoutingSendsBothTablesThenCaches) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    D3::RouterOutSrc outSrc{};
    D3::RouterMixerSrc mixerSrc{};
    D3::RouterMeterSrc meterSrc{};
    outSrc[0] = 39; outSrc[1] = 40; // Mon 1/2 from mixer 1/2
    mixerSrc[0] = 1; mixerSrc[17] = 19;
    meterSrc[0] = 39; meterSrc[1] = 40;

    CompletionCapture c;
    ASSERT_TRUE(rig.engine().SendSetRouting(outSrc, mixerSrc, meterSrc,
                                            rig.now(), c.cb()).has_value());
    ASSERT_TRUE(c.result.has_value());
    EXPECT_TRUE(c.result->has_value());
    // Last frame on the wire is the high-rate table (byte 17 marker, 284B).
    const auto& last = rig.sim().LastRequest();
    ASSERT_EQ(last.size(), D3::kRoutingHighRateFrameSize);
    EXPECT_EQ(last[17], 1);
    // Two routing messages went out.
    EXPECT_EQ(rig.engine().CommandCounter(), 2u);
    // Engine mirrored all sources as Cached.
    const auto& st = rig.engine().ControlState();
    EXPECT_EQ(st.routerOutSrc[0].value, 39u);
    EXPECT_EQ(st.routerOutSrc[0].provenance, D3::ValueProvenance::Cached);
    EXPECT_EQ(st.routerMixerSrc[17].value, 19u);
    EXPECT_EQ(st.routerMeterSrc[1].value, 40u);
}

// ---------------------------------------------------------------------------
// Bounded timeouts and stale notifications
// ---------------------------------------------------------------------------

TEST(SPro40D3Engine, TimeoutIsBoundedAndFailsTheCommand) {
    Sim::Options opts;
    opts.dropNextNotifications = 1;
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CompletionCapture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 7, rig.now(), c.cb()).has_value());
    // Inside the deadline: still in flight, no completion.
    rig.advanceTo(199);
    EXPECT_TRUE(rig.engine().CommandInFlight());
    EXPECT_FALSE(c.result.has_value());
    // At the deadline (200 ms default): bounded failure.
    rig.advanceTo(200);
    ASSERT_TRUE(c.result.has_value());
    ASSERT_FALSE(c.result->has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kTimeout);
    EXPECT_FALSE(rig.engine().CommandInFlight());
    // The engine never touched the response area while unsignalled.
    EXPECT_EQ(rig.sim().ResponseAreaReads(), 0u);
}

TEST(SPro40D3Engine, LateNotificationAfterTimeoutIsStale) {
    Sim::Options opts;
    opts.notifyDuringWrite = false; // notification queued, not delivered
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CompletionCapture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 7, rig.now(), c.cb()).has_value());
    rig.advanceTo(200);
    ASSERT_TRUE(c.result.has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kTimeout);
    // The device finally answers; the engine must ignore it, not resurrect it.
    rig.sim().DeliverPendingNotifications();
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 1u);
    EXPECT_FALSE(rig.engine().CommandInFlight());
}

TEST(SPro40D3Engine, UnsolicitedNotificationIsCountedAndIgnored) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    std::array<uint8_t, 12> stray{};
    stray[6] = 0; stray[7] = 4;
    rig.engine().OnNotification(stray, rig.now());
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 1u);
    // Malformed (short) ingress is dropped silently.
    std::array<uint8_t, 8> shortFrame{};
    rig.engine().OnNotification(shortFrame, rig.now());
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 1u);
}

TEST(SPro40D3Engine, BusyRejectsConcurrentCommand) {
    Sim::Options opts;
    opts.dropNextNotifications = 2;
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CompletionCapture c1, c2;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 1, rig.now(), c1.cb()).has_value());
    auto r = rig.engine().SendSetVolume(1, 1, 2, rig.now(), c2.cb());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kBusy);
    EXPECT_FALSE(c2.result.has_value());
    rig.advanceTo(200); // release the first
    ASSERT_TRUE(c1.result.has_value());
}

// ---------------------------------------------------------------------------
// Device status, the counter-reset ritual, and retries
// ---------------------------------------------------------------------------

TEST(SPro40D3Engine, NonzeroStatusTriggersResetRitualAndRetry) {
    Sim::Options opts;
    opts.failNextResponses = 1; // first response carries status 4
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CompletionCapture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(2, 3, 99, rig.now(), c.cb()).has_value());
    // The ritual retried the command transparently and it succeeded.
    ASSERT_TRUE(c.result.has_value());
    EXPECT_TRUE(c.result->has_value());
    EXPECT_TRUE(rig.sim().SawResetFrame());
    EXPECT_EQ(rig.sim().MixerVolume(2, 3), 99);
    EXPECT_EQ(rig.engine().LastDeviceStatus(), 0u);
    // Ritual semantics: retry used counter 0, so the NEXT command uses 1.
    EXPECT_EQ(rig.engine().CommandCounter(), 1u);
    CompletionCapture c2;
    ASSERT_TRUE(rig.engine().SendSetVolume(2, 4, 98, rig.now(), c2.cb()).has_value());
    EXPECT_EQ(rig.sim().LastRequest()[4], 0x00);
    EXPECT_EQ(rig.sim().LastRequest()[5], 0x01);
}

TEST(SPro40D3Engine, PersistentStatusFailsAfterOneRetry) {
    Sim::Options opts;
    opts.failNextResponses = 2; // status 4 twice: retry also fails
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CompletionCapture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(2, 3, 99, rig.now(), c.cb()).has_value());
    ASSERT_TRUE(c.result.has_value());
    ASSERT_FALSE(c.result->has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kResponseStatus);
    EXPECT_EQ(rig.engine().LastDeviceStatus(), 4u);
    EXPECT_TRUE(rig.sim().SawResetFrame());
    EXPECT_FALSE(rig.engine().CommandInFlight());
}

TEST(SPro40D3Engine, CounterMismatchFailsTheCommand) {
    Sim::Options opts;
    opts.counterEchoOffset = 5; // outside the recent-counter window
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CompletionCapture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 5, rig.now(), c.cb()).has_value());
    ASSERT_TRUE(c.result.has_value());
    ASSERT_FALSE(c.result->has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kCounterMismatch);
    // The paired read happened (notification was valid), then validation failed.
    EXPECT_EQ(rig.sim().ResponseAreaReads(), 1u);
}

TEST(SPro40D3Engine, CounterWraparound) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    rig.engine().OverrideCommandCounter(0xFFFE);
    CompletionCapture c1, c2, c3;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 1, rig.now(), c1.cb()).has_value());
    EXPECT_EQ(rig.sim().LastRequest()[4], 0xff);
    EXPECT_EQ(rig.sim().LastRequest()[5], 0xfe);
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 2, rig.now(), c2.cb()).has_value());
    EXPECT_EQ(rig.sim().LastRequest()[5], 0xff);
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 3, rig.now(), c3.cb()).has_value());
    EXPECT_EQ(rig.sim().LastRequest()[4], 0x00);
    EXPECT_EQ(rig.sim().LastRequest()[5], 0x00); // wrapped to 0
    EXPECT_TRUE(c1.result->has_value());
    EXPECT_TRUE(c2.result->has_value());
    EXPECT_TRUE(c3.result->has_value());
    EXPECT_EQ(rig.engine().CommandCounter(), 1u); // 0x0000 + 1
}

// ---------------------------------------------------------------------------
// Bus reset lifecycle
// ---------------------------------------------------------------------------

TEST(SPro40D3Engine, BusResetInvalidatesRegistrationStateAndCounter) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    // Establish some known state.
    CompletionCapture set;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 1,
                                      D3::sw_notice::kMuteDim, rig.now(), set.cb())
                    .has_value());
    ASSERT_TRUE(set.result->has_value());
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Cached);

    rig.sim().BusReset();
    rig.engine().OnBusReset();

    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kUnregistered);
    EXPECT_EQ(rig.engine().CommandCounter(), 0u);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Unknown);
    // Re-registration succeeds (the firmware dropped the owner on reset).
    ASSERT_TRUE(rig.registerHost());
    EXPECT_EQ(rig.sim().Owner(), kHostNotifyAddress);
}

TEST(SPro40D3Engine, BusResetFailsInFlightCommand) {
    Sim::Options opts;
    opts.notifyDuringWrite = false; // keep the command in flight (queued notif)
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CompletionCapture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 7, rig.now(), c.cb()).has_value());
    EXPECT_TRUE(rig.engine().CommandInFlight());

    rig.sim().BusReset();
    rig.engine().OnBusReset();

    ASSERT_TRUE(c.result.has_value());
    ASSERT_FALSE(c.result->has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kBusReset);
    EXPECT_FALSE(rig.engine().CommandInFlight());
    // Any ingress that still arrives is stale: the command it belonged to
    // is gone. (The sim dropped its queued notification on reset.)
    std::array<uint8_t, 12> stray{};
    rig.engine().OnNotification(stray, rig.now());
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 1u);
}

// ---------------------------------------------------------------------------
// Response-area pairing invariant (Stage 0 §7a, correction 4)
// ---------------------------------------------------------------------------

TEST(SPro40D3Engine, ResponseAreaReadIsAlwaysPaired) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    // Three completed commands, each reading the response area exactly once.
    CompletionCapture c1, c2, c3;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 1, rig.now(), c1.cb()).has_value());
    ControlCapture g;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kDim, rig.now(), g.cb())
                    .has_value());
    ASSERT_TRUE(rig.engine().SendSetVolume(1, 0, 2, rig.now(), c2.cb()).has_value());
    ASSERT_TRUE(rig.engine().SendSetVolume(2, 0, 3, rig.now(), c3.cb()).has_value());
    EXPECT_EQ(rig.sim().ResponseAreaReads(), 4u); // set, get, set, set
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
}

// ---------------------------------------------------------------------------
// Reentrancy and the pump (Stage 1 review, P0: reentrant transport/callback
// safety). The sim delivers notifications synchronously inside WriteBlock and
// re-touches the request span afterwards (ASan tripwire); these tests drive
// every re-entrant path through the queue-then-drain pump.
// ---------------------------------------------------------------------------

/// Control-flavor counting capture (exactly-once assertions on get paths).
struct CountingControlCapture {
    uint32_t calls{0};
    std::optional<std::expected<uint32_t, D3::MailboxError>> result{};
    D3::SPro40D3MailboxEngine::ControlCallback cb() {
        return [this](std::expected<uint32_t, D3::MailboxError> r) {
            ++calls;
            result = r;
        };
    }
};

/// Fires a re-entrant engine call from inside the completion callback.
struct ReentrantCapture {
    std::optional<std::expected<void, D3::MailboxError>> first{};
    std::optional<std::expected<uint32_t, D3::MailboxError>> second{};
    D3::SPro40D3MailboxEngine* engine{};
    uint64_t now{};
    bool triggerBusReset{false};
    D3::SPro40D3MailboxEngine::VoidCallback cb() {
        return [this](std::expected<void, D3::MailboxError> r) {
            first = r;
            if (triggerBusReset) {
                engine->OnBusReset(); // re-entrant: must queue, not run nested
            } else {
                auto accepted = engine->SendGetControl32(
                    D3::control_offset::kDim, now,
                    [this](std::expected<uint32_t, D3::MailboxError> s) { second = s; });
                EXPECT_TRUE(accepted.has_value()); // queued: acceptance contract
            }
        };
    }
};

TEST(SPro40D3Engine, SyncNotificationCompletesDuringWriteWithoutRequestMutation) {
    EngineRig rig; // notifyDuringWrite=true: the firmware answers inside WriteBlock
    ASSERT_TRUE(rig.registerHost());
    CountingCapture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 42, rig.now(), c.cb()).has_value());
    // The command completed before Send returned, exactly once, and the
    // request buffer stayed intact for the whole transport call (the sim
    // re-reads it after the notification callback; ASan flags any
    // use-after-free of the old recursive design).
    ASSERT_TRUE(c.result.has_value());
    EXPECT_TRUE(c.result->has_value());
    EXPECT_EQ(c.calls, 1u);
    EXPECT_FALSE(rig.sim().RequestMutatedDuringNotify());
    EXPECT_EQ(rig.sim().RequestCount(), 1u);
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
}

TEST(SPro40D3Engine, ChainedCommandNotificationsDrainThroughThePump) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    CountingCapture c;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 3,
                                      D3::sw_notice::kMuteDim, rig.now(), c.cb())
                    .has_value());
    // Both chained steps (write, then software notice) exchanged with the
    // device through nested synchronous notifications; the pump drained them
    // iteratively and completed the logical command exactly once.
    ASSERT_TRUE(c.result.has_value());
    EXPECT_TRUE(c.result->has_value());
    EXPECT_EQ(c.calls, 1u);
    EXPECT_EQ(rig.engine().CommandCounter(), 2u);
    EXPECT_EQ(rig.sim().RequestCount(), 2u);
    EXPECT_FALSE(rig.sim().RequestMutatedDuringNotify());
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
}

TEST(SPro40D3Engine, CallbackCanLaunchTheNextCommand) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    ReentrantCapture cap;
    cap.engine = &rig.engine();
    cap.now = rig.now();
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 1, rig.now(), cap.cb()).has_value());
    // The re-entrant Send was queued and drained by the same pump: both
    // commands completed before the outer Send returned.
    ASSERT_TRUE(cap.first.has_value());
    EXPECT_TRUE(cap.first->has_value());
    ASSERT_TRUE(cap.second.has_value());
    EXPECT_TRUE(cap.second->has_value());
    EXPECT_EQ(rig.engine().CommandCounter(), 2u);
}

TEST(SPro40D3Engine, CallbackCanTriggerBusReset) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    ReentrantCapture cap;
    cap.engine = &rig.engine();
    cap.triggerBusReset = true;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 1, rig.now(), cap.cb()).has_value());
    ASSERT_TRUE(cap.first.has_value());
    EXPECT_TRUE(cap.first->has_value());
    // The queued bus reset was drained after the callback returned.
    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kUnregistered);
    EXPECT_EQ(rig.engine().CommandCounter(), 0u);
    EXPECT_EQ(rig.engine().ControlState().mixerVolumes[0][0].provenance,
              D3::ValueProvenance::Unknown);
}

TEST(SPro40D3Engine, QueuedLaunchBehindInFlightCommandCompletesBusy) {
    Sim::Options opts;
    opts.notifyDuringWrite = false; // commands stay in flight until delivered
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    // Command A's callback queues TWO launches (B then C). B launches and
    // stays in flight (its notification is pending), so C - already accepted -
    // must complete exactly once with kBusy instead of silently vanishing.
    struct DoubleLaunchCapture {
        std::optional<std::expected<void, D3::MailboxError>> a{}, b{}, c{};
        D3::SPro40D3MailboxEngine* engine{};
        uint64_t now{};
        D3::SPro40D3MailboxEngine::VoidCallback cb() {
            return [this](std::expected<void, D3::MailboxError> r) {
                a = r;
                EXPECT_TRUE(engine->SendSetVolume(1, 0, 2, now,
                    [this](std::expected<void, D3::MailboxError> s) { b = s; }).has_value());
                EXPECT_TRUE(engine->SendSetVolume(2, 0, 3, now,
                    [this](std::expected<void, D3::MailboxError> s) { c = s; }).has_value());
            };
        }
    } cap;
    cap.engine = &rig.engine();
    cap.now = rig.now();
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 1, rig.now(), cap.cb()).has_value());
    rig.sim().DeliverPendingNotifications(); // completes A; callback queues B, C
    ASSERT_TRUE(cap.a.has_value());
    EXPECT_TRUE(cap.a->has_value());
    ASSERT_TRUE(cap.c.has_value());
    ASSERT_FALSE(cap.c->has_value());
    EXPECT_EQ(cap.c->error(), D3::MailboxError::kBusy); // exactly-once, not dropped
    EXPECT_FALSE(cap.b.has_value());                    // B still exchanging
    EXPECT_TRUE(rig.engine().CommandInFlight());
    // C never reached the wire: only A and B did.
    EXPECT_EQ(rig.sim().RequestCount(), 2u);
    rig.sim().DeliverPendingNotifications(); // B's notification
    ASSERT_TRUE(cap.b.has_value());
    EXPECT_TRUE(cap.b->has_value());
    EXPECT_FALSE(rig.engine().CommandInFlight());
}

TEST(SPro40D3Engine, LateNotificationDuringNextCommandIsHarmless) {
    Sim::Options opts;
    opts.notifyDuringWrite = false;
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    // Seed mute=5 so the two get_control responses are distinguishable.
    CompletionCapture seed;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 5,
                                      D3::sw_notice::kMuteDim, rig.now(), seed.cb())
                    .has_value());
    // set_control chains the notice: deliver the primary's notification,
    // then the notice's (queued by the chain step's write).
    rig.sim().DeliverPendingNotifications();
    rig.sim().DeliverPendingNotifications();
    ASSERT_TRUE(seed.result.has_value());
    EXPECT_TRUE(seed.result->has_value());
    // A: get mute (counter 2). B: get dim (counter 3).
    ControlCapture a, b;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kMute, rig.now(), a.cb())
                    .has_value());
    rig.sim().DeliverPendingNotifications();
    ASSERT_TRUE(a.result.has_value());
    EXPECT_EQ(**a.result, 5u);
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kDim, rig.now(), b.cb())
                    .has_value());
    // A's notification is re-delivered while B is in flight (a duplicate the
    // device must never produce, but a robust host survives). The engine
    // attributes the response by counter: B completes with ITS OWN response.
    // Recorded so far: set_control, notice, A. (B's is still pending.)
    ASSERT_EQ(rig.notifiedFrames().size(), 3u);
    rig.engine().OnNotification(rig.notifiedFrames()[2], rig.now());
    ASSERT_TRUE(b.result.has_value());
    ASSERT_TRUE(b.result->has_value());
    EXPECT_EQ(**b.result, 0u); // dim, not mute: no cross-command bleed
    // B's genuine notification then arrives to an empty slot: stale.
    rig.sim().DeliverPendingNotifications();
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 1u);
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
}

TEST(SPro40D3Engine, StaleCounterResponseKeepsCommandWaiting) {
    Sim::Options opts;
    opts.notifyDuringWrite = false;
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    // A completes (counter 0 consumed). B (counter 1) goes in flight; the
    // response area is then overwritten out of band with a response echoing
    // A's OLD counter. The engine must treat it as stale - keep B waiting -
    // and still complete B when the genuine response arrives.
    ControlCapture a, b;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kDim, rig.now(), a.cb())
                    .has_value());
    rig.sim().DeliverPendingNotifications();
    ASSERT_TRUE(a.result.has_value());
    ASSERT_TRUE(a.result->has_value());
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kMute, rig.now(), b.cb())
                    .has_value());

    std::array<uint8_t, 20> staleResp{}; // 16-byte header + 4-byte payload
    staleResp[0] = 0x80; staleResp[1] = 0x80; // get_control opcode echo
    staleResp[2] = 0x00; staleResp[3] = 0x00;
    staleResp[4] = 0x00; staleResp[5] = 0x00; // counter 0 = A's
    staleResp[6] = 0x00; staleResp[7] = 0x04; // payload length 4
    ASFW::FW::WriteBE32(staleResp.data() + 16, 0xDEAD);
    rig.sim().InjectResponse(staleResp);
    std::array<uint8_t, 12> notif{};
    notif[7] = 4; // rlen 4
    rig.engine().OnNotification(notif, rig.now());
    // Stale: counted, no failure, B keeps waiting.
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 1u);
    EXPECT_TRUE(rig.engine().CommandInFlight());
    EXPECT_FALSE(b.result.has_value());

    std::array<uint8_t, 20> genuineResp{};
    genuineResp[0] = 0x80; genuineResp[1] = 0x80;
    genuineResp[2] = 0x00; genuineResp[3] = 0x00;
    genuineResp[4] = 0x00; genuineResp[5] = 0x01; // counter 1 = B's
    genuineResp[6] = 0x00; genuineResp[7] = 0x04;
    ASFW::FW::WriteBE32(genuineResp.data() + 16, 0x1234);
    rig.sim().InjectResponse(genuineResp);
    rig.engine().OnNotification(notif, rig.now());
    ASSERT_TRUE(b.result.has_value());
    ASSERT_TRUE(b.result->has_value());
    EXPECT_EQ(**b.result, 0x1234u);
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 1u);
    EXPECT_EQ(rig.sim().ResponseAreaReads(), 3u); // A + stale + genuine
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
}

// ---------------------------------------------------------------------------
// Exactly-once completion and partial completion (Stage 1 review, P1)
// ---------------------------------------------------------------------------

TEST(SPro40D3Engine, InitialWriteFailureFiresExactlyOnceWithTransport) {
    Sim::Options opts;
    opts.failWriteBlockAtRequestIndex = 1; // the very first command write fails
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CountingCapture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 7, rig.now(), c.cb()).has_value());
    ASSERT_TRUE(c.result.has_value());
    ASSERT_FALSE(c.result->has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kTransport);
    EXPECT_EQ(c.calls, 1u); // exactly once, not zero (the old moved-from bug)
    EXPECT_EQ(rig.engine().LastTransportError(), kIOReturnNotReady);
    EXPECT_FALSE(rig.engine().CommandInFlight());
    EXPECT_EQ(rig.engine().ControlState().mixerVolumes[0][0].provenance,
              D3::ValueProvenance::Unknown);
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
}

TEST(SPro40D3Engine, NoticeChainTransportFailureIsPartialCompletion) {
    Sim::Options opts;
    opts.failWriteBlockAtRequestIndex = 2; // primary write lands, notice fails
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CountingCapture c;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 5,
                                      D3::sw_notice::kMuteDim, rig.now(), c.cb())
                    .has_value());
    ASSERT_TRUE(c.result.has_value());
    ASSERT_FALSE(c.result->has_value());
    // The write was confirmed applied; only the notice step failed.
    EXPECT_EQ(c.result->error(), D3::MailboxError::kPartialCompletion);
    EXPECT_EQ(c.calls, 1u);
    EXPECT_EQ(rig.sim().ControlStore().at(D3::control_offset::kMute), 5u);
    // The confirmed value stays Cached; the failure is explicit and partial.
    EXPECT_EQ(rig.engine().ControlState().mute.value, 5u);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Cached);
    EXPECT_FALSE(rig.engine().CommandInFlight());
}

TEST(SPro40D3Engine, RoutingHighTableFailureIsPartialCompletion) {
    Sim::Options opts;
    opts.failWriteBlockAtRequestIndex = 2; // low table lands, high table fails
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    D3::RouterOutSrc outSrc{};
    D3::RouterMixerSrc mixerSrc{};
    D3::RouterMeterSrc meterSrc{};
    outSrc[0] = 39;
    CountingCapture c;
    ASSERT_TRUE(rig.engine().SendSetRouting(outSrc, mixerSrc, meterSrc,
                                            rig.now(), c.cb()).has_value());
    ASSERT_TRUE(c.result.has_value());
    ASSERT_FALSE(c.result->has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kPartialCompletion);
    EXPECT_EQ(c.calls, 1u);
    // Mixed state (low applied, high not): the mirror falls back to Unknown
    // rather than trusting a half-applied routing.
    EXPECT_EQ(rig.engine().ControlState().routerOutSrc[0].provenance,
              D3::ValueProvenance::Unknown);
    EXPECT_EQ(rig.engine().ControlState().routerMixerSrc[0].provenance,
              D3::ValueProvenance::Unknown);
}

TEST(SPro40D3Engine, TimeoutInvalidatesWriteTargets) {
    Sim::Options opts;
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    // Establish a known crosspoint, then lose the next write's response:
    // the engine cannot know whether the device applied it, so the slot
    // falls back to Unknown (last value kept, untrusted) - never Cached.
    CompletionCapture warm;
    ASSERT_TRUE(rig.engine().SendSetVolume(3, 5, 0x1abc, rig.now(), warm.cb()).has_value());
    ASSERT_TRUE(warm.result->has_value());
    EXPECT_EQ(rig.engine().ControlState().mixerVolumes[3][5].provenance,
              D3::ValueProvenance::Cached);

    CompletionCapture lost;
    rig.sim().ArmDropNotifications(1); // this write's answer never arrives
    ASSERT_TRUE(rig.engine().SendSetVolume(3, 5, 0x2222, rig.now(), lost.cb()).has_value());
    rig.advanceTo(200);
    ASSERT_TRUE(lost.result.has_value());
    EXPECT_EQ(lost.result->error(), D3::MailboxError::kTimeout);
    const auto& slot = rig.engine().ControlState().mixerVolumes[3][5];
    EXPECT_EQ(slot.value, 0x1abc);       // last confirmed value kept
    EXPECT_EQ(slot.provenance, D3::ValueProvenance::Unknown); // but untrusted

    // Same policy for the control store and the routing mirror.
    CompletionCapture setMute;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 7,
                                      D3::sw_notice::kMuteDim, rig.now(), setMute.cb())
                    .has_value());
    ASSERT_TRUE(setMute.result->has_value());
    rig.sim().ArmDropNotifications(1);
    CompletionCapture lostMute;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 9,
                                      D3::sw_notice::kMuteDim, rig.now(), lostMute.cb())
                    .has_value());
    rig.advanceTo(400);
    ASSERT_TRUE(lostMute.result.has_value());
    EXPECT_EQ(lostMute.result->error(), D3::MailboxError::kTimeout);
    EXPECT_EQ(rig.engine().ControlState().mute.value, 7u);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Unknown);

    D3::RouterOutSrc outSrc{};
    outSrc[0] = 39;
    CompletionCapture route;
    ASSERT_TRUE(rig.engine().SendSetRouting(outSrc, {}, {}, rig.now(), route.cb()).has_value());
    ASSERT_TRUE(route.result->has_value());
    rig.sim().ArmDropNotifications(1);
    CompletionCapture lostRoute;
    ASSERT_TRUE(rig.engine().SendSetRouting(outSrc, {}, {}, rig.now(), lostRoute.cb()).has_value());
    rig.advanceTo(600);
    ASSERT_TRUE(lostRoute.result.has_value());
    EXPECT_EQ(lostRoute.result->error(), D3::MailboxError::kTimeout);
    EXPECT_EQ(rig.engine().ControlState().routerOutSrc[0].value, 39u);
    EXPECT_EQ(rig.engine().ControlState().routerOutSrc[0].provenance,
              D3::ValueProvenance::Unknown);
}

// ---------------------------------------------------------------------------
// Bounded response-area reads (Stage 1 review, P0; Stage 0 §7a)
// ---------------------------------------------------------------------------

TEST(SPro40D3Engine, OversizedNotificationRejectedWithoutRead) {
    Sim::Options opts;
    opts.nextNotificationLengthOverride = 500; // 500 + 16 > 400-byte area
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CountingControlCapture c;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kDim, rig.now(), c.cb())
                    .has_value());
    // The oversized notification was dropped WITHOUT reading; the command
    // kept waiting and then failed at its deadline.
    EXPECT_EQ(rig.engine().MalformedNotificationCount(), 1u);
    EXPECT_EQ(rig.sim().ResponseAreaReads(), 0u);
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
    EXPECT_TRUE(rig.engine().CommandInFlight());
    rig.advanceTo(200);
    ASSERT_TRUE(c.result.has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kTimeout);
    EXPECT_EQ(c.calls, 1u);
}

TEST(SPro40D3Engine, WrongLengthNotificationRejectedWithoutRead) {
    Sim::Options opts;
    opts.nextNotificationLengthOverride = 8; // get_control answers 4, not 8
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CountingControlCapture c;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kDim, rig.now(), c.cb())
                    .has_value());
    // Well-formed frame (8 + 16 <= 400) but it contradicts the command's
    // expected response shape: fail without reading, exactly once.
    ASSERT_TRUE(c.result.has_value());
    ASSERT_FALSE(c.result->has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kResponseShapeMismatch);
    EXPECT_EQ(c.calls, 1u);
    EXPECT_EQ(rig.sim().ResponseAreaReads(), 0u);
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
    EXPECT_EQ(rig.engine().MalformedNotificationCount(), 0u); // not malformed
    EXPECT_FALSE(rig.engine().CommandInFlight());
}

TEST(SPro40D3Engine, MalformedFrameIgnoredCommandStillCompletes) {
    Sim::Options opts;
    opts.notifyDuringWrite = false;
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    ControlCapture c;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kDim, rig.now(), c.cb())
                    .has_value());
    std::array<uint8_t, 5> junk{0x80, 0x00, 0x00, 0x01, 0xFF};
    rig.engine().OnNotification(junk, rig.now());
    EXPECT_EQ(rig.engine().MalformedNotificationCount(), 1u);
    EXPECT_TRUE(rig.engine().CommandInFlight()); // dropped, still waiting
    EXPECT_FALSE(c.result.has_value());
    rig.sim().DeliverPendingNotifications(); // the genuine answer
    ASSERT_TRUE(c.result.has_value());
    ASSERT_TRUE(c.result->has_value());
    EXPECT_EQ(rig.sim().ResponseAreaReads(), 1u);
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
}

TEST(SPro40D3Engine, ResponseOpcodeEchoMismatchIsMalformed) {
    Sim::Options opts;
    opts.responseOpcodeCorrupt = true; // one-shot: byte 1 of the echo flips
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CountingControlCapture c;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kDim, rig.now(), c.cb())
                    .has_value());
    // The read was paired and shaped correctly; the header then disagreed
    // with the request opcode.
    ASSERT_TRUE(c.result.has_value());
    ASSERT_FALSE(c.result->has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kMalformedResponse);
    EXPECT_EQ(c.calls, 1u);
    EXPECT_EQ(rig.sim().ResponseAreaReads(), 1u);
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
}

TEST(SPro40D3Engine, ResponseLengthEchoMismatchIsMalformed) {
    Sim::Options opts;
    opts.responseLengthEchoOffset = 4; // header echoes 8, notification said 4
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CountingControlCapture c;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kDim, rig.now(), c.cb())
                    .has_value());
    ASSERT_TRUE(c.result.has_value());
    ASSERT_FALSE(c.result->has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kMalformedResponse);
    EXPECT_EQ(c.calls, 1u);
    EXPECT_EQ(rig.sim().ResponseAreaReads(), 1u);
}

TEST(SPro40D3Engine, DeadlineAtIngressNeverReadsLateResponse) {
    Sim::Options opts;
    opts.notifyDuringWrite = false;
    EngineRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    CountingControlCapture c;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kDim, rig.now(), c.cb())
                    .has_value());
    rig.setClock(200); // exactly the deadline, WITHOUT polling first
    rig.sim().DeliverPendingNotifications();
    // The response arrived at/after the deadline: ambiguous, so it is never
    // trusted and the response area is never read for it.
    ASSERT_TRUE(c.result.has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kTimeout);
    EXPECT_EQ(c.calls, 1u);
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 1u);
    EXPECT_EQ(rig.sim().ResponseAreaReads(), 0u);
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
}

// ---------------------------------------------------------------------------
// Input validation (never send what the device must reject)
// ---------------------------------------------------------------------------

TEST(SPro40D3Engine, InputValidationRejectsOutOfRangeWithoutSending) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    CountingCapture c;
    const auto rejected = [&](auto r) {
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error(), D3::MailboxError::kInvalidArgument);
    };
    rejected(rig.engine().SendSetVolume(D3::kChannelCount, 0, 1, rig.now(), c.cb()));
    rejected(rig.engine().SendSetVolume(0, D3::kMixCount, 1, rig.now(), c.cb()));
    rejected(rig.engine().SendSetVolume(0, 0, -1, rig.now(), c.cb()));
    rejected(rig.engine().SendSetVolume(0, 0, 0x10000, rig.now(), c.cb()));
    rejected(rig.engine().SendGetMasterMeter(0, rig.now(), {}));
    rejected(rig.engine().SendGetMixerMeter(0, rig.now(), {}));
    rejected(rig.engine().SendSetControl32(0, 0, 5, rig.now(), c.cb()));  // bad notice
    rejected(rig.engine().SendSetControl32(0, 0, 0, rig.now(), c.cb()));  // bad notice
    // Nothing fired and nothing reached the device.
    EXPECT_EQ(c.calls, 0u);
    EXPECT_EQ(rig.sim().RequestCount(), 0u);
    // The boundaries themselves are legal (channel 17, mix 15, 0, 0xFFFF).
    ASSERT_TRUE(rig.engine().SendSetVolume(D3::kChannelCount - 1, D3::kMixCount - 1, 0xFFFF,
                                            rig.now(), c.cb()).has_value());
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 0, rig.now(), c.cb()).has_value());
    MeterCapture m;
    ASSERT_TRUE(rig.engine().SendGetMasterMeter(44100, rig.now(), m.cb()).has_value());
    ASSERT_TRUE(m.result.has_value());
    EXPECT_EQ(c.calls, 2u); // the valid ones completed (exactly once each)
    EXPECT_EQ(rig.sim().RequestCount(), 3u);
}

TEST(SPro40D3Engine, RoutingSourceDomainIsValidated) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    D3::RouterOutSrc outSrc{};
    D3::RouterMixerSrc mixerSrc{};
    D3::RouterMeterSrc meterSrc{};
    CountingCapture c;

    // Boundary: every entry at the domain maximum (mixer output 16,
    // MIX_COUNT = 16, spro40d3.rs:29) is valid and completes.
    outSrc.fill(D3::kMaxSymbolicRouteSource);
    mixerSrc.fill(D3::kMaxSymbolicRouteSource);
    meterSrc.fill(D3::kMaxSymbolicRouteSource);
    ASSERT_TRUE(rig.engine().SendSetRouting(outSrc, mixerSrc, meterSrc,
                                            rig.now(), c.cb()).has_value());
    ASSERT_EQ(c.calls, 1u);
    ASSERT_TRUE(c.result.has_value());

    // First invalid id: mixer output 17 does not exist. Then large unsigned
    // values, including the (source << 12) wrap threshold, where a huge
    // symbolic id would silently masquerade as a small source class and
    // contaminate the entry (review round 3, finding B). Each array is
    // checked independently; zero transport ops, zero callbacks.
    const uint32_t wrapAliasOfAnalog1 = 39 + (1u << 20) + 0x80 - 0x300;
    for (uint32_t bad : {D3::kMaxSymbolicRouteSource + 1, 0x1000u,
                         1048576u, wrapAliasOfAnalog1, 0xFFFFFFFFu}) {
        outSrc.fill(0); mixerSrc.fill(0); meterSrc.fill(0);
        outSrc[3] = bad;
        auto r1 = rig.engine().SendSetRouting(outSrc, mixerSrc, meterSrc,
                                              rig.now(), c.cb());
        ASSERT_FALSE(r1.has_value());
        EXPECT_EQ(r1.error(), D3::MailboxError::kInvalidArgument);
        outSrc[3] = 0;
        mixerSrc[7] = bad;
        auto r2 = rig.engine().SendSetRouting(outSrc, mixerSrc, meterSrc,
                                              rig.now(), c.cb());
        ASSERT_FALSE(r2.has_value());
        EXPECT_EQ(r2.error(), D3::MailboxError::kInvalidArgument);
        mixerSrc[7] = 0;
        meterSrc[1] = bad;
        auto r3 = rig.engine().SendSetRouting(outSrc, mixerSrc, meterSrc,
                                              rig.now(), c.cb());
        ASSERT_FALSE(r3.has_value());
        EXPECT_EQ(r3.error(), D3::MailboxError::kInvalidArgument);
        meterSrc[1] = 0;
    }
    // Only the accepted boundary command reached the device (two chained
    // messages: low-rate then high-rate table); the rejected requests
    // performed zero transport operations and fired no callbacks.
    EXPECT_EQ(c.calls, 1u);
    EXPECT_EQ(rig.sim().RequestCount(), 2u);
}

// ---------------------------------------------------------------------------
// Descriptor revalidation (Stage 1 review: revalidate after bus reset)
// ---------------------------------------------------------------------------

TEST(SPro40D3Engine, RegisterRevalidatesDescriptorAfterBusReset) {
    EngineRig rig;
    ASSERT_TRUE(rig.registerHost());
    EXPECT_EQ(rig.sim().CasCalls(), 1u);
    // A different unit answers after the reset (version bumped to 3).
    std::array<uint8_t, 48> other = ASFW::Testing::DICE::kSPro40D3DescriptorBytes;
    other[23] = 3; // version is BE32 at +0x14 = bytes [20..24)
    rig.sim().BusReset();
    rig.engine().OnBusReset();
    rig.sim().SetDescriptorOverride(other);
    auto r = rig.engine().RegisterNotificationAddress(kHostNotifyAddress);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kBadDescriptor);
    // No CAS was attempted against the foreign descriptor.
    EXPECT_EQ(rig.sim().CasCalls(), 1u);
    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kUnregistered);
    // The right unit is back: registration proceeds (fresh owner = none).
    rig.sim().SetDescriptorOverride(std::nullopt);
    ASSERT_TRUE(rig.registerHost());
    EXPECT_EQ(rig.sim().CasCalls(), 2u);
}

// ---------------------------------------------------------------------------
// Ownership release (Stage 1 review: unverified on hardware, off by default)
// ---------------------------------------------------------------------------

TEST(SPro40D3Engine, UnregisterIsDisabledByDefault) {
    EngineRig rig; // default Options: allowOwnershipRelease = false
    ASSERT_TRUE(rig.registerHost());
    auto r = rig.engine().UnregisterNotificationAddress();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), D3::MailboxError::kReleaseDisabled);
    // The unverified release CAS never reached the device.
    EXPECT_EQ(rig.sim().CasCalls(), 1u);
    EXPECT_EQ(rig.sim().Owner(), kHostNotifyAddress);
    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kRegistered);
    // Still fully operational.
    CompletionCapture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 1, rig.now(), c.cb()).has_value());
    ASSERT_TRUE(c.result.has_value());
    EXPECT_TRUE(c.result->has_value());
}
