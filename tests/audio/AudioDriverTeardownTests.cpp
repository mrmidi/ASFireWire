#include "ASFWAvcAudioStream.h"
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioDriverTeardownTests.cpp - ASFWAudioDriver's graph teardown leaves the
// driver holding no ADK object.
//
// Found 2026-10-02: after an OHCI unplug the dext process stayed alive, its
// IOUserServer still registered, and "ASFWAudioDriver: free()" was never
// logged. Stop() detached the device with RemoveObject but kept the device,
// streams, buffers and controls in the ivars until free(). The device is
// created with its driver (ASFWAudioDevice::init(&driver)), so a device the
// ivars still hold can keep the driver alive. Whether real ADK retains the
// driver there is unverified (ADKVirtualAudioLab O1); these tests pin our side:
// after the teardown Stop runs, the driver holds nothing.

#include "Audio/DriverKit/ASFWAudioDriverPrivate.hpp"

#include "ASFWAudioDevice.h"
#include "ASFWAudioDriver.h"
#include "ASFWAudioNub.h"
#include "ASFWProtocolBooleanControl.h"

#include <gtest/gtest.h>

#include <memory>
#include <bit>

namespace {

using ASFW::Audio::DriverKit::StopAudioDriverGraph;
using ASFW::Audio::DriverKit::TearDownAudioGraph;

/// A device that holds its driver, as an ADK device created with
/// init(&driver) may.
class DeviceHoldingDriver final : public ASFWAudioDevice {
public:
    explicit DeviceHoldingDriver(OSObject* driver) : driver_(driver) { driver_->retain(); }
    void free() override {
        driver_->release();
        ASFWAudioDevice::free();
    }

private:
    OSObject* driver_;
};

/// Every ADK object a started driver holds, each with one reference owned by
/// the test, so a count back at 1 means the driver let go.
struct StartedGraph {
    OSSharedPtr<IOUserAudioStream> input{new IOUserAudioStream(), OSNoRetain};
    OSSharedPtr<IOUserAudioStream> output{new IOUserAudioStream(), OSNoRetain};
    OSSharedPtr<IOMemoryMap> inputMap{new IOMemoryMap(), OSNoRetain};
    OSSharedPtr<IOMemoryMap> outputMap{new IOMemoryMap(), OSNoRetain};
    OSSharedPtr<IOMemoryMap> controlMap{new IOMemoryMap(), OSNoRetain};
    OSSharedPtr<ASFWProtocolBooleanControl> control{new ASFWProtocolBooleanControl(), OSNoRetain};

