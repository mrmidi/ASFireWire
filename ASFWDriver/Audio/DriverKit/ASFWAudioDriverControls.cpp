//
// ASFWAudioDriverControls.cpp
// ASFWDriver
//
// Protocol-backed control forwarding for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "Runtime/OutputMutePolicy.hpp"

kern_return_t ASFWAudioDriver::ApplyProtocolBooleanControl(uint32_t classIdFourCC,
                                                           uint32_t element,
                                                           bool value)
{
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    return ivars->device.audioNub->SetProtocolBooleanControl(classIdFourCC, element, value);
}

kern_return_t ASFWAudioDriver::ReadProtocolBooleanControl(uint32_t classIdFourCC,
                                                          uint32_t element,
                                                          bool* outValue)
{
    if (!outValue) {
        return kIOReturnBadArgument;
    }
    *outValue = false;
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    return ivars->device.audioNub->GetProtocolBooleanControl(classIdFourCC, element, outValue);
}

// ---- Protocol-backed level controls ------------------------------------------------------

namespace {

constexpr uint32_t kLevelKind = static_cast<uint32_t>(ASFW::Audio::ControlKind::kLevel);

/// Knob tracking is a human-scale control; ten reads of shared memory a second is plenty.
constexpr uint64_t kControlSyncPeriodNs = 100'000'000;

/// After the host moves the control, the device keeps reporting the old knob position
/// until the coalesced write lands. Following it in that window would snap the slider back.
constexpr uint64_t kHostChangeHoldoffNs = 400'000'000;

[[nodiscard]] bool IsMasterOutputVolume(uint32_t classIdFourCC,
                                        uint32_t scopeFourCC,
                                        uint32_t element) noexcept {
    return classIdFourCC == ASFW::Audio::kControlClassVolume &&
           scopeFourCC == ASFW::Audio::kControlScopeOutput &&
           element == ASFW::Audio::kControlElementMain;
}

/// Send a level to the device without disturbing the volume control's own value, which is
/// what unmute comes back to. Stamps the host-change time so the knob reconciler holds off
/// while the write is in flight.
[[nodiscard]] kern_return_t WriteOutputLevelToDevice(ASFWAudioDriver_IVars& ivars,
                                                     float decibels) {
    if (!ivars.device.audioNub) {
        return kIOReturnNotReady;
    }
    ivars.runtime.lastHostLevelChangeTicks.store(mach_absolute_time(),
                                                 std::memory_order_relaxed);
    return ivars.device.audioNub->WriteProtocolControl(
        kLevelKind, ASFW::Audio::kControlClassVolume, ASFW::Audio::kControlScopeOutput,
        ASFW::Audio::kControlElementMain, ASFW::Audio::DecibelBits(decibels));
}

/// Drop a mute that no longer reflects the device, and tell the HAL.
void ClearOutputMuteControl(ASFWAudioDriver_IVars& ivars) {
    if (!ivars.runtime.outputMuted.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    if (ivars.device.outputMuteControl) {
        (void)ivars.device.outputMuteControl->SetControlValue(false);
    }
}

} // namespace

kern_return_t ASFWAudioDriver::DescribeProtocolLevelControl(uint32_t classIdFourCC,
                                                            uint32_t scopeFourCC,
                                                            uint32_t element,
                                                            bool* outSettable,
                                                            float* outMinDecibels,
                                                            float* outMaxDecibels)
{
    if (!outSettable || !outMinDecibels || !outMaxDecibels) {
        return kIOReturnBadArgument;
    }
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    bool settable = false;
    uint64_t rangeBits = 0;
    const kern_return_t status = ivars->device.audioNub->DescribeProtocolControl(
        kLevelKind, classIdFourCC, scopeFourCC, element, &settable, &rangeBits);
    if (status != kIOReturnSuccess) {
        return status;
    }
    *outSettable = settable;
    *outMinDecibels = ASFW::Audio::DecibelsFromBits(static_cast<uint32_t>(rangeBits >> 32));
    *outMaxDecibels = ASFW::Audio::DecibelsFromBits(static_cast<uint32_t>(rangeBits));
    return kIOReturnSuccess;
}

kern_return_t ASFWAudioDriver::ReadProtocolLevelControl(uint32_t classIdFourCC,
                                                        uint32_t scopeFourCC,
                                                        uint32_t element,
                                                        float* outDecibels)
{
    if (!outDecibels) {
        return kIOReturnBadArgument;
    }
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    uint64_t valueBits = 0;
    const kern_return_t status = ivars->device.audioNub->ReadProtocolControl(
        kLevelKind, classIdFourCC, scopeFourCC, element, &valueBits);
    if (status == kIOReturnSuccess) {
        *outDecibels = ASFW::Audio::DecibelsFromBits(static_cast<uint32_t>(valueBits));
    }
    return status;
}

kern_return_t ASFWAudioDriver::ApplyProtocolLevelControl(uint32_t classIdFourCC,
                                                         uint32_t scopeFourCC,
                                                         uint32_t element,
                                                         float decibels)
{
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    if (!IsMasterOutputVolume(classIdFourCC, scopeFourCC, element)) {
        return ivars->device.audioNub->WriteProtocolControl(
            kLevelKind, classIdFourCC, scopeFourCC, element,
            ASFW::Audio::DecibelBits(decibels));
    }

    // Moving the slider while muted unmutes, as it does everywhere else in macOS.
    ClearOutputMuteControl(*ivars);
    ivars->runtime.outputVolumeControlDbBits.store(ASFW::Audio::DecibelBits(decibels),
                                                   std::memory_order_relaxed);
    return WriteOutputLevelToDevice(*ivars, decibels);
}

kern_return_t ASFWAudioDriver::ApplyOutputMute(bool muted)
{
    if (!ivars || !ivars->device.audioNub) {
        return kIOReturnNotReady;
    }
    if (!ivars->device.outputVolumeControl) {
        return kIOReturnUnsupported; // nothing to attenuate
    }
    ivars->runtime.outputMuted.store(muted, std::memory_order_relaxed);
    const float controlDb = ASFW::Audio::DecibelsFromBits(
        ivars->runtime.outputVolumeControlDbBits.load(std::memory_order_relaxed));
    const float minDb = ASFW::Audio::DecibelsFromBits(
        ivars->runtime.outputVolumeMinDbBits.load(std::memory_order_relaxed));
    return WriteOutputLevelToDevice(
        *ivars, ASFW::Audio::DriverKit::LevelForMute(muted, controlDb, minDb));
}

void ASFWAudioDriver::ArmControlSyncTimer()
{
    if (!ivars || !ivars->controlSyncTimer) {
        return;
    }
    (void)ASFW::Timing::initializeHostTimebase();
    const uint64_t deadline =
        mach_absolute_time() + ASFW::Timing::nanosToHostTicks(kControlSyncPeriodNs);
    (void)ivars->controlSyncTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, deadline, 0);
}

