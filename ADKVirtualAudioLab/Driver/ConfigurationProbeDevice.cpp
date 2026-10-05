// SPDX-License-Identifier: Apache-2.0
// Silent HAL lifecycle experiment; no FireWire, codec, or production policy.
#include <new>
#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <os/log.h>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include "ConfigurationProbeDevice.h"
#include "ConfigurationProbeStream.h"
#include "../Core/LabAudioGeometry.hpp"

#define PROBE_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[ADKConfigProbe] " fmt, ##__VA_ARGS__)
namespace {
constexpr std::array<uint32_t, 7> rates{32000, 44100, 48000, 88200, 96000, 176400, 192000};
constexpr uint32_t capacityFrames = 49152;
constexpr uint32_t capacityChannels = 16;
constexpr uint64_t actionTag = uint64_t{0xCA} << 56;
uint32_t Channels(uint32_t rate) { return rate > 96000 ? 8 : rate > 48000 ? 12 : 16; }
bool Supported(double rate) {
    for (auto value : rates) if (rate == value) return true;
    return false;
}
IOUserAudioStreamBasicDescription Format(uint32_t rate) {
    return {.mSampleRate = double(rate), .mFormatID = IOUserAudioFormatID::LinearPCM,
        .mFormatFlags = static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsFloat |
            IOUserAudioFormatFlags::FormatFlagsNativeEndian),
        .mBytesPerPacket = Channels(rate) * 4, .mFramesPerPacket = 1,
        .mBytesPerFrame = Channels(rate) * 4, .mChannelsPerFrame = Channels(rate), .mBitsPerChannel = 32};
}
}
struct ProbeIOState {
    std::atomic<uint64_t> calls{0}, late{0};
    std::atomic<bool> running{false};
};
struct ConfigurationProbeDevice_IVars {
    OSSharedPtr<IODispatchQueue> queue;
    OSSharedPtr<ConfigurationProbeStream> output, input;
    OSSharedPtr<IOBufferMemoryDescriptor> outputMemory, inputMemory;
    OSSharedPtr<IOTimerDispatchSource> timer;
    OSSharedPtr<OSAction> action;
    std::atomic<uint64_t> pending{0}, nextToken{1};
    std::atomic<bool> running{false}, projecting{false}, unavailable{false};
    std::atomic<bool> shuttingDown{false};
    std::shared_ptr<ProbeIOState> ioState;
    std::atomic<uint32_t> rate{48000};
    uint64_t epoch{0}, periodIndex{0};
    mach_timebase_info_data_t timebase{};
};
bool ConfigurationProbeDevice::init(IOUserAudioDriver* driver, bool prewarming,
    OSString* uid, OSString* model, OSString* manufacturer, uint32_t period) {
    if (!super::init(driver, prewarming, uid, model, manufacturer, period)) return false;
    ivars = IONewZero(ConfigurationProbeDevice_IVars, 1);
    if (!ivars) return false;
    ivars->queue = driver->GetWorkQueue();
    if (mach_timebase_info(&ivars->timebase) != KERN_SUCCESS ||
        !ivars->timebase.numer || !ivars->timebase.denom || !ivars->queue) return false;
    std::array<double, 7> available{};
    std::array<IOUserAudioStreamBasicDescription, 7> formats{};
    for (size_t i = 0; i < rates.size(); ++i) { available[i] = rates[i]; formats[i] = Format(rates[i]); }
    if (SetAvailableSampleRates(available.data(), available.size()) != kIOReturnSuccess ||
        SetSampleRate(48000) != kIOReturnSuccess) return false;
    const auto makeStream = [&](IOUserAudioStreamDirection direction,
        OSSharedPtr<IOBufferMemoryDescriptor>& memory, OSSharedPtr<ConfigurationProbeStream>& stream) {
        if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
            capacityFrames * capacityChannels * sizeof(float), 0, memory.attach()) != kIOReturnSuccess) return false;
        OSSharedPtr<IOMemoryMap> mapping;
        if (memory->CreateMapping(0, 0, 0, 0, 0, mapping.attach()) != kIOReturnSuccess) return false;
        memset(reinterpret_cast<void*>(mapping->GetAddress()), 0, mapping->GetLength());
        stream = OSSharedPtr(OSTypeAlloc(ConfigurationProbeStream), OSNoRetain);
        if (!stream || !stream->init(driver, direction, memory.get())) return false;
        stream->Bind(this);
        auto initial = Format(48000);
        return stream->SetAvailableStreamFormats(formats.data(), formats.size()) == kIOReturnSuccess &&
            stream->SetCurrentStreamFormat(&initial) == kIOReturnSuccess && AddStream(stream.get()) == kIOReturnSuccess;
    };
    if (!makeStream(IOUserAudioStreamDirection::Output, ivars->outputMemory, ivars->output) ||
        !makeStream(IOUserAudioStreamDirection::Input, ivars->inputMemory, ivars->input)) return false;
    IOTimerDispatchSource* timer = nullptr;
    if (IOTimerDispatchSource::Create(ivars->queue.get(), &timer) != kIOReturnSuccess) return false;
    ivars->timer = OSSharedPtr(timer, OSNoRetain);
    OSAction* action = nullptr;
    if (CreateActionTick(0, &action) != kIOReturnSuccess) return false;
    ivars->action = OSSharedPtr(action, OSNoRetain);
    if (ivars->timer->SetHandler(action) != kIOReturnSuccess) return false;
    ivars->ioState = std::make_shared<ProbeIOState>();
    auto state = ivars->ioState;
    if (SetIOOperationHandler(^kern_return_t(IOUserAudioObjectID, IOUserAudioIOOperation,
        uint32_t, uint64_t, uint64_t) {
        state->calls.fetch_add(1, std::memory_order_relaxed);
        if (!state->running.load(std::memory_order_acquire)) state->late.fetch_add(1, std::memory_order_relaxed);
        return kIOReturnSuccess;
    }) != kIOReturnSuccess) return false;
    PROBE_LOG("init object=%{public}u rate=48000 channels=16 capacity_frames=%{public}u", GetObjectID(), capacityFrames);
    return true;
}
void ConfigurationProbeDevice::Shutdown() {
    if (!ivars || ivars->shuttingDown.exchange(true)) return;
    ivars->unavailable.store(true);
    ivars->pending.store(0);
    ivars->running.store(false);
    if (ivars->ioState) ivars->ioState->running.store(false);
    if (ivars->output) ivars->output->Bind(nullptr);
    if (ivars->input) ivars->input->Bind(nullptr);
    if (ivars->timer) {
        retain(); // keep the target alive until the last queued tick completes
        if (ivars->timer->Cancel(^{ release(); }) != kIOReturnSuccess) release();
    }
}
void ConfigurationProbeDevice::free() {
    if (ivars) {
        ivars->running.store(false);
        if (ivars->output) { ivars->output->Bind(nullptr); RemoveStream(ivars->output.get()); }
        if (ivars->input) { ivars->input->Bind(nullptr); RemoveStream(ivars->input.get()); }
        // super releases the IO block before its captured state is destroyed.
        super::free();
        IOSafeDeleteNULL(ivars, ConfigurationProbeDevice_IVars, 1);
        return;
    }
    super::free();
}
kern_return_t ConfigurationProbeDevice::HandleChangeSampleRate(double rate) {
    if (!ivars || !Supported(rate)) return kIOReturnUnsupported;
    if (ivars->unavailable.load()) return kIOReturnNotReady;
    if (rate == ivars->rate.load() && ivars->pending.load() == 0) return kIOReturnSuccess;
    const uint64_t token = ivars->nextToken.fetch_add(1);
    if (token >= (uint64_t{1} << 24)) return kIOReturnNoResources;
    const uint64_t action = actionTag | (token << 32) | uint32_t(rate);
    uint64_t empty = 0;
    if (!ivars->pending.compare_exchange_strong(empty, action)) return kIOReturnBusy;
    PROBE_LOG("request object=%{public}u token=%{public}llu old=%{public}u requested=%{public}.0f running=%{public}u",
        GetObjectID(), token, ivars->rate.load(), rate, ivars->running.load());
    const auto result = RequestDeviceConfigurationChange(action, nullptr);
    if (result != kIOReturnSuccess) { uint64_t expected = action; ivars->pending.compare_exchange_strong(expected, uint64_t{0}); }
    return result;
}
kern_return_t ConfigurationProbeDevice::RequestFormat(ConfigurationProbeStream* stream,
    const IOUserAudioStreamBasicDescription* format) {
    if (!ivars || !format || !Supported(format->mSampleRate)) return kIOReturnUnsupported;
    const auto expected = Format(uint32_t(format->mSampleRate));
    if (format->mFormatID != expected.mFormatID || format->mFormatFlags != expected.mFormatFlags ||
        format->mChannelsPerFrame != expected.mChannelsPerFrame || format->mBitsPerChannel != 32 ||
        format->mBytesPerFrame != expected.mBytesPerFrame || format->mBytesPerPacket != expected.mBytesPerPacket ||
        format->mFramesPerPacket != 1) return kIOReturnUnsupported;
    PROBE_LOG("stream_callback object=%{public}u stream=%{public}u rate=%{public}.0f channels=%{public}u projecting=%{public}u",
        GetObjectID(), stream->GetObjectID(), format->mSampleRate, format->mChannelsPerFrame, ivars->projecting.load());
    if (ivars->projecting.load()) return stream->SetCurrentStreamFormat(format);
    if (ivars->pending.load()) return kIOReturnBusy;
    if (format->mSampleRate == ivars->rate.load()) return stream->SetCurrentStreamFormat(format);
    const auto result = HandleChangeSampleRate(format->mSampleRate);
    // SDK requires a successful callback to have installed the format. The
    // deferred path reports Busy; the runtime experiment checks host retry.
    return result == kIOReturnSuccess ? kIOReturnBusy : result;
}
kern_return_t ConfigurationProbeDevice::PerformDeviceConfigurationChange(uint64_t action, OSObject* info) {
    if ((action & (uint64_t{0xff} << 56)) != actionTag) {
        PROBE_LOG("host_perform_enter object=%{public}u action=%{public}llu running=%{public}u", GetObjectID(), action, ivars->running.load());
        const auto result = super::PerformDeviceConfigurationChange(action, info);
        PROBE_LOG("host_perform_exit object=%{public}u action=%{public}llu pending=%{public}llu result=0x%{public}x", GetObjectID(), action, ivars->pending.load(), result);
        return result;
    }
    if (!ivars || ivars->pending.load() != action) return kIOReturnNotReady;
    PROBE_LOG("perform object=%{public}u token=%{public}llu running=%{public}u", GetObjectID(),
        (action >> 32) & 0xffffff, ivars->running.load());
    if (ivars->running.load()) { ivars->unavailable.store(true); return kIOReturnBusy; }
    const uint32_t candidate = uint32_t(action);
    const uint32_t prior = ivars->rate.load();
    ivars->projecting.store(true);
    auto project = [&](uint32_t rate) {
        auto format = Format(rate);
        auto geometry = ASFW::Lab::AudioGeometryForRate(rate);
        auto result = SetSampleRate(rate);
        if (result == kIOReturnSuccess) result = ivars->output->SetCurrentStreamFormat(&format);
        if (result == kIOReturnSuccess) result = ivars->input->SetCurrentStreamFormat(&format);
        if (result == kIOReturnSuccess) result = SetZeroTimeStampPeriod(geometry.zeroTimestampPeriodFrames);
        return result;
    };
    auto result = project(candidate);
    if (result == kIOReturnSuccess) result = super::PerformDeviceConfigurationChange(action, info);
    if (result == kIOReturnSuccess) ivars->rate.store(candidate);
    else if (project(prior) != kIOReturnSuccess) ivars->unavailable.store(true);
    ivars->projecting.store(false);
    ivars->pending.store(0);
    PROBE_LOG("commit object=%{public}u output=%{public}u input=%{public}u token=%{public}llu rate=%{public}u hal=%{public}.0f out_rate=%{public}.0f in_rate=%{public}.0f channels=%{public}u zts=%{public}u result=0x%{public}x stopped=%{public}u",
        GetObjectID(), ivars->output->GetObjectID(), ivars->input->GetObjectID(), (action >> 32) & 0xffffff,
        ivars->rate.load(), GetSampleRate(), ivars->output->GetCurrentStreamFormat().mSampleRate,
        ivars->input->GetCurrentStreamFormat().mSampleRate, Channels(ivars->rate.load()), GetZeroTimestampPeriod(), result, ivars->unavailable.load());
    return result;
}
kern_return_t ConfigurationProbeDevice::AbortDeviceConfigurationChange(uint64_t action, OSObject* info) {
    if (ivars) { uint64_t expected = action; ivars->pending.compare_exchange_strong(expected, uint64_t{0}); }
    PROBE_LOG("abort object=%{public}u action=%{public}llu", GetObjectID(), action);
    return super::AbortDeviceConfigurationChange(action, info);
}
kern_return_t ConfigurationProbeDevice::StartIO(IOUserAudioStartStopFlags flags) {
    if (!ivars || ivars->unavailable.load() || ivars->pending.load()) return kIOReturnNotReady;
    ivars->epoch = mach_absolute_time(); ivars->periodIndex = 0;
    UpdateCurrentZeroTimestamp(0, ivars->epoch);
    ivars->running.store(true, std::memory_order_release);
    ivars->ioState->running.store(true, std::memory_order_release);
    auto result = super::StartIO(flags);
    if (result == kIOReturnSuccess) { ivars->timer->SetEnable(true); Tick_Impl(nullptr, 0); }
    else { ivars->running.store(false); ivars->ioState->running.store(false); }
    PROBE_LOG("start object=%{public}u rate=%{public}u channels=%{public}u result=0x%{public}x", GetObjectID(), ivars->rate.load(), Channels(ivars->rate.load()), result);
    return result;
}
kern_return_t ConfigurationProbeDevice::StopIO(IOUserAudioStartStopFlags flags) {
    ivars->running.store(false, std::memory_order_release); ivars->timer->SetEnable(false);
    ivars->ioState->running.store(false, std::memory_order_release);
    auto result = super::StopIO(flags);
    PROBE_LOG("stop object=%{public}u rate=%{public}u io=%{public}llu late_io=%{public}llu pending=%{public}llu result=0x%{public}x",
        GetObjectID(), ivars->rate.load(), ivars->ioState->calls.load(), ivars->ioState->late.load(), ivars->pending.load(), result);
    return result;
}
void ConfigurationProbeDevice::Tick_Impl(OSAction*, uint64_t) {
    if (!ivars || !ivars->running.load()) return;
    const uint64_t frames = ivars->periodIndex * GetZeroTimestampPeriod();
    const uint64_t ns = (frames / ivars->rate.load()) * 1'000'000'000 +
        (frames % ivars->rate.load()) * 1'000'000'000 / ivars->rate.load();
    const uint64_t ticks = ns / ivars->timebase.numer * ivars->timebase.denom +
        ns % ivars->timebase.numer * ivars->timebase.denom / ivars->timebase.numer;
    UpdateCurrentZeroTimestamp(frames, ivars->epoch + ticks);
    ++ivars->periodIndex;
    const uint64_t nextFrames = ivars->periodIndex * GetZeroTimestampPeriod();
    const uint64_t nextNs = (nextFrames / ivars->rate.load()) * 1'000'000'000 +
        (nextFrames % ivars->rate.load()) * 1'000'000'000 / ivars->rate.load();
    const uint64_t nextTicks = nextNs / ivars->timebase.numer * ivars->timebase.denom +
        nextNs % ivars->timebase.numer * ivars->timebase.denom / ivars->timebase.numer;
    ivars->timer->WakeAtTime(kIOTimerClockMachAbsoluteTime, ivars->epoch + nextTicks, 0);
}
