//
// ASFWIOUserAudioDevice.cpp
// ASFWDriver
//
// Implementation of custom IOUserAudioDevice subclass.
// HandleChangeSampleRate orchestrates AV/C sample rate switching
// + isoch stream restart synchronously (≤1s total).
//

#include "ASFWIOUserAudioDevice.h"
#include "ASFWAudioNub.h"
#include "ASFWDriver.h"
#include "../../Audio/AudioCoordinator.hpp"
#include "../../Controller/ControllerCore.hpp"
#include "../../Service/DriverContext.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSMetaClass.h>
#include <atomic>

OSSharedPtr<ASFWIOUserAudioDevice> ASFWIOUserAudioDevice::Create(
    IOService* ownerDriver,
    bool       inPrewarm,
    OSString*  inDeviceUID,
    OSString*  inModelUID,
    OSString*  inManufacturerUID,
    uint32_t   inZeroTimestampPeriod)
{
    auto* device = OSTypeAlloc(ASFWIOUserAudioDevice);
    if (!device) {
        return nullptr;
    }

    if (!device->init(ownerDriver, inPrewarm, inDeviceUID, inModelUID, inManufacturerUID, inZeroTimestampPeriod)) {
        device->release();
        return nullptr;
    }

    return OSSharedPtr(device, OSNoRetain);
}

bool ASFWIOUserAudioDevice::init(
    IOService* ownerDriver,
    bool       inPrewarm,
    OSString*  inDeviceUID,
    OSString*  inModelUID,
    OSString*  inManufacturerUID,
    uint32_t   inZeroTimestampPeriod)
{
    if (!super::init(ownerDriver, inPrewarm, inDeviceUID, inModelUID, inManufacturerUID, inZeroTimestampPeriod)) {
        return false;
    }
    ivars = IONewZero(ASFWIOUserAudioDevice_IVars, 1);
    return ivars != nullptr;
}

void ASFWIOUserAudioDevice::free()
{
    if (ivars) {
        IOSafeDeleteNULL(ivars, ASFWIOUserAudioDevice_IVars, 1);
    }
    super::free();
}

kern_return_t ASFWIOUserAudioDevice::SetStreamingContext(ASFWAudioNub* nub, uint64_t guid)
{
    if (!ivars) return kIOReturnNotReady;
    ivars->nub  = nub;
    ivars->guid = guid;
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWIOUserAudioDevice, HandleChangeSampleRate)
{
    if (!ivars || !ivars->nub || ivars->guid == 0) {
        ASFW_LOG_WARNING(Audio, "ASFWIOUserAudioDevice: HandleChangeSampleRate called before SetStreamingContext");
        return HandleChangeSampleRate(in_sample_rate, SUPERDISPATCH);
    }

    const auto rateHz = static_cast<uint32_t>(in_sample_rate);
    ASFW_LOG(Audio,
             "ASFWIOUserAudioDevice: HandleChangeSampleRate %.0f Hz (GUID=0x%016llx)",
             in_sample_rate, ivars->guid);

    // ── Resolve coordinator + discovery ────────────────────────────────────
    ASFWDriver* parentDriver = ivars->nub->GetParentDriver();
    if (!parentDriver) {
        ASFW_LOG_WARNING(Audio, "ASFWIOUserAudioDevice: no parent driver — skipping AV/C rate change");
        return HandleChangeSampleRate(in_sample_rate, SUPERDISPATCH);
    }

    auto* ctx = static_cast<ServiceContext*>(parentDriver->GetServiceContext());
    if (!ctx || !ctx->audioCoordinator) {
        ASFW_LOG_WARNING(Audio, "ASFWIOUserAudioDevice: no AudioCoordinator — skipping AV/C rate change");
        return HandleChangeSampleRate(in_sample_rate, SUPERDISPATCH);
    }
    ASFW::Audio::AudioCoordinator* coordinator = ctx->audioCoordinator.get();

    auto* core = static_cast<ASFW::Driver::ControllerCore*>(parentDriver->GetControllerCore());
    if (!core) {
        ASFW_LOG_WARNING(Audio, "ASFWIOUserAudioDevice: no ControllerCore — skipping AV/C rate change");
        return HandleChangeSampleRate(in_sample_rate, SUPERDISPATCH);
    }
    ASFW::Protocols::AVC::IAVCDiscovery* discovery = core->GetAVCDiscovery();
    if (!discovery) {
        ASFW_LOG_WARNING(Audio, "ASFWIOUserAudioDevice: no IAVCDiscovery — skipping AV/C rate change");
        return HandleChangeSampleRate(in_sample_rate, SUPERDISPATCH);
    }

    // ── 1. Stop isoch streaming ────────────────────────────────────────────
    (void)coordinator->StopStreaming(ivars->guid);

    // ── 2. Send AV/C INPUT PLUG SIGNAL FORMAT (0x19) ──────────────────────
    std::atomic<bool>  avcDone{false};
    std::atomic<bool>  avcAccepted{false};
    const uint64_t     captureGuid = ivars->guid;

    discovery->SendSampleRateCommand(
        captureGuid,
        rateHz,
        [&avcDone, &avcAccepted](bool accepted) {
            avcAccepted.store(accepted, std::memory_order_release);
            avcDone.store(true,     std::memory_order_release);
        });

    // Poll ≤ 500 ms (AV/C FCP round-trip is typically < 50 ms).
    constexpr uint32_t kPollMs    = 5;
    constexpr uint32_t kTimeoutMs = 500;
    for (uint32_t waited = 0; waited < kTimeoutMs; waited += kPollMs) {
        if (avcDone.load(std::memory_order_acquire)) break;
        IOSleep(kPollMs);
    }

    if (!avcDone.load(std::memory_order_acquire)) {
        ASFW_LOG_WARNING(Audio,
                         "ASFWIOUserAudioDevice: AV/C rate change timed out (%.0f Hz, GUID=0x%016llx)",
                         in_sample_rate, captureGuid);
    } else if (!avcAccepted.load(std::memory_order_acquire)) {
        ASFW_LOG_WARNING(Audio,
                         "ASFWIOUserAudioDevice: device rejected rate %.0f Hz (GUID=0x%016llx)",
                         in_sample_rate, captureGuid);
    }

    // ── 3. Tell HAL / CoreAudio the new rate ──────────────────────────────
    kern_return_t kr = HandleChangeSampleRate(in_sample_rate, SUPERDISPATCH);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_WARNING(Audio,
                         "ASFWIOUserAudioDevice: super::HandleChangeSampleRate failed kr=0x%x", kr);
    }

    // ── 4. Restart isoch streaming at new rate ────────────────────────────
    const kern_return_t startKr = coordinator->StartStreaming(captureGuid);
    if (startKr != kIOReturnSuccess) {
        ASFW_LOG_WARNING(Audio,
                         "ASFWIOUserAudioDevice: StartStreaming failed after rate change kr=0x%x", startKr);
    }

    ASFW_LOG(Audio,
             "ASFWIOUserAudioDevice: sample rate change to %.0f Hz complete (GUID=0x%016llx)",
             in_sample_rate, captureGuid);
    return kr;
}
