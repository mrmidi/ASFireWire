#include "ASFWAvcAudioStream.h"
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

void TearDownAudioGraph(ASFWAudioDriver& driver, ASFWAudioDriver_IVars& ivars,
                        AudioGraphDetach detach) noexcept {
    ivars.runtime.isRunning.store(false, std::memory_order_release);
    UnbindMidiTx(ivars);
    UnbindDirectAudioSkeleton(ivars);

    if (auto* stream = OSDynamicCast(ASFWAvcAudioStream, ivars.inputStream.get())) stream->Bind(nullptr);
    if (auto* stream = OSDynamicCast(ASFWAvcAudioStream, ivars.outputStream.get())) stream->Bind(nullptr);
    auto& state = ivars.graphState;
    const char* const mode = detach == AudioGraphDetach::kDeviceOnly ? "device-only" : "streams-then-device";
    if (ivars.audioDevice) {
        // One line before each ADK call: a call that never returns is the
        // last line logged.
        ASFW_LOG(Audio,
                 "[Lifecycle] ASFWAudioDriver graph detach begin mode=%{public}s device=%p in=%d out=%d added=%d",
                 mode, ivars.audioDevice.get(), state.inputStreamAdded, state.outputStreamAdded,
                 state.audioDeviceAdded);
        if (detach == AudioGraphDetach::kStreamsThenDevice) {
            if (state.outputStreamAdded && ivars.outputStream) {
                ASFW_LOG(Audio, "[Lifecycle] ASFWAudioDriver RemoveStream output stream=%p",
                         ivars.outputStream.get());
                const kern_return_t kr = ivars.audioDevice->RemoveStream(ivars.outputStream.get());
                ASFW_LOG(Audio, "[Lifecycle] ASFWAudioDriver RemoveStream output kr=0x%x", kr);
            }
            if (state.inputStreamAdded && ivars.inputStream) {
                ASFW_LOG(Audio, "[Lifecycle] ASFWAudioDriver RemoveStream input stream=%p",
                         ivars.inputStream.get());
                const kern_return_t kr = ivars.audioDevice->RemoveStream(ivars.inputStream.get());
                ASFW_LOG(Audio, "[Lifecycle] ASFWAudioDriver RemoveStream input kr=0x%x", kr);
            }
        }
        if (state.audioDeviceAdded) {
            ASFW_LOG(Audio, "[Lifecycle] ASFWAudioDriver RemoveObject device=%p", ivars.audioDevice.get());
            const kern_return_t kr = driver.RemoveObject(ivars.audioDevice.get());
            ASFW_LOG(Audio, "[Lifecycle] ASFWAudioDriver RemoveObject device kr=0x%x", kr);
        }
        // ADK may keep the device past this driver; it must not reach these
        // ivars after they are gone.
        ivars.audioDevice->SetDriverIvars(nullptr);
    }
    state = {};

    ivars.device.parameterControls.clear();
    ivars.device.controlRevision = 0;
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
    ASFW_LOG(Audio, "[Lifecycle] ASFWAudioDriver graph released mode=%{public}s", mode);
}

void StopAudioDriverGraph(ASFWAudioDriver& driver, ASFWAudioDriver_IVars& ivars) noexcept {
    // IO still running: the device left mid-stream. Stop it here, while the
    // device is still attached. CoreAudio's own StopIO arrives after the
    // device is removed below and no longer reaches it, so ADK's IO loop kept
    // cycling with no timestamps (62% CPU for minutes) and neither the driver
    // nor the nub was freed (Pro 24 DSP unplug while playing, 2026-10-09).
    // super::StopDevice stops ADK's IO state and calls our StopIO.
    if (ivars.audioDevice && ivars.runtime.isRunning.load(std::memory_order_acquire)) {
        const uint32_t deviceId = ivars.audioDevice->GetObjectID();
        ASFW_LOG(Audio, "ASFWAudioDriver: Stop() with IO running; stopping device %u first",
                 deviceId);
        const kern_return_t kr = driver.StopDevice(deviceId, IOUserAudioStartStopFlags::None);
        ASFW_LOG(Audio, "[Lifecycle] ASFWAudioDriver StopDevice returned device=%u kr=0x%x", deviceId, kr);
    }
    // A wire kept across StopIO (including by the stop just above) gets no
    // StopIO on unplug: release it the way StopIO does, or the TX isoch
    // resources and their mappings outlive the device (Phase 88 unplug while
    // retained, 2026-10-08: no release, no free()).
    if (ivars.audioDevice) {
        (void)ivars.audioDevice->ReleaseKeptWireForDriverStop();
    }
    ivars.runtime.isRunning.store(false, std::memory_order_release);
    // A wire kept across StopIO (Runtime/WireRetention.hpp) still has its TX
    // producer armed: stop it and wait out a pass in flight before the stream
    // stops and the slabs go. Harmless when IO already
    // stopped it.
    ivars.runtime.txActive.store(false, std::memory_order_release);
    if (ivars.txPreparationQueue) {
        ASFW_LOG(Audio, "[Lifecycle] ASFWAudioDriver TX preparation queue drain begin");
        ivars.txPreparationQueue->DispatchSync(^{ });
        ASFW_LOG(Audio, "[Lifecycle] ASFWAudioDriver TX preparation queue drained");
    }
    if (auto* nub = ivars.device.audioNub) {
        const kern_return_t stopKr = nub->StopAudioStreamingOrRemoteResult();
        if (stopKr != kIOReturnSuccess) {
            ASFW_LOG(Audio, "ASFWAudioDriver: StopAudioStreaming failed in Stop(): 0x%x (%{public}s)", stopKr, ASFW::Logging::IOReturnName(stopKr));
        }
        (void)nub->RegisterTxPreparationAction(nullptr);
        (void)nub->RegisterZtsAnchorAction(nullptr);
        (void)nub->RegisterDeviceClockChangedAction(nullptr);
        (void)nub->RegisterIoRestartRequiredAction(nullptr);
        (void)nub->RegisterControlChangeAction(nullptr);
        ASFW_LOG(Audio, "[Lifecycle] ASFWAudioDriver nub actions withdrawn nub=%p", nub);
    }
    ivars.txPreparationAction.reset();
    ivars.txPreparationQueue.reset();
    ivars.deviceClockChangedAction.reset();
    ivars.ioRestartRequiredAction.reset();
    ivars.controlChangeAction.reset();
    ivars.ztsAnchorAction.reset();
    ivars.ztsQueue.reset();
    // Detach and drop the whole ADK graph here, not in free(). The device is
    // created with this driver (ASFWAudioDevice::init(&driver)), so a device
    // still held by the ivars can keep the driver from ever being freed, and
    // with it the dext process alive after an unplug (2026-10-02).
    TearDownAudioGraph(driver, ivars, AudioGraphDetach::kDeviceOnly);
}

} // namespace ASFW::Audio::DriverKit