    void AttachTo(ASFWAudioDriver_IVars& ivars, OSSharedPtr<ASFWAudioDevice> device) const {
        ivars.audioDevice = std::move(device);
        ivars.inputStream = input;
        ivars.outputStream = output;
        ivars.inputMap = inputMap;
        ivars.outputMap = outputMap;
        ivars.controlMap = controlMap;
        ivars.device.boolControls[0].control = control;
        ivars.device.boolControls[0].valid = true;
        ivars.device.boolControlCount = 1;
        ivars.graphState = {.inputStreamAdded = true, .outputStreamAdded = true, .audioDeviceAdded = true};
    }
};

TEST(AudioDriverTeardownTests, TeardownDetachesAndReleasesTheWholeGraph) {
    OSSharedPtr<ASFWAudioDriver> driver{new ASFWAudioDriver(), OSNoRetain};
    auto ivars = std::make_unique<ASFWAudioDriver_IVars>();
    OSSharedPtr<ASFWAudioDevice> device{new ASFWAudioDevice(), OSNoRetain};
    StartedGraph graph;
    graph.AttachTo(*ivars, device);
    ASSERT_EQ(device->GetRetainCount(), 2);
    ASSERT_EQ(graph.input->GetRetainCount(), 2);

    TearDownAudioGraph(*driver, *ivars);

    // Detached in ADK first: both streams off the device, the device off the
    // driver, and the device no longer pointing at these ivars.
    EXPECT_EQ(device->removedStreams.size(), 2U);
    ASSERT_EQ(driver->removedObjects.size(), 1U);
    EXPECT_EQ(driver->removedObjects[0], device.get());
    EXPECT_EQ(device->driverIvars, nullptr);
    EXPECT_EQ(device->driverIvarsWrites, 1U);

    // Then nothing is left in the ivars: the test holds the only reference.
    EXPECT_EQ(device->GetRetainCount(), 1);
    EXPECT_EQ(graph.input->GetRetainCount(), 1);
    EXPECT_EQ(graph.output->GetRetainCount(), 1);
    EXPECT_EQ(graph.inputMap->GetRetainCount(), 1);
    EXPECT_EQ(graph.outputMap->GetRetainCount(), 1);
    EXPECT_EQ(graph.controlMap->GetRetainCount(), 1);
    EXPECT_EQ(graph.control->GetRetainCount(), 1);
    EXPECT_FALSE(ivars->device.boolControls[0].valid);
    EXPECT_EQ(ivars->device.boolControlCount, 0U);
    EXPECT_EQ(ivars->device.audioNub, nullptr);
    EXPECT_FALSE(ivars->graphState.audioDeviceAdded);
}

// The shape behind the unplug leak. With the device left in the ivars (Stop
// before this change: RemoveObject only), the device keeps its driver alive
// for as long as the driver keeps the device, so neither is ever freed.
TEST(AudioDriverTeardownTests, ADeviceHoldingItsDriverNoLongerKeepsItAlive) {
    OSSharedPtr<ASFWAudioDriver> driver{new ASFWAudioDriver(), OSNoRetain};
    auto ivars = std::make_unique<ASFWAudioDriver_IVars>();
    StartedGraph graph;
    graph.AttachTo(*ivars, OSSharedPtr<ASFWAudioDevice>(new DeviceHoldingDriver(driver.get()), OSNoRetain));
    ASSERT_EQ(driver->GetRetainCount(), 2);  // The test's, and the device's.

    TearDownAudioGraph(*driver, *ivars);

    // The ivars held the device's last reference: dropping it freed the
    // device, which let go of the driver.
    EXPECT_EQ(driver->GetRetainCount(), 1);
}

// The whole Stop path: streaming stopped, every nub registration withdrawn,
// the actions and queues dropped, the graph torn down -- and a device that
// holds its driver no longer keeps it alive.
TEST(AudioDriverTeardownTests, StopLeavesTheDriverFreeable) {
    OSSharedPtr<ASFWAudioDriver> driver{new ASFWAudioDriver(), OSNoRetain};
    OSSharedPtr<ASFWAudioNub> nub{new ASFWAudioNub(), OSNoRetain};
    auto ivars = std::make_unique<ASFWAudioDriver_IVars>();
    StartedGraph graph;
    graph.AttachTo(*ivars, OSSharedPtr<ASFWAudioDevice>(new DeviceHoldingDriver(driver.get()), OSNoRetain));
    ivars->device.audioNub = nub.get();
    OSSharedPtr<OSAction> actions[] = {
        {new OSAction(), OSNoRetain}, {new OSAction(), OSNoRetain},
        {new OSAction(), OSNoRetain}, {new OSAction(), OSNoRetain},
        {new OSAction(), OSNoRetain}};
    OSSharedPtr<IODispatchQueue> queues[] = {{new IODispatchQueue(), OSNoRetain},
                                             {new IODispatchQueue(), OSNoRetain}};
    ivars->txPreparationAction = actions[0];
    ivars->ztsAnchorAction = actions[1];
    ivars->deviceClockChangedAction = actions[2];
    ivars->ioRestartRequiredAction = actions[3];
    ivars->controlChangeAction = actions[4];
    ivars->txPreparationQueue = queues[0];
    ivars->ztsQueue = queues[1];
    nub->RegisterTxPreparationAction(actions[0].get());
    nub->RegisterZtsAnchorAction(actions[1].get());
    nub->RegisterDeviceClockChangedAction(actions[2].get());
    nub->RegisterIoRestartRequiredAction(actions[3].get());
    nub->RegisterControlChangeAction(actions[4].get());
    ivars->runtime.isRunning.store(true);

    StopAudioDriverGraph(*driver, *ivars);

    EXPECT_EQ(nub->stopStreamingCalls, 1U);
    EXPECT_EQ(nub->txPreparationAction, nullptr);
    EXPECT_EQ(nub->ztsAnchorAction, nullptr);
    EXPECT_EQ(nub->deviceClockChangedAction, nullptr);
    EXPECT_EQ(nub->ioRestartRequiredAction, nullptr);
    EXPECT_EQ(nub->controlChangeAction, nullptr);
    for (const auto& action : actions) EXPECT_EQ(action->GetRetainCount(), 1);
    for (const auto& queue : queues) EXPECT_EQ(queue->GetRetainCount(), 1);
    EXPECT_FALSE(ivars->runtime.isRunning.load());
    EXPECT_EQ(ivars->device.audioNub, nullptr);
    EXPECT_EQ(graph.input->GetRetainCount(), 1);
    EXPECT_EQ(driver->GetRetainCount(), 1);
}

// A wire kept across StopIO gets no StopIO on unplug, so Stop asks the device
// to release it (Phase 88, 2026-10-08: unplug while retained logged no
// release and no free() until it did).
TEST(AudioDriverTeardownTests, StopReleasesAKeptWire) {
    OSSharedPtr<ASFWAudioDriver> driver{new ASFWAudioDriver(), OSNoRetain};
    auto ivars = std::make_unique<ASFWAudioDriver_IVars>();
    OSSharedPtr<ASFWAudioDevice> device{new ASFWAudioDevice(), OSNoRetain};
    StartedGraph graph;
    graph.AttachTo(*ivars, device);

    StopAudioDriverGraph(*driver, *ivars);

    EXPECT_EQ(device->keptWireReleaseRequests, 1U);
    EXPECT_EQ(ivars->audioDevice.get(), nullptr);
}

// Unplugged while CoreAudio plays: IO is still running when Stop() comes.
// Stop it first, while the device is attached, then release the wire (the
// stop keeps it) and only then remove the device. CoreAudio's own StopIO
// arrives after removal and no longer reaches the device: ADK's IO loop then
// spun at 62% CPU and nothing was freed (Pro 24 DSP, 2026-10-09).
TEST(AudioDriverTeardownTests, StopWithIoRunningStopsTheDeviceBeforeRemovingIt) {
    TeardownSequence() = 0;
    OSSharedPtr<ASFWAudioDriver> driver{new ASFWAudioDriver(), OSNoRetain};
    auto ivars = std::make_unique<ASFWAudioDriver_IVars>();
    OSSharedPtr<ASFWAudioDevice> device{new ASFWAudioDevice(), OSNoRetain};
    device->objectId = 7;
    StartedGraph graph;
    graph.AttachTo(*ivars, device);
    ivars->runtime.isRunning.store(true);

    StopAudioDriverGraph(*driver, *ivars);

    ASSERT_EQ(driver->stoppedDevices.size(), 1U);
    EXPECT_EQ(driver->stoppedDevices[0], 7U);
    EXPECT_LT(driver->stopDeviceAt, device->keptWireReleaseAt);
    EXPECT_LT(device->keptWireReleaseAt, driver->removedAt);
    EXPECT_EQ(ivars->audioDevice.get(), nullptr);
}

// IO already stopped (a kept wire, or never started): nothing to stop.
TEST(AudioDriverTeardownTests, StopWithIoStoppedDoesNotStopTheDevice) {
    OSSharedPtr<ASFWAudioDriver> driver{new ASFWAudioDriver(), OSNoRetain};
    auto ivars = std::make_unique<ASFWAudioDriver_IVars>();
    OSSharedPtr<ASFWAudioDevice> device{new ASFWAudioDevice(), OSNoRetain};
    StartedGraph graph;
    graph.AttachTo(*ivars, device);

    StopAudioDriverGraph(*driver, *ivars);

    EXPECT_TRUE(driver->stoppedDevices.empty());
    EXPECT_EQ(device->keptWireReleaseRequests, 1U);
}

// free() runs the same teardown after Stop already did; the second pass finds
// nothing attached and detaches nothing twice.
TEST(AudioDriverTeardownTests, ASecondTeardownDetachesNothing) {
    OSSharedPtr<ASFWAudioDriver> driver{new ASFWAudioDriver(), OSNoRetain};
    auto ivars = std::make_unique<ASFWAudioDriver_IVars>();
    OSSharedPtr<ASFWAudioDevice> device{new ASFWAudioDevice(), OSNoRetain};
    StartedGraph graph;
    graph.AttachTo(*ivars, device);

    TearDownAudioGraph(*driver, *ivars);
    TearDownAudioGraph(*driver, *ivars);

    EXPECT_EQ(driver->removedObjects.size(), 1U);
    EXPECT_EQ(device->removedStreams.size(), 2U);
    EXPECT_EQ(device->GetRetainCount(), 1);
}

TEST(AudioDriverTeardownTests, ConfirmedControlPushUpdatesHalWithoutAnotherHardwareWrite) {
    namespace C = ASFW::Audio::Controls;
    namespace AM = ASFW::AudioModel;
    OSSharedPtr<ASFWAudioDriver> driver{new ASFWAudioDriver(), OSNoRetain};
    OSSharedPtr<ASFWAudioNub> nub{new ASFWAudioNub(), OSNoRetain};
    auto ivars = std::make_unique<ASFWAudioDriver_IVars>();
    driver->ivars = ivars.get();
    ivars->audioDevice = OSSharedPtr<ASFWAudioDevice>(new ASFWAudioDevice(), OSNoRetain);
    ivars->device.guid = 42;
    ivars->device.audioNub = nub.get();
    ivars->controlChangeAction = OSSharedPtr<OSAction>(new OSAction(), OSNoRetain);
    const auto descriptor = [](uint32_t id, C::CoreAudioControlKind kind, AM::ParameterDomain domain,
                               AM::ParameterValue value) {
        return C::CoreAudioControl{AM::ParameterId{id}, kind, 0, 'outp', 0, true, "", std::move(domain), value};
    };
    auto& slots = ivars->device.parameterControls;
    slots.push_back({descriptor(512, C::CoreAudioControlKind::Level,
                                AM::ScalarDomain{-60, 0, 1.0 / 256, AM::ScalarUnit::Decibels}, -20.0)});
    slots.push_back({descriptor(513, C::CoreAudioControlKind::Boolean, AM::BooleanDomain{}, false)});
    slots.push_back({descriptor(514, C::CoreAudioControlKind::Selector,
                                AM::EnumDomain{{{0, "Stream"}, {1, "Mixer"}}}, int64_t{0})});
    slots[0].level = OSSharedPtr<IOUserAudioLevelControl>(new IOUserAudioLevelControl(), OSNoRetain);
    slots[1].boolean = OSSharedPtr<IOUserAudioBooleanControl>(new IOUserAudioBooleanControl(), OSNoRetain);
    slots[2].selector = OSSharedPtr<IOUserAudioSelectorControl>(new IOUserAudioSelectorControl(), OSNoRetain);
    auto level = slots[0].level;
    auto mute = slots[1].boolean;
    auto source = slots[2].selector;
    const auto deliver = [&](uint64_t guid, uint64_t revision, uint64_t sequence, uint32_t id,
                             uint64_t kind, uint64_t bits) {
        driver->ControlParameterChanged_Impl(ivars->controlChangeAction.get(), guid, revision,
                                             sequence, id, kind, bits, 0);
    };
    deliver(42, 1, 10, 512, 1, std::bit_cast<uint64_t>(-12.0));
    deliver(42, 1, 11, 513, 0, 1);
    deliver(42, 1, 12, 514, 2, 1);
    EXPECT_EQ(level->decibels, -12.0f);
    EXPECT_TRUE(mute->enabled);
    EXPECT_EQ(source->selected, 1U);
    EXPECT_EQ(level->updates, 1U);
    EXPECT_EQ(mute->updates, 1U);
    EXPECT_EQ(source->updates, 1U);
    EXPECT_EQ(nub->parameterWrites, 0U);
    deliver(42, 1, 9, 512, 1, std::bit_cast<uint64_t>(-30.0)); // late event
    deliver(42, 1, 0, 513, 0, 0); // registration baseline cannot replace live state
    deliver(43, 1, 13, 513, 0, 0); // another device
    deliver(42, 0, 14, 513, 0, 0); // old revision
    deliver(42, 1, 15, 512, 1, std::bit_cast<uint64_t>(10.0)); // readback outside the published range
    deliver(42, 1, 16, 999, 0, 1); // a parameter this device did not publish
    EXPECT_EQ(level->updates, 1U);
    EXPECT_EQ(mute->updates, 1U);

    // A state invalidation re-reads every control from the controller's state
    // instead of trusting a pushed value, and still never writes.
    nub->readValues[{512, false}] = {1, std::bit_cast<uint64_t>(-6.0)};
    nub->readValues[{513, false}] = {0, 0};
    nub->readValues[{514, false}] = {2, 0};
    deliver(42, 1, 20, 0, 4, 0);
    EXPECT_EQ(level->decibels, -6.0f);
    EXPECT_FALSE(mute->enabled);
    EXPECT_EQ(source->selected, 0U);
    EXPECT_EQ(nub->parameterWrites, 0U);

    StopAudioDriverGraph(*driver, *ivars);
    EXPECT_EQ(level->GetRetainCount(), 1);
    EXPECT_EQ(mute->GetRetainCount(), 1);
    EXPECT_EQ(source->GetRetainCount(), 1);
    deliver(42, 1, 21, 513, 0, 1); // a queued action after Stop is ignored
    EXPECT_FALSE(mute->enabled);
    driver->ivars = nullptr;
}
} // namespace

