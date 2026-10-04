//
// ASFWAudioDriverTeardown.cpp
// ASFWDriver
//
// ADK graph teardown for ASFWAudioDriver, shared by a failed Start and by Stop.
// Kept apart from graph construction so the host suite can check that a
// teardown leaves the driver holding no ADK object.
//
#include "ASFWAudioDevice.h"
#include "ASFWAudioDriverPrivate.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Audio::DriverKit {

void UnbindDirectAudioSkeleton(ASFWAudioDriver_IVars& ivars) noexcept {
    ivars.runtime.directAudioSkeletonBound.store(false, std::memory_order_release);
    ivars.runtime.directAudioGraph = {};
    ivars.runtime.lastHalZeroTimestampGeneration.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampSampleFrame.store(0, std::memory_order_release);
    ivars.runtime.lastHalZeroTimestampHostTicks.store(0, std::memory_order_release);
}

void TearDownAudioGraph(ASFWAudioDriver& driver, ASFWAudioDriver_IVars& ivars) noexcept {
    ivars.runtime.isRunning.store(false, std::memory_order_release);
    UnbindDirectAudioSkeleton(ivars);

    auto& state = ivars.graphState;
    if (ivars.audioDevice) {
        if (state.outputStreamAdded && ivars.outputStream) {
            (void)ivars.audioDevice->RemoveStream(ivars.outputStream.get());
        }
        if (state.inputStreamAdded && ivars.inputStream) {
            (void)ivars.audioDevice->RemoveStream(ivars.inputStream.get());
        }
        if (state.audioDeviceAdded) {
            (void)driver.RemoveObject(ivars.audioDevice.get());
        }
        // ADK may keep the device past this driver; it must not reach these
        // ivars after they are gone.
        ivars.audioDevice->SetDriverIvars(nullptr);
    }
    state = {};

    ivars.device.avcControlCount = 0;
    ivars.device.boolControlCount = 0;
    ASFW::Isoch::Audio::ResetBoolControlSlots(ivars.device.boolControls,
                                              ASFW::Isoch::Audio::kMaxBoolControls);
    ivars.outputStream.reset();
    ivars.inputStream.reset();
    ivars.outputMap.reset();
    ivars.inputMap.reset();
    ivars.controlMap.reset();
    ivars.outputBuffer.reset();
    ivars.inputBuffer.reset();
    ivars.controlBuffer.reset();
    ivars.audioDevice.reset();
    ivars.workQueue.reset();
    ivars.device.audioNub = nullptr;
}

void StopAudioDriverGraph(ASFWAudioDriver& driver, ASFWAudioDriver_IVars& ivars) noexcept {
    ivars.runtime.isRunning.store(false, std::memory_order_release);
    if (auto* nub = ivars.device.audioNub) {
        const kern_return_t stopKr = nub->StopAudioStreaming();
        if (stopKr != kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioDriver: StopAudioStreaming failed in Stop(): 0x%x", stopKr);
        }
        (void)nub->RegisterTxPreparationAction(nullptr);
        (void)nub->RegisterZtsAnchorAction(nullptr);
        (void)nub->RegisterDeviceClockChangedAction(nullptr);
        (void)nub->RegisterIoRestartRequiredAction(nullptr);
    }
    ivars.txPreparationAction.reset();
    ivars.txPreparationQueue.reset();
    ivars.deviceClockChangedAction.reset();
    ivars.ioRestartRequiredAction.reset();
    ivars.ztsAnchorAction.reset();
    ivars.ztsQueue.reset();
    // Detach and drop the whole ADK graph here, not in free(). The device is
    // created with this driver (ASFWAudioDevice::init(&driver)), so a device
    // still held by the ivars can keep the driver from ever being freed, and
    // with it the dext process alive after an unplug (2026-10-02).
    TearDownAudioGraph(driver, ivars);
}

} // namespace ASFW::Audio::DriverKit
