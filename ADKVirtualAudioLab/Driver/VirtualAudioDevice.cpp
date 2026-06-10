#include <new>
#include <atomic>
#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <os/log.h>
#include "VirtualAudioDevice.h"

#define LAB_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[ADKLab] " fmt, ##__VA_ARGS__)
#include "../Core/VirtualAudioDeviceController.hpp"
#include "../Lab/StickyCounterSink.hpp"
#include "../Lab/VerifyingSlotProvider.hpp"

using namespace ASFW::Driver;

// M3 dext clock model (see ../README.md, "Key design decisions" and
// Milestone 3):
//
// - The device is its own clock master. An IOTimerDispatchSource on the work
//   queue fires once per ZTS period (512 frames at 48 kHz = 10.667 ms) in the
//   kIOTimerClockMachAbsoluteTime timebase — the same clock CoreAudio host
//   times use. Each fire anchors UpdateCurrentZeroTimestamp(n * period,
//   fire_time) with RAW values: the deadline chain is nominal (computed from
//   the period index, never from the previous fire), the host time is the
//   actual fire time, and the host smooths via the clock algorithm. No
//   driver-side extrapolation (the model RE'd from the Saffire kext).
// - The same timer fire exposes the next period's packets through the
//   controller (PrepareLabPacket) — the lab's stand-in for the OHCI IT ring
//   interrupt: "hardware" requests data on its interrupt; WriteEnd only fills
//   PCM into already-exposed packets.
// - The Verifying(Fake) decorator from Step 6 sits between the engine and the
//   fake ring for the whole run; StopIO dumps its sticky counters plus the
//   O/C instrumentation via IOLog (never from the IO callback).
// - O2 instrumentation: the IO block keeps the SDK-documented raw ivars
//   capture; ioRunning gates it and late fires are counted instead of
//   crashing, so the lifecycle question is answered with a counter.

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kOutputChannels = 8;
constexpr uint32_t kBytesPerSample = sizeof(float);
constexpr uint32_t kOutputBytesPerFrame = kOutputChannels * kBytesPerSample;
constexpr uint32_t kRingPeriods = 8; // ring = 8 ZTS periods (4096 frames)
constexpr uint64_t kZtsPeriodNsNumer = 32000000ull; // 512/48000 s = 32e6/3 ns
constexpr uint64_t kZtsPeriodNsDenom = 3ull;
constexpr uint32_t kMaxPreparePerCall = 512; // runaway guard for the pump

struct LabTimebase final {
    uint32_t numer{1};
    uint32_t denom{1};

    uint64_t NsToTicks(uint64_t ns) const noexcept {
        return (numer == denom) ? ns : (ns * denom) / numer;
    }
};

// Nominal nanoseconds elapsed after n ZTS periods (exact thirds, no
// accumulated rounding: computed from n, not incrementally).
inline uint64_t NsForPeriodIndex(uint64_t n) noexcept {
    return (n * kZtsPeriodNsNumer) / kZtsPeriodNsDenom;
}

} // namespace

struct VirtualAudioDevice_IVars
{
    OSSharedPtr<IOUserAudioDriver> driver;
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<IOUserAudioStream> outputStream;
    OSSharedPtr<IOMemoryMap> outputMemoryMap;
    OSSharedPtr<IOTimerDispatchSource> ztsTimer;
    OSSharedPtr<OSAction> ztsTimerAction;

    VirtualAudioDeviceController* controller{nullptr};
    ASFW::Lab::VerifyingSlotProvider* verifier{nullptr};
    ASFW::Lab::StickyCounterSink* diagSink{nullptr};

    uint32_t outputBytesPerFrame{0};
    uint32_t outputChannels{0};
    uint32_t ringFrames{0};

    LabTimebase timebase{};

    // Clock chain state (work-queue confined).
    uint64_t startHostTime{0};
    uint64_t periodIndex{0};

    // Packet pump state (work-queue confined).
    uint32_t nextPacketIndex{0};
    uint64_t exposedFrames{0};
    uint64_t prepareFailures{0};