TEST(AudioDriverTeardownTests, AvcStreamsDropOwnerBeforeGraphRemoval) {
    OSSharedPtr<ASFWAudioDriver> driver{new ASFWAudioDriver(), OSNoRetain};
    auto ivars = std::make_unique<ASFWAudioDriver_IVars>();
    OSSharedPtr<ASFWAudioDevice> device{new ASFWAudioDevice(), OSNoRetain};
    OSSharedPtr<ASFWAvcAudioStream> input{new ASFWAvcAudioStream(), OSNoRetain};
    OSSharedPtr<ASFWAvcAudioStream> output{new ASFWAvcAudioStream(), OSNoRetain};
    input->Bind(device.get()); output->Bind(device.get());
    ivars->audioDevice = device;
    ivars->inputStream = OSSharedPtr<IOUserAudioStream>(input.get(), OSRetain);
    ivars->outputStream = OSSharedPtr<IOUserAudioStream>(output.get(), OSRetain);
    ivars->graphState = {.inputStreamAdded = true, .outputStreamAdded = true, .audioDeviceAdded = true};
    EXPECT_EQ(device->GetRetainCount(), 4);
    TearDownAudioGraph(*driver, *ivars);
    EXPECT_FALSE(input->owner); EXPECT_FALSE(output->owner);
    EXPECT_EQ(device->GetRetainCount(), 1);
}

