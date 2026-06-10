#include <new>
#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include "VirtualAudioDevice.h"
#include "../Core/VirtualAudioDeviceController.hpp"

using namespace ASFW::Driver;

struct VirtualAudioDevice_IVars
{
    OSSharedPtr<IOUserAudioDriver> driver;
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<IOUserAudioStream> outputStream;
    OSSharedPtr<IOMemoryMap> outputMemoryMap;

    VirtualAudioDeviceController* controller{nullptr};

    uint32_t outputBytesPerFrame{0};
    uint32_t outputChannels{0};
};

bool VirtualAudioDevice::init(IOUserAudioDriver* in_driver,
                               bool in_supports_prewarming,
                               OSString* in_device_uid,
                               OSString* in_model_uid,
                               OSString* in_manufacturer_uid,
                               uint32_t in_zero_timestamp_period)
{
    if (!super::init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period)) {
        return false;
    }
    
    ivars = IONewZero(VirtualAudioDevice_IVars, 1);
    if (ivars == nullptr) {
        return false;
    }
    
    ivars->driver = OSSharedPtr(in_driver, OSRetain);
    ivars->workQueue = GetWorkQueue();
    
    IOLog("VirtualAudioDevice: init\n");
    
    ivars->controller = new VirtualAudioDeviceController();
    if (!ivars->controller->Initialize()) {
        IOLog("VirtualAudioDevice: Failed to initialize controller\n");
        return false;
    }

    // Pick Saffire for testing
    ASFW::Protocols::Audio::DICE::DiceDeviceIdentity identity{};
    identity.vendorId = 0x00130e; // Focusrite
    ivars->controller->SelectProfile(identity);

    // Set up a basic float32 output stream (2 channels, 48kHz)
    double sampleRate = 48000.0;
    SetAvailableSampleRates(&sampleRate, 1);
    SetSampleRate(sampleRate);
    
    IOUserAudioStreamBasicDescription format = {
        .mSampleRate = sampleRate,
        .mFormatID = IOUserAudioFormatID::LinearPCM,
        .mFormatFlags = static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsFloat | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
        .mBytesPerPacket = 8,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 8,
        .mChannelsPerFrame = 2,
        .mBitsPerChannel = 32
    };
    
    ivars->outputBytesPerFrame = format.mBytesPerFrame;
    ivars->outputChannels = format.mChannelsPerFrame;

    OSSharedPtr<IOBufferMemoryDescriptor> buffer;
    uint32_t bufferSize = in_zero_timestamp_period * format.mBytesPerFrame;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, bufferSize, 0, buffer.attach()) == kIOReturnSuccess) {
        ivars->outputStream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Output, buffer.get());
        if (ivars->outputStream) {
            ivars->outputStream->SetAvailableStreamFormats(&format, 1);
            ivars->outputStream->SetCurrentStreamFormat(&format);
            AddStream(ivars->outputStream.get());
        }
    }
    
    ivars->controller->ConfigureOutputStream(48000, 2, in_zero_timestamp_period);
    
    auto ivarsPtr = ivars;
    
    auto io_operation = ^kern_return_t(IOUserAudioObjectID in_device,
                                      IOUserAudioIOOperation in_io_operation,
                                      uint32_t in_io_buffer_frame_size,
                                      uint64_t in_sample_time,
                                      uint64_t in_host_time)
    {
        if (in_io_operation == IOUserAudioIOOperationWriteEnd) {
            if (ivarsPtr->controller && ivarsPtr->outputMemoryMap) {
                float* floatBuffer = reinterpret_cast<float*>(ivarsPtr->outputMemoryMap->GetAddress() + ivarsPtr->outputMemoryMap->GetOffset());
                uint32_t ringFrames = static_cast<uint32_t>(ivarsPtr->outputMemoryMap->GetLength() / ivarsPtr->outputBytesPerFrame);
                uint32_t offsetFrames = static_cast<uint32_t>(in_sample_time % ringFrames);

                ASFW::Protocols::Audio::AMDTP::HostAudioBufferView outputView {
                    .interleavedFloat32 = &floatBuffer[offsetFrames * ivarsPtr->outputChannels],
                    .firstFrame = in_sample_time,
                    .frameCount = in_io_buffer_frame_size,
                    .frameCapacity = ringFrames,
                    .channels = ivarsPtr->outputChannels
                };

                ivarsPtr->controller->SubmitWriteEnd(outputView);
            }
        }
        return kIOReturnSuccess;
    };
    
    SetIOOperationHandler(io_operation);
    
    return true;
}

void VirtualAudioDevice::free()
{
    if (ivars != nullptr) {
        if (ivars->controller) {
            delete ivars->controller;
        }
        ivars->driver.reset();
        ivars->workQueue.reset();
        ivars->outputStream.reset();
        ivars->outputMemoryMap.reset();
    }
    IOSafeDeleteNULL(ivars, VirtualAudioDevice_IVars, 1);
    super::free();
}

kern_return_t VirtualAudioDevice::StartIO(IOUserAudioStartStopFlags in_flags)
{
    IOLog("VirtualAudioDevice: StartIO\n");
    
    __block kern_return_t kr = kIOReturnSuccess;
    ivars->workQueue->DispatchSync(^(){
        kr = super::StartIO(in_flags);
        if (kr == kIOReturnSuccess && ivars->outputStream) {
            auto buffer = ivars->outputStream->GetIOMemoryDescriptor();
            if (buffer) {
                buffer->CreateMapping(0, 0, 0, 0, 0, ivars->outputMemoryMap.attach());
            }
            if (ivars->controller) {
                ivars->controller->ResetTransportLab(0, 0);
            }
        }
    });
    
    return kr;
}

kern_return_t VirtualAudioDevice::StopIO(IOUserAudioStartStopFlags in_flags)
{
    IOLog("VirtualAudioDevice: StopIO\n");
    
    __block kern_return_t kr = kIOReturnSuccess;
    ivars->workQueue->DispatchSync(^(){
        kr = super::StopIO(in_flags);
        ivars->outputMemoryMap.reset();
    });

    return kr;
}

kern_return_t VirtualAudioDevice::PerformDeviceConfigurationChange(uint64_t change_action,
                                                                   OSObject* in_change_info)
{
    return super::PerformDeviceConfigurationChange(change_action, in_change_info);
}

kern_return_t VirtualAudioDevice::AbortDeviceConfigurationChange(uint64_t change_action,
                                                                 OSObject* in_change_info)
{
    return super::AbortDeviceConfigurationChange(change_action, in_change_info);
}

kern_return_t VirtualAudioDevice::HandleChangeSampleRate(double in_sample_rate)
{
    return SetSampleRate(in_sample_rate);
}