    // Lifecycle gate + O/C instrumentation (IO callback is a real-time
    // thread: relaxed atomics only, no logging, no allocation).
    std::atomic<bool> ioRunning{false};
    std::atomic<uint64_t> anchorsPublished{0};
    std::atomic<uint64_t> anchorsBeforeFirstWriteEnd{0};
    std::atomic<uint64_t> writeEndCount{0};
    std::atomic<uint64_t> framesDelivered{0};
    std::atomic<uint32_t> minIoFrames{0xFFFFFFFFu};
    std::atomic<uint32_t> maxIoFrames{0};
    std::atomic<uint64_t> sampleTimeBreaks{0};
    std::atomic<uint64_t> expectedNextSampleTime{0};
    std::atomic<bool> expectedSampleTimeValid{false};
    std::atomic<uint64_t> firstWriteEndSampleTime{0};
    std::atomic<uint64_t> firstWriteEndHostTime{0};
    std::atomic<uint64_t> otherIoOperations{0};
    std::atomic<uint64_t> ioAfterStop{0};      // O2: WriteEnd after StopIO
    std::atomic<uint64_t> timerAfterStop{0};   // O2: timer fire after StopIO
};

bool VirtualAudioDevice::init(IOUserAudioDriver* in_driver,
                               bool in_supports_prewarming,
                               OSString* in_device_uid,
                               OSString* in_model_uid,
                               OSString* in_manufacturer_uid,
                               uint32_t in_zero_timestamp_period)
{
    LAB_LOG("init - entering. DeviceUID: %{public}s, ModelUID: %{public}s, ManufacturerUID: %{public}s, zeroTimestampPeriod: %{public}u",
            in_device_uid ? in_device_uid->getCStringNoCopy() : "NULL",
            in_model_uid ? in_model_uid->getCStringNoCopy() : "NULL",
            in_manufacturer_uid ? in_manufacturer_uid->getCStringNoCopy() : "NULL",
            in_zero_timestamp_period);

    LAB_LOG("init - calling super::init");
    if (!super::init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period)) {
        LAB_LOG("init - super::init failed");
        return false;
    }
    LAB_LOG("init - super::init succeeded");

    LAB_LOG("init - allocating ivars");
    ivars = IONewZero(VirtualAudioDevice_IVars, 1);
    if (ivars == nullptr) {
        LAB_LOG("init - failed to allocate ivars");
        return false;
    }
    LAB_LOG("init - ivars allocated successfully");

    ivars->driver = OSSharedPtr(in_driver, OSRetain);
    ivars->workQueue = GetWorkQueue();
    if (ivars->workQueue.get() == nullptr) {
        LAB_LOG("init - workQueue is null");
        return false;
    }
    LAB_LOG("init - workQueue retrieved successfully");

    mach_timebase_info_data_t timebaseInfo{};
    if (mach_timebase_info(&timebaseInfo) == KERN_SUCCESS &&
        timebaseInfo.numer != 0 && timebaseInfo.denom != 0) {
        ivars->timebase.numer = timebaseInfo.numer;
        ivars->timebase.denom = timebaseInfo.denom;
        LAB_LOG("init - Mach timebase info: numer=%{public}u, denom=%{public}u", timebaseInfo.numer, timebaseInfo.denom);
    } else {
        LAB_LOG("init - mach_timebase_info failed");
    }

    LAB_LOG("init - initializing controller");
    ivars->controller = new VirtualAudioDeviceController();
    if (!ivars->controller->Initialize()) {
        LAB_LOG("init - Failed to initialize controller");
        return false;
    }
    LAB_LOG("init - controller initialized successfully");

    // Step 6 instrument under real pacing: Verifying(Fake) for the whole run.
    // P5 enabled — M2 timing stamps real SYTs on every data packet.
    LAB_LOG("init - setting up VerifyingSlotProvider");
    ivars->diagSink = new ASFW::Lab::StickyCounterSink();
    {
        ASFW::Lab::VerifyingSlotProvider::Config verifierConfig{};
        verifierConfig.diagSink = ivars->diagSink;
        verifierConfig.p5Enabled = true;
        ivars->verifier = new ASFW::Lab::VerifyingSlotProvider(
            ivars->controller->FakeSlotProvider(), verifierConfig);
    }
    ivars->controller->BindLabSlotProvider(ivars->verifier);
    LAB_LOG("init - VerifyingSlotProvider bound to controller");

    // Pick Saffire for testing
    ASFW::Protocols::Audio::DICE::DiceDeviceIdentity identity{};
    identity.vendorId = 0x00130e; // Focusrite
    ivars->controller->SelectProfile(identity);
    LAB_LOG("init - selected profile for Focusrite (vendor 0x00130e)");

    // The lab device reports as a FireWire-transport clock device — that is
    // the contract ASFW will live under (AudioDriverKitTypes.h '1394').
    LAB_LOG("init - setting transport type to FireWire");
    SetTransportType(IOUserAudioTransportType::FireWire);

    // Set up preferred output channel layout (8 discrete channels)
    LAB_LOG("init - setting preferred output channel layout");
    IOUserAudioChannelLabel outputChannelLayout[kOutputChannels] = {
        IOUserAudioChannelLabel::Discrete_0,
        IOUserAudioChannelLabel::Discrete_1,
        IOUserAudioChannelLabel::Discrete_2,
        IOUserAudioChannelLabel::Discrete_3,
        IOUserAudioChannelLabel::Discrete_4,
        IOUserAudioChannelLabel::Discrete_5,
        IOUserAudioChannelLabel::Discrete_6,
        IOUserAudioChannelLabel::Discrete_7
    };
    kern_return_t kr = SetPreferredOutputChannelLayout(outputChannelLayout, kOutputChannels);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetPreferredOutputChannelLayout failed (kr = 0x%{public}08x)", kr);
    } else {
        LAB_LOG("init - SetPreferredOutputChannelLayout succeeded");
    }

    // Set capabilities for default output
    LAB_LOG("init - setting default output device capabilities");
    kr = SetCanBeDefaultOutputDevice(true);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetCanBeDefaultOutputDevice failed (kr = 0x%{public}08x)", kr);
    } else {
        LAB_LOG("init - SetCanBeDefaultOutputDevice set to true");
    }

    kr = SetCanBeDefaultSystemOutputDevice(true);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetCanBeDefaultSystemOutputDevice failed (kr = 0x%{public}08x)", kr);
    } else {
        LAB_LOG("init - SetCanBeDefaultSystemOutputDevice set to true");
    }

    // Set output safety offset
    LAB_LOG("init - setting output safety offset to 0");
    kr = SetOutputSafetyOffset(0);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetOutputSafetyOffset failed (kr = 0x%{public}08x)", kr);
    } else {
        LAB_LOG("init - SetOutputSafetyOffset set to 0");
    }

    // Set up a basic float32 output stream (8 channels, 48kHz)
    double sampleRate = kSampleRate;
    LAB_LOG("init - setting available sample rate to %{public}.1f", sampleRate);
    SetAvailableSampleRates(&sampleRate, 1);
    LAB_LOG("init - setting current sample rate to %{public}.1f", sampleRate);
    SetSampleRate(sampleRate);

    IOUserAudioStreamBasicDescription format = {
        .mSampleRate = sampleRate,
        .mFormatID = IOUserAudioFormatID::LinearPCM,
        .mFormatFlags = static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsFloat | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
        .mBytesPerPacket = kOutputBytesPerFrame,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = kOutputBytesPerFrame,
        .mChannelsPerFrame = kOutputChannels,
        .mBitsPerChannel = 32
    };

    LAB_LOG("init - stream format basic description:");
    LAB_LOG("  mSampleRate: %{public}.1f", format.mSampleRate);
    LAB_LOG("  mFormatID: 0x%{public}x (lpcm = 0x6c70636d)", static_cast<uint32_t>(format.mFormatID));
    LAB_LOG("  mFormatFlags: 0x%{public}x (Float/Packed/Native)", static_cast<uint32_t>(format.mFormatFlags));
    LAB_LOG("  mBytesPerPacket: %{public}u", format.mBytesPerPacket);
    LAB_LOG("  mFramesPerPacket: %{public}u", format.mFramesPerPacket);
    LAB_LOG("  mBytesPerFrame: %{public}u", format.mBytesPerFrame);
    LAB_LOG("  mChannelsPerFrame: %{public}u", format.mChannelsPerFrame);
    LAB_LOG("  mBitsPerChannel: %{public}u", format.mBitsPerChannel);

    ivars->outputBytesPerFrame = format.mBytesPerFrame;
    ivars->outputChannels = format.mChannelsPerFrame;

    // Ring = 8 ZTS periods. A one-period ring leaves the HAL zero headroom
    // around the wrap anchor; 8 matches the host scenario pump (4096 frames).
    // C4 (period vs IO buffer size coupling) stays observable by varying this.
    ivars->ringFrames = kRingPeriods * in_zero_timestamp_period;

    LAB_LOG("init - creating output ring buffer (%{public}u bytes, ringFrames = %{public}u, zeroTimestampPeriod = %{public}u)",
            ivars->ringFrames * format.mBytesPerFrame, ivars->ringFrames, in_zero_timestamp_period);
    OSSharedPtr<IOBufferMemoryDescriptor> buffer;
    uint32_t bufferSize = ivars->ringFrames * format.mBytesPerFrame;
    kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, bufferSize, 0, buffer.attach());
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - Failed to create output IOBufferMemoryDescriptor (kr = 0x%{public}08x)", kr);
        return false;
    }
    LAB_LOG("init - IOBufferMemoryDescriptor created successfully. Length: %{public}u bytes", bufferSize);

    LAB_LOG("init - creating output stream object");
    ivars->outputStream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Output, buffer.get());
    if (!ivars->outputStream) {
        LAB_LOG("init - Failed to create output IOUserAudioStream");
        return false;
    }
    LAB_LOG("init - output stream object created successfully");

    LAB_LOG("init - configuring stream formats");
    ivars->outputStream->SetAvailableStreamFormats(&format, 1);
    ivars->outputStream->SetCurrentStreamFormat(&format);

    LAB_LOG("init - adding stream to device");
    kr = AddStream(ivars->outputStream.get());
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - AddStream failed (kr = 0x%{public}08x)", kr);
        return false;
    }
    LAB_LOG("init - stream added successfully to device");

    LAB_LOG("init - configuring controller output stream");
    ivars->controller->ConfigureOutputStream(kSampleRate, ivars->outputChannels,
                                             ivars->ringFrames);

    // M2: SYT realism — the controller stamps real SYTs from TxTimingModel
    // against the simulated timeline (which rides the exposure cursor: the
    // packet's projected transmit position, per the Saffire model).
    LAB_LOG("init - enabling controller timing model");
    ivars->controller->EnableLabTiming(ASFW::Driver::TxTimingModel::Config{});

    // The ZTS heartbeat timer (armed in StartIO).
    LAB_LOG("init - creating ZTS timer");
    IOTimerDispatchSource* timer = nullptr;
    if (IOTimerDispatchSource::Create(ivars->workQueue.get(), &timer) == kIOReturnSuccess) {
        ivars->ztsTimer = OSSharedPtr(timer, OSNoRetain);
        OSAction* action = nullptr;
        if (CreateActionZtsTimerOccurred(0, &action) == kIOReturnSuccess) {
            ivars->ztsTimerAction = OSSharedPtr(action, OSNoRetain);
            ivars->ztsTimer->SetHandler(ivars->ztsTimerAction.get());
            LAB_LOG("init - ZTS timer and action created successfully");
        } else {
            LAB_LOG("init - failed to create ZTS timer action");
            return false;
        }
    } else {
        LAB_LOG("init - failed to create ZTS timer");
        return false;
    }

    auto ivarsPtr = ivars;

    auto io_operation = ^kern_return_t(IOUserAudioObjectID in_device,
                                       IOUserAudioIOOperation in_io_operation,
                                       uint32_t in_io_buffer_frame_size,
                                       uint64_t in_sample_time,
                                       uint64_t in_host_time)
    {
        if (in_io_operation == IOUserAudioIOOperationWriteEnd) {
            if (!ivarsPtr->ioRunning.load(std::memory_order_relaxed)) {
                ivarsPtr->ioAfterStop.fetch_add(1, std::memory_order_relaxed);
            }

            // C3 shape instrumentation (RT-safe: counters only).
            const uint64_t count =
                ivarsPtr->writeEndCount.fetch_add(1, std::memory_order_relaxed);
            if (count == 0) {
                ivarsPtr->firstWriteEndSampleTime.store(in_sample_time,
                                                        std::memory_order_relaxed);
                ivarsPtr->firstWriteEndHostTime.store(in_host_time,
                                                      std::memory_order_relaxed);
            }
            ivarsPtr->framesDelivered.fetch_add(in_io_buffer_frame_size,
                                                std::memory_order_relaxed);
            if (in_io_buffer_frame_size <
                ivarsPtr->minIoFrames.load(std::memory_order_relaxed)) {
                ivarsPtr->minIoFrames.store(in_io_buffer_frame_size,
                                            std::memory_order_relaxed);
            }
            if (in_io_buffer_frame_size >
                ivarsPtr->maxIoFrames.load(std::memory_order_relaxed)) {
                ivarsPtr->maxIoFrames.store(in_io_buffer_frame_size,
                                            std::memory_order_relaxed);
            }
            if (ivarsPtr->expectedSampleTimeValid.load(std::memory_order_relaxed) &&
                ivarsPtr->expectedNextSampleTime.load(std::memory_order_relaxed) !=
                    in_sample_time) {
                ivarsPtr->sampleTimeBreaks.fetch_add(1, std::memory_order_relaxed);
            }
            ivarsPtr->expectedNextSampleTime.store(
                in_sample_time + in_io_buffer_frame_size, std::memory_order_relaxed);
            ivarsPtr->expectedSampleTimeValid.store(true, std::memory_order_relaxed);

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
        } else {
            ivarsPtr->otherIoOperations.fetch_add(1, std::memory_order_relaxed);
        }
        return kIOReturnSuccess;
    };

    LAB_LOG("init - setting IO operation handler");
    kr = SetIOOperationHandler(io_operation);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetIOOperationHandler failed (kr = 0x%{public}08x)", kr);
        return false;
    }
    LAB_LOG("init - SetIOOperationHandler succeeded. Device initialization complete.");

    return true;
}

