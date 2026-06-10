#include <new>
#include <atomic>
#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include "VirtualAudioDevice.h"
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

    mach_timebase_info_data_t timebaseInfo{};
    if (mach_timebase_info(&timebaseInfo) == KERN_SUCCESS &&
        timebaseInfo.numer != 0 && timebaseInfo.denom != 0) {
        ivars->timebase.numer = timebaseInfo.numer;
        ivars->timebase.denom = timebaseInfo.denom;
    }

    ivars->controller = new VirtualAudioDeviceController();
    if (!ivars->controller->Initialize()) {
        IOLog("VirtualAudioDevice: Failed to initialize controller\n");
        return false;
    }

    // Step 6 instrument under real pacing: Verifying(Fake) for the whole run.
    ivars->diagSink = new ASFW::Lab::StickyCounterSink();
    ivars->verifier = new ASFW::Lab::VerifyingSlotProvider(
        ivars->controller->FakeSlotProvider(),
        ASFW::Lab::VerifyingSlotProvider::Config{true, 0x02, ivars->diagSink});
    ivars->controller->BindLabSlotProvider(ivars->verifier);

    // Pick Saffire for testing
    ASFW::Protocols::Audio::DICE::DiceDeviceIdentity identity{};
    identity.vendorId = 0x00130e; // Focusrite
    ivars->controller->SelectProfile(identity);

    // The lab device reports as a FireWire-transport clock device — that is
    // the contract ASFW will live under (AudioDriverKitTypes.h '1394').
    SetTransportType(IOUserAudioTransportType::FireWire);

    // Set up a basic float32 output stream (2 channels, 48kHz)
    double sampleRate = kSampleRate;
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

    // Ring = 8 ZTS periods. A one-period ring leaves the HAL zero headroom
    // around the wrap anchor; 8 matches the host scenario pump (4096 frames).
    // C4 (period vs IO buffer size coupling) stays observable by varying this.
    ivars->ringFrames = kRingPeriods * in_zero_timestamp_period;

    OSSharedPtr<IOBufferMemoryDescriptor> buffer;
    uint32_t bufferSize = ivars->ringFrames * format.mBytesPerFrame;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, bufferSize, 0, buffer.attach()) == kIOReturnSuccess) {
        ivars->outputStream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Output, buffer.get());
        if (ivars->outputStream) {
            ivars->outputStream->SetAvailableStreamFormats(&format, 1);
            ivars->outputStream->SetCurrentStreamFormat(&format);
            AddStream(ivars->outputStream.get());
        }
    }

    ivars->controller->ConfigureOutputStream(kSampleRate, ivars->outputChannels,
                                             ivars->ringFrames);

    // The ZTS heartbeat timer (armed in StartIO).
    IOTimerDispatchSource* timer = nullptr;
    if (IOTimerDispatchSource::Create(ivars->workQueue.get(), &timer) == kIOReturnSuccess) {
        ivars->ztsTimer = OSSharedPtr(timer, OSNoRetain);
        OSAction* action = nullptr;
        if (CreateActionZtsTimerOccurred(0, &action) == kIOReturnSuccess) {
            ivars->ztsTimerAction = OSSharedPtr(action, OSNoRetain);
            ivars->ztsTimer->SetHandler(ivars->ztsTimerAction.get());
        } else {
            IOLog("VirtualAudioDevice: failed to create ZTS timer action\n");
            return false;
        }
    } else {
        IOLog("VirtualAudioDevice: failed to create ZTS timer\n");
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

    SetIOOperationHandler(io_operation);

    return true;
}