TEST(RemoteDeviceStopResultTests, UnpublishedResultDoesNotAuthorizeLateStop) {
    std::atomic<uint64_t> slot{0};
    EXPECT_FALSE(ASFW::Audio::Runtime::RemoteDeviceStopResult::Read(slot));
}

TEST(RemoteDeviceStopResultTests, ResultSurvivesDetachmentAndRepeatedStops) {
    ASFWAudioNub nub;
    nub.RecordRemoteDeviceStopResult(kIOReturnSuccess);
    nub.rpcStopResult = kIOReturnIPCError; // Terminated service rejects RPC dispatch.
    EXPECT_EQ(nub.StopAudioStreamingOrRemoteResult(), kIOReturnSuccess);
    EXPECT_EQ(nub.StopAudioStreamingOrRemoteResult(), kIOReturnSuccess);
    ASFWAudioNub replacement;
    EXPECT_FALSE(ASFW::Audio::Runtime::RemoteDeviceStopResult::Read(replacement.remoteStopResult));
}

TEST(RemoteDeviceStopResultTests, FailedQuiescenceRemainsAFailureForLateStops) {
    ASFWAudioNub nub;
    for (const auto status : {kIOReturnTimeout, kIOReturnNotReady, kIOReturnDMAError}) {
        nub.RecordRemoteDeviceStopResult(status);
        EXPECT_EQ(nub.StopAudioStreamingOrRemoteResult(), status);
        EXPECT_EQ(nub.StopAudioStreamingOrRemoteResult(), status);
    }
}

TEST(RemoteDeviceStopResultTests, UnconfirmedStopStillPropagatesRpcFailure) {
    ASFWAudioNub nub;
    nub.rpcStopResult = kIOReturnIPCError;
    EXPECT_EQ(nub.StopAudioStreamingOrRemoteResult(), kIOReturnIPCError);
    EXPECT_EQ(nub.stopStreamingCalls, 1U);
}