void VirtualAudioDevice::free()
{
    if (ivars != nullptr) {
        // O2 answer at teardown: how many callbacks arrived after StopIO.
        LAB_LOG("free: io_after_stop=%{public}llu timer_after_stop=%{public}llu",
                ivars->ioAfterStop.load(std::memory_order_relaxed),
                ivars->timerAfterStop.load(std::memory_order_relaxed));

        if (ivars->ztsTimer) {
            ivars->ztsTimer->Cancel(^{});
        }
        if (ivars->verifier) {
            delete ivars->verifier;
        }
        if (ivars->diagSink) {
            delete ivars->diagSink;
        }
        if (ivars->controller) {
            delete ivars->controller;
        }
        ivars->driver.reset();
        ivars->workQueue.reset();
        ivars->outputStream.reset();
        ivars->outputMemoryMap.reset();
        ivars->ztsTimer.reset();
        ivars->ztsTimerAction.reset();
    }
    IOSafeDeleteNULL(ivars, VirtualAudioDevice_IVars, 1);
    super::free();
}

// Expose packets until the timeline covers targetFrames (work-queue only).
// The lab analog of refilling the IT DMA ring: structurally valid packets
// (silence until audio arrives) published through Verifying(Fake).
static void PrepareCoverage(VirtualAudioDevice_IVars* ivars, uint64_t targetFrames)
{
    uint32_t prepared = 0;
    while (ivars->exposedFrames < targetFrames && prepared < kMaxPreparePerCall) {
        // The simulated bus rides the exposure cursor (projected transmit
        // position), so SYT lead stays at the grafted seed by construction.
        ivars->controller->AdvanceLabTimelineToFrame(ivars->exposedFrames);
        if (!ivars->controller->PrepareLabPacketTimed(ivars->nextPacketIndex)) {
            ++ivars->prepareFailures;
            return;
        }
        const auto* published =
            ivars->controller->FakeSlotProvider().PublishedPacket(
                ivars->nextPacketIndex);
        if (published != nullptr && published->isData) {
            ivars->exposedFrames += published->framesInPacket;
        }
        ++ivars->nextPacketIndex;
        ++prepared;
    }
}