void IMPL(ASFWAudioDriver, ControlSyncTimerFired)
{
    (void)action;
    (void)time;
    if (!ivars) {
        return;
    }
    ASFW::Audio::DriverKit::ReconcileReportedOutputLevel(*ivars);
    // Knob reports only arrive while the device streams, so stop when it stops;
    // StartDevice re-arms.
    if (ivars->runtime.isRunning.load(std::memory_order_acquire)) {
        ArmControlSyncTimer();
    }
}

namespace ASFW::Audio::DriverKit {

void ReconcileReportedOutputLevel(ASFWAudioDriver_IVars& ivars) noexcept {
    auto* control = ivars.device.outputVolumeControl.get();
    auto* block = ivars.runtime.directAudioGraph.control;
    if (control == nullptr || block == nullptr) {
        return;
    }
    const uint32_t encoded = block->deviceReportedOutputLevel.load(std::memory_order_relaxed);
    if (encoded == ivars.runtime.lastAppliedReportedLevel) {
        return;
    }
    float reportedDb = 0.0f;
    if (!ASFW::Audio::Runtime::DecodeReportedLevel(encoded, reportedDb)) {
        return;
    }
    const uint64_t lastHostChange =
        ivars.runtime.lastHostLevelChangeTicks.load(std::memory_order_relaxed);
    const uint64_t now = mach_absolute_time();
    if (lastHostChange != 0 && now > lastHostChange &&
        ASFW::Timing::hostTicksToNanos(now - lastHostChange) < kHostChangeHoldoffNs) {
        return; // our own write is still settling; look again next tick
    }
    ivars.runtime.lastAppliedReportedLevel = encoded;

    const float controlDb = ASFW::Audio::DecibelsFromBits(
        ivars.runtime.outputVolumeControlDbBits.load(std::memory_order_relaxed));
    const float minDb = ASFW::Audio::DecibelsFromBits(
        ivars.runtime.outputVolumeMinDbBits.load(std::memory_order_relaxed));
    const DeviceReportDecision decision =
        DecideDeviceReport(ivars.runtime.outputMuted.load(std::memory_order_relaxed),
                           reportedDb, controlDb, minDb);
    if (decision.clearMute) {
        ClearOutputMuteControl(ivars);
    }
    if (!decision.applyToControl) {
        return; // our own mute echoed back, or the level the host already set
    }
    // SetDecibelValue notifies the HAL without re-entering HandleChange*, so this does not
    // write the value back to the device.
    if (control->SetDecibelValue(reportedDb) == kIOReturnSuccess) {
        ivars.runtime.outputVolumeControlDbBits.store(ASFW::Audio::DecibelBits(reportedDb),
                                                      std::memory_order_relaxed);
        ASFW_LOG(Audio, "ASFWAudioDriver: output volume follows the device knob centiDb=%d",
                 static_cast<int>(reportedDb * 100.0f));
    }
}

} // namespace ASFW::Audio::DriverKit
