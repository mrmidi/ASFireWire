// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuHostRebindTests.cpp
// A bus reset while a MOTU device streams must rebind (AUDIO_DEVICE_HOST.md §5 Δ6).
// MotuAudioBackend had no OnDeviceResumed: nothing restarted the stream after a
// reset. Here the real audio device host and the real session run over the MOTU
// recipe, so the rebind is seen as what it is on the wire: a full restart (a new
// run, the device stopped and prepared again).
//
// The device itself is scripted (Linux does not restart MOTU on reset either;
// the vendor kext does): what is under test is that the host asks the session
// for the restart, once, for a streaming MOTU.

#include <gtest/gtest.h>

#include "SessionTestSupport.hpp"

#include "Audio/Host/AudioDeviceHost.hpp"
#include "Audio/Host/MotuFamilyAdapter.hpp"

#include <string>

namespace {

using namespace ASFW::Testing::Session;
using ASFW::Audio::AudioBackendKind;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::Host::AudioDeviceHost;
using ASFW::Audio::Host::HostEvent;
using ASFW::Audio::Host::HostOutcome;
using ASFW::Audio::Host::MotuFamilyAdapter;
namespace Ids = ASFW::DeviceProfiles::Audio;

const SessionShape kMotuDevice{"motu-828mk2", Ids::kMotuVendorId, 0U, Ids::kMotuVendorId,
                               Ids::kMotu828mk2SwVersion};

struct MotuHostRig {
    SessionRig rig{kMotuDevice};
    MotuFamilyAdapter adapter{};
    AudioDeviceHost host{rig.publisher, rig.registry, rig.runtime, rig.sessions, rig.host};

    MotuHostRig() {
        host.Install(AudioBackendKind::MotuRegister, adapter);
        host.WorkQueueForTesting()->SetManualDispatchForTesting(true);
    }

    [[nodiscard]] uint64_t Count(HostEvent event, HostOutcome outcome) const {
        return host.OutcomeCount(event, outcome);
    }
    [[nodiscard]] int TraceCount(const std::string& prefix) {
        int n = 0;
        for (const auto& line : rig.bus.Trace().Lines()) {
            n += line.rfind(prefix, 0) == 0 ? 1 : 0;
        }
        return n;
    }
};

TEST(MotuHostRebindTests, TheHostResolvesTheMotuRecipeToTheMotuAdapter) {
    MotuHostRig m;
    EXPECT_EQ(m.host.KindForGuid(m.rig.guid), AudioBackendKind::MotuRegister);
}

TEST(MotuHostRebindTests, ABusResetWhileStreamingRestartsTheStreamOnce) {
    MotuHostRig m;
    ASSERT_EQ(m.rig.Start(), kIOReturnSuccess);
    ASSERT_TRUE(m.rig.IsStreaming());
    const uint64_t runBefore = m.rig.CurrentRun();
    const int preparesBefore = m.TraceCount("D prepare");

    // The coordinator calls this when the device is rediscovered after a reset while
    // it streams. The rig is not given a new bus generation: its scripted device
    // reports a fixed generation, which the session would then reject as stale
    // before ever reaching the restart this test is about.
    m.host.OnDeviceResumed(m.rig.guid);

    EXPECT_EQ(m.Count(HostEvent::Rebind, HostOutcome::Queued), 1U);
    m.host.WorkQueueForTesting()->DrainAllForTesting();
    m.rig.Settle();

    EXPECT_EQ(m.Count(HostEvent::Rebind, HostOutcome::RestartRequested), 1U);
    EXPECT_EQ(m.Count(HostEvent::Rebind, HostOutcome::Failed), 0U);
    EXPECT_EQ(m.Count(HostEvent::Rebind, HostOutcome::Declined), 0U);
    EXPECT_EQ(m.rig.CurrentRun(), runBefore + 1) << "the rebind is a new run";
    EXPECT_TRUE(m.rig.IsStreaming());
    EXPECT_EQ(m.TraceCount("D prepare"), preparesBefore + 1)
        << "the device is prepared again after the reset";
}

TEST(MotuHostRebindTests, ADuplicateResumeBeforeTheRebindRunsIsDedupedPerDevice) {
    MotuHostRig m;
    ASSERT_EQ(m.rig.Start(), kIOReturnSuccess);
    const uint64_t runBefore = m.rig.CurrentRun();

    m.host.OnDeviceResumed(m.rig.guid);
    m.host.OnDeviceResumed(m.rig.guid);

    EXPECT_EQ(m.Count(HostEvent::Rebind, HostOutcome::Queued), 1U);
    EXPECT_EQ(m.Count(HostEvent::Rebind, HostOutcome::Deduped), 1U);
    m.host.WorkQueueForTesting()->DrainAllForTesting();
    m.rig.Settle();
    EXPECT_EQ(m.rig.CurrentRun(), runBefore + 1) << "one restart, not two";
}

TEST(MotuHostRebindTests, AResetWhileIdleDoesNotStartAnything) {
    MotuHostRig m;

    m.host.OnDeviceResumed(m.rig.guid);

    EXPECT_EQ(m.Count(HostEvent::Rebind, HostOutcome::NotStreaming), 1U);
    EXPECT_EQ(m.Count(HostEvent::Rebind, HostOutcome::Queued), 0U);
    EXPECT_FALSE(m.rig.IsStreaming());
    EXPECT_EQ(m.TraceCount("D prepare"), 0);
}

TEST(MotuHostRebindTests, TeardownBetweenResumeAndRebindCancelsTheRestart) {
    MotuHostRig m;
    ASSERT_EQ(m.rig.Start(), kIOReturnSuccess);
    const uint64_t runBefore = m.rig.CurrentRun();
    m.host.OnDeviceResumed(m.rig.guid);
    ASSERT_EQ(m.Count(HostEvent::Rebind, HostOutcome::Queued), 1U);

    m.host.BeginTeardown();
    m.host.WorkQueueForTesting()->DrainAllForTesting();

    EXPECT_EQ(m.Count(HostEvent::Rebind, HostOutcome::RestartRequested), 0U);
    EXPECT_EQ(m.rig.CurrentRun(), runBefore);
}

TEST(MotuHostRebindTests, ATimingLossWhileStreamingIsJudgedAsARestartForMotu) {
    MotuHostRig m;
    ASSERT_EQ(m.rig.Start(), kIOReturnSuccess);
    const uint64_t runBefore = m.rig.CurrentRun();

    m.host.OnRuntimeFault(m.rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss);
    EXPECT_EQ(m.Count(HostEvent::RuntimeFault, HostOutcome::Queued), 1U);
    m.host.WorkQueueForTesting()->DrainAllForTesting();
    m.rig.Settle();

    // MOTU's verdict is "restart" with no settle and no health read.
    EXPECT_EQ(m.Count(HostEvent::RuntimeFault, HostOutcome::RestartRequested), 1U);
    EXPECT_EQ(m.Count(HostEvent::RuntimeFault, HostOutcome::SelfHealed), 0U);
    EXPECT_EQ(m.Count(HostEvent::RuntimeFault, HostOutcome::DeviceLeft), 0U);
    EXPECT_EQ(m.rig.CurrentRun(), runBefore + 1);
}

}  // namespace