void VirtualAudioDevice::ZtsTimerOccurred_Impl(OSAction* action, uint64_t time)
{
    if (ivars == nullptr) {
        return;
    }
    if (!ivars->ioRunning.load(std::memory_order_relaxed)) {
        ivars->timerAfterStop.fetch_add(1, std::memory_order_relaxed);
        return; // do not re-arm
    }

    // Anchor with RAW values: nominal sample position, actual fire time.
    const uint64_t sampleTime = ivars->periodIndex * GetZeroTimestampPeriod();
    UpdateCurrentZeroTimestamp(sampleTime, time);
    ivars->anchorsPublished.fetch_add(1, std::memory_order_relaxed);
    if (ivars->writeEndCount.load(std::memory_order_relaxed) == 0) {
        ivars->anchorsBeforeFirstWriteEnd.fetch_add(1, std::memory_order_relaxed);
    }

    // The "hardware" requests the next period's data: keep the exposed
    // timeline one ring-wrap ahead of where the HAL will write.
    PrepareCoverage(ivars, sampleTime + 3ull * GetZeroTimestampPeriod());

    // Drift-free nominal chain: the next deadline comes from the period
    // index, never from the (jittered) previous fire time.
    ivars->periodIndex += 1;
    const uint64_t deadline =
        ivars->startHostTime +
        ivars->timebase.NsToTicks(NsForPeriodIndex(ivars->periodIndex));
    const uint64_t leeway = ivars->timebase.NsToTicks(500000); // 0.5 ms
    ivars->ztsTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, deadline, leeway);
}