void VirtualAudioDevice::free()
{
    if (ivars != nullptr) {
        // O2 answer at teardown: how many callbacks arrived after StopIO.
        IOLog("ADKLab[free] io_after_stop=%llu timer_after_stop=%llu\n",
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
        if (!ivars->controller->PrepareLabPacket(ivars->nextPacketIndex, 0xFFFF,
                                                 false)) {
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
    PrepareCoverage(ivars, sampleTime + 2ull * GetZeroTimestampPeriod());

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
    IOLog("VirtualAudioDevice: StartIO\n");

    __block kern_return_t kr = kIOReturnSuccess;
    ivars->workQueue->DispatchSync(^(){
        kr = super::StartIO(in_flags);
        if (kr != kIOReturnSuccess) {
            return;
        }
        if (ivars->outputStream) {
            auto buffer = ivars->outputStream->GetIOMemoryDescriptor();
            if (buffer) {
                buffer->CreateMapping(0, 0, 0, 0, 0, ivars->outputMemoryMap.attach());
            }
        }
        if (ivars->controller) {
            ivars->controller->ResetTransportLab(0, 0);
        }
        if (ivars->verifier) {
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
        UpdateCurrentZeroTimestamp(0, ivars->startHostTime);
        ivars->anchorsPublished.fetch_add(1, std::memory_order_relaxed);
        ivars->anchorsBeforeFirstWriteEnd.fetch_add(1, std::memory_order_relaxed);

        if (ivars->controller) {
            PrepareCoverage(ivars, 2ull * GetZeroTimestampPeriod());
        }

        ivars->ioRunning.store(true, std::memory_order_relaxed);
        ivars->periodIndex = 1;
        const uint64_t deadline =
            ivars->startHostTime +
            ivars->timebase.NsToTicks(NsForPeriodIndex(1));
        const uint64_t leeway = ivars->timebase.NsToTicks(500000);
        if (ivars->ztsTimer) {
            ivars->ztsTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, deadline,
                                        leeway);
        }
    });

    return kr;
}

kern_return_t VirtualAudioDevice::StopIO(IOUserAudioStartStopFlags in_flags)
{
    IOLog("VirtualAudioDevice: StopIO\n");

    __block kern_return_t kr = kIOReturnSuccess;
    ivars->workQueue->DispatchSync(^(){
        ivars->ioRunning.store(false, std::memory_order_relaxed);

        kr = super::StopIO(in_flags);
        ivars->outputMemoryMap.reset();

        // ---- M3 dump (StopIO may take as long as necessary) ----
        const auto snapshot = ivars->verifier ? ivars->verifier->Snapshot()
                                              : ASFW::Lab::VerifierSnapshot{};
        using ASFW::Lab::VerifierCounterId;

        IOLog("ADKLab[dump] zts: anchors=%llu before_first_io=%llu period=%u "
              "ring_frames=%u prepare_failures=%llu\n",
              ivars->anchorsPublished.load(std::memory_order_relaxed),
              ivars->anchorsBeforeFirstWriteEnd.load(std::memory_order_relaxed),
              GetZeroTimestampPeriod(), ivars->ringFrames,
              ivars->prepareFailures);

        const uint64_t firstHost =
            ivars->firstWriteEndHostTime.load(std::memory_order_relaxed);
        IOLog("ADKLab[dump] writeend: count=%llu frames=%llu min=%u max=%u "
              "sample_breaks=%llu first_sample=%llu first_host_delta=%lld "
              "other_ops=%llu\n",
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

        IOLog("ADKLab[dump] verifier: violations=%llu p1_win=%llu p1_run=%llu "
              "p1_idx=%llu p2_dbc=%llu p3_bytes=%llu p3_q0=%llu p3_q1=%llu "
              "p3_unacq=%llu p4_tile=%llu p4_cnt=%llu\n",
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
              snapshot.Value(VerifierCounterId::kP4FrameCountViolation));

        IOLog("ADKLab[dump] packets: published=%llu data=%llu nodata=%llu "
              "acquire_failures=%llu\n",
              snapshot.Value(VerifierCounterId::kPacketsPublished),
              snapshot.Value(VerifierCounterId::kDataPackets),
              snapshot.Value(VerifierCounterId::kNoDataPackets),
              snapshot.Value(VerifierCounterId::kAcquireFailures));

        if (snapshot.firstViolationValid) {
            IOLog("ADKLab[dump] verifier_first: id=%u packet=%llu\n",
                  snapshot.firstViolationId,
                  snapshot.firstViolationPacketIndex);
        }

        if (ivars->controller) {
            const auto& payload = ivars->controller->PayloadCounters();
            IOLog("ADKLab[dump] payload: visited=%llu written=%llu "
                  "without_packet=%llu outside_packet=%llu\n",
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
