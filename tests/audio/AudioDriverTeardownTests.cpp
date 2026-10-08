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
        {new OSAction(), OSNoRetain}, {new OSAction(), OSNoRetain}};
    OSSharedPtr<IODispatchQueue> queues[] = {{new IODispatchQueue(), OSNoRetain},
                                             {new IODispatchQueue(), OSNoRetain}};
    ivars->txPreparationAction = actions[0];
    ivars->ztsAnchorAction = actions[1];
    ivars->deviceClockChangedAction = actions[2];
    ivars->ioRestartRequiredAction = actions[3];
    ivars->txPreparationQueue = queues[0];
    ivars->ztsQueue = queues[1];
    nub->RegisterTxPreparationAction(actions[0].get());
    nub->RegisterZtsAnchorAction(actions[1].get());
    nub->RegisterDeviceClockChangedAction(actions[2].get());
    nub->RegisterIoRestartRequiredAction(actions[3].get());
    ivars->runtime.isRunning.store(true);

    StopAudioDriverGraph(*driver, *ivars);

    EXPECT_EQ(nub->stopStreamingCalls, 1U);
    EXPECT_EQ(nub->txPreparationAction, nullptr);
    EXPECT_EQ(nub->ztsAnchorAction, nullptr);
    EXPECT_EQ(nub->deviceClockChangedAction, nullptr);
    EXPECT_EQ(nub->ioRestartRequiredAction, nullptr);
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