kern_return_t VirtualAudioDevice::StartIO(IOUserAudioStartStopFlags in_flags)
{
    LAB_LOG("StartIO - entering. flags = 0x%{public}x", in_flags);

    __block kern_return_t kr = kIOReturnSuccess;
    ivars->workQueue->DispatchSync(^(){
        LAB_LOG("StartIO - calling super::StartIO");
        kr = super::StartIO(in_flags);
        if (kr != kIOReturnSuccess) {
            LAB_LOG("StartIO - super::StartIO failed (kr = 0x%{public}08x)", kr);
            return;
        }
        LAB_LOG("StartIO - super::StartIO succeeded");

        if (ivars->outputStream) {
            LAB_LOG("StartIO - getting output stream memory descriptor");
            auto buffer = ivars->outputStream->GetIOMemoryDescriptor();
            if (buffer) {
                uint64_t bufferLength = 0;
                buffer->GetLength(&bufferLength);
                LAB_LOG("StartIO - creating memory mapping for output stream (length = %{public}llu bytes)", bufferLength);
                kern_return_t mapKr = buffer->CreateMapping(0, 0, 0, 0, 0, ivars->outputMemoryMap.attach());
                if (mapKr != kIOReturnSuccess) {
                    LAB_LOG("StartIO - CreateMapping failed (kr = 0x%{public}08x)", mapKr);
                    kr = mapKr;
                    return;
                }
                LAB_LOG("StartIO - output memory map created: address = 0x%{public}llx, offset = %{public}llu, length = %{public}llu bytes",
                        ivars->outputMemoryMap->GetAddress(), ivars->outputMemoryMap->GetOffset(), ivars->outputMemoryMap->GetLength());
            } else {
                LAB_LOG("StartIO - output stream has no memory descriptor");
                kr = kIOReturnInternalError;
                return;
            }
        } else {
            LAB_LOG("StartIO - output stream is null");
            kr = kIOReturnInternalError;
            return;
        }

        if (ivars->controller) {
            LAB_LOG("StartIO - resetting controller transport lab");
            ivars->controller->ResetTransportLab(0, 0);
        }
        if (ivars->verifier) {
            LAB_LOG("StartIO - resetting verifier");
            ivars->verifier->Reset();
        }

        // Reset pump + instrumentation for this run.
        ivars->nextPacketIndex = 0;
        ivars->exposedFrames = 0;
        ivars->prepareFailures = 0;
        ivars->periodIndex = 0;
        ivars->anchorsPublished.store(0, std::memory_order_relaxed);
        ivars->anchorsBeforeFirstWriteEnd.store(0, std::memory_order_relaxed);
        ivars->writeEndCount.store(0, std::memory_order_relaxed);
        ivars->framesDelivered.store(0, std::memory_order_relaxed);
        ivars->minIoFrames.store(0xFFFFFFFFu, std::memory_order_relaxed);
        ivars->maxIoFrames.store(0, std::memory_order_relaxed);
        ivars->sampleTimeBreaks.store(0, std::memory_order_relaxed);
        ivars->expectedSampleTimeValid.store(false, std::memory_order_relaxed);
        ivars->otherIoOperations.store(0, std::memory_order_relaxed);

        // Seed the clock chain: anchor (0, now), pre-expose two periods, and
        // arm the first wrap. C1 counts how many anchors precede the first
        // WriteEnd the HAL ever delivers.
        ivars->startHostTime = mach_absolute_time();
        LAB_LOG("StartIO - seeding clock chain: startHostTime = %{public}llu ticks", ivars->startHostTime);
        UpdateCurrentZeroTimestamp(0, ivars->startHostTime);
        ivars->anchorsPublished.fetch_add(1, std::memory_order_relaxed);
        ivars->anchorsBeforeFirstWriteEnd.fetch_add(1, std::memory_order_relaxed);

        if (ivars->controller) {
            LAB_LOG("StartIO - pre-preparing coverage");
            PrepareCoverage(ivars, 3ull * GetZeroTimestampPeriod());
        }

        ivars->ioRunning.store(true, std::memory_order_relaxed);
        ivars->periodIndex = 1;
        const uint64_t deadline =
            ivars->startHostTime +
            ivars->timebase.NsToTicks(NsForPeriodIndex(1));
        const uint64_t leeway = ivars->timebase.NsToTicks(500000);
        
        LAB_LOG("StartIO - arming ZTS timer for first deadline = %{public}llu ticks (leeway = %{public}llu)", deadline, leeway);
        if (ivars->ztsTimer) {
            ivars->ztsTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, deadline,
                                        leeway);
        } else {
            LAB_LOG("StartIO - ZTS timer is null!");
        }
    });

    LAB_LOG("StartIO - exiting. result = 0x%{public}08x", kr);
    return kr;
}

kern_return_t VirtualAudioDevice::StopIO(IOUserAudioStartStopFlags in_flags)
{
    LAB_LOG("StopIO - entering. flags = 0x%{public}x", in_flags);

    __block kern_return_t kr = kIOReturnSuccess;
    ivars->workQueue->DispatchSync(^(){
        LAB_LOG("StopIO - setting ioRunning to false");
        ivars->ioRunning.store(false, std::memory_order_relaxed);

        LAB_LOG("StopIO - calling super::StopIO");
        kr = super::StopIO(in_flags);
        if (kr != kIOReturnSuccess) {
            LAB_LOG("StopIO - super::StopIO failed (kr = 0x%{public}08x)", kr);
        } else {
            LAB_LOG("StopIO - super::StopIO succeeded");
        }

        LAB_LOG("StopIO - resetting output memory map");
        ivars->outputMemoryMap.reset();

        // ---- M3 dump (StopIO may take as long as necessary) ----
        const auto snapshot = ivars->verifier ? ivars->verifier->Snapshot()
                                               : ASFW::Lab::VerifierSnapshot{};
        using ASFW::Lab::VerifierCounterId;

        LAB_LOG("dump zts: anchors=%{public}llu before_first_io=%{public}llu period=%{public}u "
                "ring_frames=%{public}u prepare_failures=%{public}llu",
                ivars->anchorsPublished.load(std::memory_order_relaxed),
                ivars->anchorsBeforeFirstWriteEnd.load(std::memory_order_relaxed),
                GetZeroTimestampPeriod(), ivars->ringFrames,
                ivars->prepareFailures);

        const uint64_t firstHost =
            ivars->firstWriteEndHostTime.load(std::memory_order_relaxed);
        LAB_LOG("dump writeend: count=%{public}llu frames=%{public}llu min=%{public}u max=%{public}u "
                "sample_breaks=%{public}llu first_sample=%{public}llu first_host_delta=%{public}lld "
                "other_ops=%{public}llu",
                ivars->writeEndCount.load(std::memory_order_relaxed),
                ivars->framesDelivered.load(std::memory_order_relaxed),
                ivars->minIoFrames.load(std::memory_order_relaxed),
                ivars->maxIoFrames.load(std::memory_order_relaxed),
                ivars->sampleTimeBreaks.load(std::memory_order_relaxed),
                ivars->firstWriteEndSampleTime.load(std::memory_order_relaxed),
                (firstHost != 0)
                    ? (int64_t)(firstHost - ivars->startHostTime)
                    : (int64_t)0,
                ivars->otherIoOperations.load(std::memory_order_relaxed));

        LAB_LOG("dump verifier: violations=%{public}llu p1_win=%{public}llu p1_run=%{public}llu "
                "p1_idx=%{public}llu p2_dbc=%{public}llu p3_bytes=%{public}llu p3_q0=%{public}llu p3_q1=%{public}llu "
                "p3_unacq=%{public}llu p4_tile=%{public}llu p4_cnt=%{public}llu p5_step=%{public}llu p5_graft=%{public}llu",
                snapshot.TotalViolations(),
                snapshot.Value(VerifierCounterId::kP1CadenceWindowViolation),
                snapshot.Value(VerifierCounterId::kP1CadenceRunViolation),
                snapshot.Value(VerifierCounterId::kP1PacketIndexGapViolation),
                snapshot.Value(VerifierCounterId::kP2DbcViolation),
                snapshot.Value(VerifierCounterId::kP3ByteCountViolation),
                snapshot.Value(VerifierCounterId::kP3CipQ0Violation),
                snapshot.Value(VerifierCounterId::kP3CipQ1Violation),
                snapshot.Value(VerifierCounterId::kP3UnacquiredPublishViolation),
                snapshot.Value(VerifierCounterId::kP4FrameTilingViolation),
                snapshot.Value(VerifierCounterId::kP4FrameCountViolation),
                snapshot.Value(VerifierCounterId::kP5SytStepViolation),
                snapshot.Value(VerifierCounterId::kP5SytGraftViolation));

        if (ivars->controller && ivars->controller->LabTimingEnabled()) {
            const auto& timing = ivars->controller->TimingCounters();
            LAB_LOG("dump timing: data_syts=%{public}llu seeds=%{public}llu tight=%{public}llu "
                    "late=%{public}llu gate=%{public}llu escalate=%{public}llu last_lead=%{public}lld",
                    timing.dataPackets, timing.seeds, timing.tightWarn,
                    timing.late, timing.gate, timing.escalate,
                    timing.lastLeadTicks);
        }

        LAB_LOG("dump packets: published=%{public}llu data=%{public}llu nodata=%{public}llu "
                "acquire_failures=%{public}llu",
                snapshot.Value(VerifierCounterId::kPacketsPublished),
                snapshot.Value(VerifierCounterId::kDataPackets),
                snapshot.Value(VerifierCounterId::kNoDataPackets),
                snapshot.Value(VerifierCounterId::kAcquireFailures));

        if (snapshot.firstViolationValid) {
            LAB_LOG("dump verifier_first: id=%{public}u packet=%{public}llu",
                    snapshot.firstViolationId,
                    snapshot.firstViolationPacketIndex);
        }

        if (ivars->controller) {
            const auto& payload = ivars->controller->PayloadCounters();
            LAB_LOG("dump payload: visited=%{public}llu written=%{public}llu "
                    "without_packet=%{public}llu outside_packet=%{public}llu",
                    payload.framesVisited.load(std::memory_order_relaxed),
                    payload.framesWritten.load(std::memory_order_relaxed),
                    payload.framesWithoutPacket.load(std::memory_order_relaxed),
                    payload.framesOutsidePacket.load(std::memory_order_relaxed));
        }
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
