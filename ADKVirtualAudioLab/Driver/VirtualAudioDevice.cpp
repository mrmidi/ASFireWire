#include <new>
#include <atomic>
#include <cstring>
#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <os/log.h>
#include "VirtualAudioDevice.h"

#define LAB_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[ADKLab] " fmt, ##__VA_ARGS__)
#define ADK_CONFIG_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[ADKConfig] " fmt, ##__VA_ARGS__)
#include "../Core/VirtualAudioDeviceController.hpp"
#include "../Lab/ADKConfigChange.hpp"
#include "../Lab/LabConfigurationCoordinator.hpp"
#include "../Lab/PacketDumpBlob.hpp"
#include "../Lab/StickyCounterSink.hpp"
#include "../Lab/VerifyingSlotProvider.hpp"
#include "../Runtime/VirtualDeviceRegistry.hpp"

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
// - RT discipline (post-M3 crash fix): the ring is mapped once in init()
//   and its raw base cached before SetIOOperationHandler; the IO block uses
//   only cached state and is fully gated by ioRunning; StartIO does all
//   driver-side prep first and calls super last; slot access on the RT side
//   goes through the timeline's generation seqlock (see AmdtpPacketTimeline).

namespace {

constexpr uint32_t kInitialSampleRate = ASFW::Lab::kADKConfigRateB;
constexpr uint32_t kAlternateSampleRate = ASFW::Lab::kADKConfigRateA;
constexpr uint32_t kMaxLabChannels = 32; // bound for profile + HAL layouts
constexpr uint32_t kBytesPerSample = sizeof(float);

// C4 rig: declared per-direction latency/safety-offset under test. Set to
// Phase88's current shipped values as the baseline run; edit + rebuild +
// reload to sweep candidates (e.g. kLabOutputSafetyOffsetFrames = 48) and
// diff the StopIO dump against this baseline. These are the exact ADK
// properties ASFWAudioDriverGraph.cpp sets on the real device
// (SetOutputSafetyOffset/SetInputSafetyOffset/SetOutputLatency/
// SetInputLatency) -- unlike the Python simulator, these values are read
// back by the REAL AudioDriverKit HAL scheduler, not a guessed model of it.
constexpr uint32_t kLabOutputSafetyOffsetFrames = 64;
constexpr uint32_t kLabInputSafetyOffsetFrames = 128;
constexpr uint32_t kLabOutputLatencyFrames = 128;
constexpr uint32_t kLabInputLatencyFrames = 128;
constexpr uint32_t kMaxPreparePerCall = 512; // runaway guard for the pump

struct LabHALDeviceShape final {
    uint32_t inputChannels{0};
    uint32_t outputChannels{0};
};

struct LabDeviceConfiguration final {
    uint32_t sampleRate{0};
    uint32_t opticalInput{static_cast<uint32_t>(ASFW::Lab::ADKConfigOpticalMode::None)};
    uint32_t opticalOutput{static_cast<uint32_t>(ASFW::Lab::ADKConfigOpticalMode::None)};
};

const ASFW::Runtime::VirtualDeviceDefinition* FindLabDeviceDefinition(
    OSString* deviceUID) noexcept
{
    if (deviceUID == nullptr || deviceUID->getCStringNoCopy() == nullptr) {
        return nullptr;
    }

    using ASFW::Runtime::VirtualDeviceKind;
    VirtualDeviceKind kind{};
    const char* uid = deviceUID->getCStringNoCopy();
    if (std::strcmp(uid, "VirtualADKAudioLab.Duet") == 0) {
        kind = VirtualDeviceKind::Duet;
    } else if (std::strcmp(uid, "VirtualADKAudioLab.Phase88") == 0) {
        kind = VirtualDeviceKind::Phase88;
    } else if (std::strcmp(uid, "VirtualADKAudioLab.FW1814") == 0) {
        kind = VirtualDeviceKind::FW1814;
    } else if (std::strcmp(uid, "VirtualADKAudioLab.Saffire") == 0) {
        kind = VirtualDeviceKind::SaffirePro24DSP;
    } else {
        return nullptr;
    }

    return ASFW::Runtime::findVirtualDevice(kind);
}

uint32_t EncodeOpticalMode(
    const std::optional<ASFW::Device::OpticalMode>& mode) noexcept
{
    if (!mode.has_value()) {
        return static_cast<uint32_t>(ASFW::Lab::ADKConfigOpticalMode::None);
    }
    return *mode == ASFW::Device::OpticalMode::Adat
        ? static_cast<uint32_t>(ASFW::Lab::ADKConfigOpticalMode::Adat)
        : static_cast<uint32_t>(ASFW::Lab::ADKConfigOpticalMode::Spdif);
}

bool DecodeOpticalMode(uint32_t raw,
                       std::optional<ASFW::Device::OpticalMode>& outMode) noexcept
{
    switch (static_cast<ASFW::Lab::ADKConfigOpticalMode>(raw)) {
    case ASFW::Lab::ADKConfigOpticalMode::None:
        outMode.reset();
        return true;
    case ASFW::Lab::ADKConfigOpticalMode::Adat:
        outMode = ASFW::Device::OpticalMode::Adat;
        return true;
    case ASFW::Lab::ADKConfigOpticalMode::Spdif:
        outMode = ASFW::Device::OpticalMode::Spdif;
        return true;
    }
    return false;
}

bool DecodeLabConfiguration(
    const LabDeviceConfiguration& configuration,
    ASFW::Device::DeviceConfiguration& outConfiguration) noexcept
{
    outConfiguration = ASFW::Device::DeviceConfiguration{};
    outConfiguration.sampleRate = configuration.sampleRate;
    return DecodeOpticalMode(configuration.opticalInput,
                             outConfiguration.opticalInput) &&
           DecodeOpticalMode(configuration.opticalOutput,
                             outConfiguration.opticalOutput);
}

LabDeviceConfiguration EncodeLabConfiguration(
    const ASFW::Device::DeviceConfiguration& configuration) noexcept
{
    return LabDeviceConfiguration{
        .sampleRate = configuration.sampleRate,
        .opticalInput = EncodeOpticalMode(configuration.opticalInput),
        .opticalOutput = EncodeOpticalMode(configuration.opticalOutput),
    };
}

bool ResolveLabHALDeviceShape(
    const ASFW::Runtime::VirtualDeviceDefinition* definition,
    const LabDeviceConfiguration& configuration,
    LabHALDeviceShape& outShape) noexcept
{
    if (definition == nullptr) {
        return false;
    }

    ASFW::Device::DeviceConfiguration modelConfiguration{};
    if (!DecodeLabConfiguration(configuration, modelConfiguration)) {
        return false;
    }

    auto resolved = definition->resolve(modelConfiguration);
    if (!resolved.has_value()) {
        return false;
    }

    outShape = LabHALDeviceShape{};
    for (const auto& stream : resolved->streams.streams) {
        if (stream.direction == ASFW::Device::StreamDirection::Capture) {
            outShape.inputChannels += stream.channels;
        } else {
            outShape.outputChannels += stream.channels;
        }
    }
    return outShape.inputChannels != 0 && outShape.outputChannels != 0 &&
           outShape.inputChannels <= kMaxLabChannels &&
           outShape.outputChannels <= kMaxLabChannels;
}

struct LabTimebase final {
    uint32_t numer{1};
    uint32_t denom{1};

    uint64_t NsToTicks(uint64_t ns) const noexcept {
        return (numer == denom) ? ns : (ns * denom) / numer;
    }
};

// Nominal nanoseconds elapsed after n ZTS periods (exact thirds, no
// accumulated rounding: computed from n, not incrementally).
inline uint64_t NsForPeriodIndex(uint64_t n,
                                 uint32_t sampleRate,
                                 uint32_t periodFrames) noexcept {
    if (sampleRate == 0) {
        return 0;
    }
    return (n * static_cast<uint64_t>(periodFrames) * 1000000000ull) /
           static_cast<uint64_t>(sampleRate);
}

} // namespace

struct VirtualAudioDevice_IVars
{
    OSSharedPtr<IOUserAudioDriver> driver;
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<IOUserAudioStream> outputStream;
    OSSharedPtr<IOMemoryMap> outputMemoryMap;
    OSSharedPtr<IOUserAudioStream> inputStream;
    OSSharedPtr<IOMemoryMap> inputMemoryMap;
    OSSharedPtr<IOTimerDispatchSource> ztsTimer;
    OSSharedPtr<OSAction> ztsTimerAction;

    VirtualAudioDeviceController* controller{nullptr};
    ASFW::Lab::VerifyingSlotProvider* verifier{nullptr};
    ASFW::Lab::StickyCounterSink* diagSink{nullptr};

    // Configuration-change experiment state. The lock protects only this
    // control-plane state and the bounded diagnostic ring; it is never used
    // from SetIOOperationHandler or any timer/packet hot path.
    IOLock* configLock{nullptr};
    uint32_t labSlot{0};
    const ASFW::Runtime::VirtualDeviceDefinition* deviceDefinition{nullptr};
    ASFW::Lab::LabConfigurationCoordinator* configurationCoordinator{nullptr};
    // The coordinator is authoritative. These fields are snapshots used by
    // ADK projection and the diagnostic wire format; they never decide a
    // transaction on their own.
    LabDeviceConfiguration currentConfiguration{};
    LabDeviceConfiguration pendingConfiguration{};
    LabHALDeviceShape currentShape{};
    LabHALDeviceShape pendingShape{};
    // The first configuration is chosen as each model's maximum advertised
    // geometry. Structural lab changes may shrink and later restore this
    // geometry while retaining the original mappings; streaming reallocation
    // is deliberately outside this experiment.
    LabHALDeviceShape allocatedShape{};
    std::atomic<uint32_t> currentSampleRate{kInitialSampleRate};
    uint64_t pendingAction{0};
    uint64_t nextAction{1};
    bool configurationPending{false};
    uint64_t nextConfigSequence{1};
    uint32_t configWriteIndex{0};
    uint32_t configEventCount{0};
    ASFW::Lab::ADKConfigEvent configEvents[ASFW::Lab::kADKConfigLogMaxEvents]{};

    // Cached IO-path state, per the SetIOOperationHandler contract (RT
    // thread, cached/captured info only): all four are set in init() before
    // the handler is registered and never mutated afterwards — the mapping
    // lives for the device lifetime, so no Start/Stop lifecycle race.
    float* ringBase{nullptr};
    uint32_t outputBytesPerFrame{0};
    uint32_t outputChannels{0};
    uint32_t ringFrames{0};

    // C4 duplex rig: the input ring's "hardware" fill cursor is advanced by
    // the same ZTS timer that stands in for the OHCI IT ring interrupt (see
    // ZtsTimerOccurred_Impl) -- independent of when the HAL calls BeginRead,
    // matching how real capture hardware fills continuously in the
    // background. inputRingBase is cached before SetIOOperationHandler for
    // the same RT-discipline reason as ringBase.
    float* inputRingBase{nullptr};
    uint32_t inputBytesPerFrame{0};
    uint32_t inputChannels{0};

    LabTimebase timebase{};

    // Clock chain state (work-queue confined).
    uint64_t startHostTime{0};
    uint64_t periodIndex{0};

    // Packet pump state (work-queue confined).
    uint32_t nextPacketIndex{0};
    uint64_t exposedFrames{0};
    uint64_t prepareFailures{0};

    // C4 rig: simulated hardware capture cursor, advanced on the work queue
    // by ZtsTimerOccurred_Impl. "Frames captured up through this point are
    // safe to read." Cross-queue read from the RT BeginRead handler, so
    // atomic (relaxed -- advisory instrumentation, not a correctness gate).
    std::atomic<uint64_t> capturedFrames{0};

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
    std::atomic<uint64_t> payloadCommittedEndFrame{0};
    std::atomic<bool> payloadCommittedValid{false};
    std::atomic<uint64_t> firstWriteEndSampleTime{0};
    std::atomic<uint64_t> firstWriteEndHostTime{0};
    std::atomic<uint64_t> otherIoOperations{0};
    std::atomic<uint64_t> ioAfterStop{0};      // O2: WriteEnd after StopIO
    std::atomic<uint64_t> timerAfterStop{0};   // O2: timer fire after StopIO

    // C4 rig: BeginRead-side mirror of the WriteEnd instrumentation above,
    // plus the capture-readiness measurement that actually answers the
    // question (does the real HAL ever call BeginRead for a span the
    // simulated hardware capture cursor hasn't reached yet?).
    std::atomic<uint64_t> beginReadCount{0};
    std::atomic<uint64_t> framesRequested{0};
    std::atomic<uint32_t> minReadIoFrames{0xFFFFFFFFu};
    std::atomic<uint32_t> maxReadIoFrames{0};
    std::atomic<uint64_t> readSampleTimeBreaks{0};
    std::atomic<uint64_t> expectedNextReadSampleTime{0};
    std::atomic<bool> expectedReadSampleTimeValid{false};
    std::atomic<uint64_t> firstBeginReadSampleTime{0};
    std::atomic<uint64_t> firstBeginReadHostTime{0};
    std::atomic<uint64_t> captureStarvations{0};  // in_sample_time+size > capturedFrames
    std::atomic<int64_t> minCaptureMarginFrames{INT64_MAX}; // capturedFrames - (sample_time+size)
    std::atomic<uint64_t> readAfterStop{0};       // O2: BeginRead after StopIO
};

static const char* ConfigPhaseName(ASFW::Lab::ADKConfigPhase phase) noexcept
{
    using ASFW::Lab::ADKConfigPhase;
    switch (phase) {
        case ADKConfigPhase::HostRequest: return "HostRequest";
        case ADKConfigPhase::RequestCalled: return "RequestCalled";
        case ADKConfigPhase::RequestReturned: return "RequestReturned";
        case ADKConfigPhase::RequestRejected: return "RequestRejected";
        case ADKConfigPhase::StopIOEnter: return "StopIOEnter";
        case ADKConfigPhase::StopIOReturn: return "StopIOReturn";
        case ADKConfigPhase::PerformEnter: return "PerformEnter";
        case ADKConfigPhase::PerformMutation: return "PerformMutation";
        case ADKConfigPhase::PerformSuper: return "PerformSuper";
        case ADKConfigPhase::PerformReturn: return "PerformReturn";
        case ADKConfigPhase::AbortEnter: return "AbortEnter";
        case ADKConfigPhase::AbortSuper: return "AbortSuper";
        case ADKConfigPhase::AbortReturn: return "AbortReturn";
        case ADKConfigPhase::StartIOEnter: return "StartIOEnter";
        case ADKConfigPhase::StartIOReturn: return "StartIOReturn";
        case ADKConfigPhase::HandleSampleRateEnter: return "HandleSampleRateEnter";
        case ADKConfigPhase::HandleSampleRateReturn: return "HandleSampleRateReturn";
        case ADKConfigPhase::DeviceRateMutation: return "DeviceRateMutation";
        case ADKConfigPhase::OutputStreamMutation: return "OutputStreamMutation";
        case ADKConfigPhase::InputStreamMutation: return "InputStreamMutation";
        case ADKConfigPhase::CandidateAccepted: return "CandidateAccepted";
        case ADKConfigPhase::CandidateRejected: return "CandidateRejected";
        case ADKConfigPhase::HardwareApply: return "HardwareApply";
        case ADKConfigPhase::HardwareCompleted: return "HardwareCompleted";
        case ADKConfigPhase::HardwareUnchanged: return "HardwareUnchanged";
        case ADKConfigPhase::HardwareUnknown: return "HardwareUnknown";
        case ADKConfigPhase::ProjectionCommitted: return "ProjectionCommitted";
        case ADKConfigPhase::CoordinatorRejected: return "CoordinatorRejected";
        case ADKConfigPhase::HardwareObserved: return "HardwareObserved";
        default: return "Unknown";
    }
}

static void AppendConfigEventLocked(
    VirtualAudioDevice* device,
    VirtualAudioDevice_IVars* ivars,
    ASFW::Lab::ADKConfigPhase phase,
    uint64_t action,
    kern_return_t result,
    uint32_t oldSampleRate,
    uint32_t newSampleRate,
    uint32_t oldChannels = 0,
    uint32_t newChannels = 0) noexcept
{
    auto& event = ivars->configEvents[ivars->configWriteIndex];
    event = ASFW::Lab::ADKConfigEvent{};
    event.sequence = ivars->nextConfigSequence++;
    event.hostTimeTicks = mach_absolute_time();
    event.deviceSlot = ivars->labSlot;
    event.deviceObjectID = static_cast<uint32_t>(device->GetObjectID());
    event.action = action;
    event.phase = static_cast<uint32_t>(phase);
    event.result = static_cast<int32_t>(result);
    event.oldSampleRate = oldSampleRate;
    event.newSampleRate = newSampleRate;
    event.configurationPending = ivars->configurationPending ? 1u : 0u;
    event.streamChannelCounts =
        ASFW::Lab::PackADKConfigChannelCounts(oldChannels, newChannels);

    ivars->configWriteIndex =
        (ivars->configWriteIndex + 1u) % ASFW::Lab::kADKConfigLogMaxEvents;
    if (ivars->configEventCount < ASFW::Lab::kADKConfigLogMaxEvents) {
        ++ivars->configEventCount;
    }
}

static void RecordConfigEvent(
    VirtualAudioDevice* device,
    VirtualAudioDevice_IVars* ivars,
    ASFW::Lab::ADKConfigPhase phase,
    uint64_t action,
    kern_return_t result,
    uint32_t oldSampleRate,
    uint32_t newSampleRate,
    uint32_t oldChannels = 0,
    uint32_t newChannels = 0) noexcept
{
    if (device == nullptr || ivars == nullptr || ivars->configLock == nullptr) {
        return;
    }

    IOLockLock(ivars->configLock);
    AppendConfigEventLocked(device, ivars, phase, action, result,
                            oldSampleRate, newSampleRate,
                            oldChannels, newChannels);
    const uint32_t pending = ivars->configurationPending ? 1u : 0u;
    const uint32_t slot = ivars->labSlot;
    IOLockUnlock(ivars->configLock);

    ADK_CONFIG_LOG("slot=%{public}u object=0x%{public}x phase=%{public}s action=%{public}llu result=0x%{public}08x old_rate=%{public}u new_rate=%{public}u old_channels=%{public}u new_channels=%{public}u pending=%{public}u",
                   slot, static_cast<uint32_t>(device->GetObjectID()),
                   ConfigPhaseName(phase), action,
                   static_cast<uint32_t>(result), oldSampleRate, newSampleRate,
                   oldChannels, newChannels, pending);
}

// The reducer owns lifecycle/pending state.  Keep the old POD fields as an
// atomically refreshed view for the diagnostic ABI and ADK projection code.
// Caller holds configLock.
static void SynchronizeCoordinatorSnapshotLocked(
    VirtualAudioDevice_IVars* ivars) noexcept
{
    if (ivars == nullptr || ivars->configurationCoordinator == nullptr) {
        return;
    }
    const auto snapshot = ivars->configurationCoordinator->Snapshot();
    ivars->currentConfiguration = EncodeLabConfiguration(snapshot.committed.configuration);
    LabHALDeviceShape currentShape{};
    if (ResolveLabHALDeviceShape(ivars->deviceDefinition,
                                 ivars->currentConfiguration, currentShape)) {
        ivars->currentShape = currentShape;
    }

    ivars->configurationPending = snapshot.pendingIdentity.has_value();
    ivars->pendingAction = snapshot.pendingIdentity
        ? snapshot.pendingIdentity->token : 0;
    ivars->pendingConfiguration = snapshot.pendingConfiguration
        ? EncodeLabConfiguration(*snapshot.pendingConfiguration)
        : LabDeviceConfiguration{};
    ivars->pendingShape = LabHALDeviceShape{};
    if (snapshot.pendingConfiguration) {
        (void)ResolveLabHALDeviceShape(ivars->deviceDefinition,
                                       ivars->pendingConfiguration,
                                       ivars->pendingShape);
    }
}

static kern_return_t CoordinatorErrorToIOReturn(
    ASFW::Configuration::StateMachineError error) noexcept
{
    using ASFW::Configuration::StateMachineError;
    switch (error) {
    case StateMachineError::Busy:
        return kIOReturnBusy;
    case StateMachineError::InvalidEvent:
    case StateMachineError::EndpointMismatch:
    case StateMachineError::GenerationMismatch:
    case StateMachineError::StaleToken:
        return kIOReturnBadArgument;
    }
    return kIOReturnError;
}

static bool DispatchCoordinatorLocked(
    VirtualAudioDevice_IVars* ivars,
    const ASFW::Configuration::ConfigurationEvent& event,
    ASFW::Configuration::TransitionResult& outTransition,
    kern_return_t& outError) noexcept
{
    if (ivars == nullptr || ivars->configurationCoordinator == nullptr) {
        outError = kIOReturnNotReady;
        return false;
    }
    const auto transition = ivars->configurationCoordinator->Dispatch(event);
    if (!transition) {
        outError = CoordinatorErrorToIOReturn(transition.error());
        return false;
    }
    outTransition = *transition;
    SynchronizeCoordinatorSnapshotLocked(ivars);
    outError = kIOReturnSuccess;
    return true;
}

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
    ivars->configLock = IOLockAlloc();
    if (ivars->configLock == nullptr) {
        LAB_LOG("init - failed to allocate configuration log lock");
        return false;
    }
    ivars->currentSampleRate.store(kInitialSampleRate, std::memory_order_relaxed);
    ivars->nextAction = 1;
    ivars->nextConfigSequence = 1;
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

    // Device caps come from the selected profile, not from constants here:
    // the profile carries the bench-confirmed stream shape and the HAL
    // format/channel layout must follow it.
    ASFW::Driver::OutputDeviceCaps caps{};
    if (!ivars->controller->GetOutputDeviceCaps(caps) ||
        caps.pcmChannels == 0 || caps.pcmChannels > kMaxLabChannels) {
        LAB_LOG("init - GetOutputDeviceCaps failed (pcmChannels = %{public}u)",
                caps.pcmChannels);
        return false;
    }
    if (caps.sampleRate != kInitialSampleRate) {
        // The initial lab profile remains 48 kHz. The configuration experiment
        // can then request the alternate advertised format after startup.
        LAB_LOG("init - profile sample rate %{public}u unsupported by lab clock chain",
                caps.sampleRate);
        return false;
    }
    LAB_LOG("init - device caps from profile: %{public}u ch @ %{public}u Hz",
            caps.pcmChannels, caps.sampleRate);

    // HAL topology comes from the lab's device model, which is also what the
    // CLI reports. The Saffire DICE profile above remains only the dormant
    // packet fixture; it must not leak its 8-channel TX shape into every
    // published CoreAudio device.
    ivars->deviceDefinition = FindLabDeviceDefinition(in_device_uid);
    if (ivars->deviceDefinition == nullptr) {
        LAB_LOG("init - failed to find model definition for %{public}s",
                in_device_uid ? in_device_uid->getCStringNoCopy() : "NULL");
        return false;
    }
    ivars->currentConfiguration = EncodeLabConfiguration(
        ivars->deviceDefinition->defaultConfiguration());

    LabHALDeviceShape halShape{};
    if (!ResolveLabHALDeviceShape(ivars->deviceDefinition,
                                  ivars->currentConfiguration, halShape)) {
        LAB_LOG("init - failed to resolve HAL shape for %{public}s",
                in_device_uid ? in_device_uid->getCStringNoCopy() : "NULL");
        return false;
    }
    ivars->currentShape = halShape;
    ivars->allocatedShape = halShape;
    ivars->currentSampleRate.store(ivars->currentConfiguration.sampleRate,
                                   std::memory_order_relaxed);
    ASFW::Device::DeviceConfiguration initialConfiguration{};
    if (!DecodeLabConfiguration(ivars->currentConfiguration, initialConfiguration)) {
        LAB_LOG("init - failed to decode initial configuration");
        return false;
    }
    ivars->configurationCoordinator = new ASFW::Lab::LabConfigurationCoordinator(
        1, 1, initialConfiguration);
    if (ivars->configurationCoordinator == nullptr) {
        LAB_LOG("init - failed to allocate configuration coordinator");
        return false;
    }
    LAB_LOG("init - HAL model shape: %{public}u input / %{public}u output",
            halShape.inputChannels, halShape.outputChannels);

    // The lab device reports as a FireWire-transport clock device — that is
    // the contract ASFW will live under (AudioDriverKitTypes.h '1394').
    LAB_LOG("init - setting transport type to FireWire");
    SetTransportType(IOUserAudioTransportType::FireWire);

    // Discrete channel layouts sized by the model/HAL caps (CoreAudio discrete
    // labels are contiguous from Discrete_0).
    LAB_LOG("init - setting preferred output channel layout");
    IOUserAudioChannelLabel outputChannelLayout[kMaxLabChannels] = {};
    for (uint32_t ch = 0; ch < halShape.outputChannels; ++ch) {
        outputChannelLayout[ch] = static_cast<IOUserAudioChannelLabel>(
            static_cast<uint32_t>(IOUserAudioChannelLabel::Discrete_0) + ch);
    }
    kern_return_t kr = SetPreferredOutputChannelLayout(
        outputChannelLayout, halShape.outputChannels);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetPreferredOutputChannelLayout failed (kr = 0x%{public}08x)", kr);
    } else {
        LAB_LOG("init - SetPreferredOutputChannelLayout succeeded");
    }

    LAB_LOG("init - setting preferred input channel layout");
    IOUserAudioChannelLabel inputChannelLayout[kMaxLabChannels] = {};
    for (uint32_t ch = 0; ch < halShape.inputChannels; ++ch) {
        inputChannelLayout[ch] = static_cast<IOUserAudioChannelLabel>(
            static_cast<uint32_t>(IOUserAudioChannelLabel::Discrete_0) + ch);
    }
    kr = SetPreferredInputChannelLayout(inputChannelLayout,
                                        halShape.inputChannels);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetPreferredInputChannelLayout failed (kr = 0x%{public}08x)", kr);
    } else {
        LAB_LOG("init - SetPreferredInputChannelLayout succeeded");
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

    // C4 rig: declared per-direction latency/safety-offset, matching the
    // real ADK properties ASFWAudioDriverGraph.cpp sets on Phase88 (see
    // kLabOutputSafetyOffsetFrames et al. above).
    LAB_LOG("init - setting output safety offset to %{public}u", kLabOutputSafetyOffsetFrames);
    kr = SetOutputSafetyOffset(kLabOutputSafetyOffsetFrames);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetOutputSafetyOffset failed (kr = 0x%{public}08x)", kr);
    } else {
        LAB_LOG("init - SetOutputSafetyOffset set to %{public}u", kLabOutputSafetyOffsetFrames);
    }

    LAB_LOG("init - setting input safety offset to %{public}u", kLabInputSafetyOffsetFrames);
    kr = SetInputSafetyOffset(kLabInputSafetyOffsetFrames);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetInputSafetyOffset failed (kr = 0x%{public}08x)", kr);
    } else {
        LAB_LOG("init - SetInputSafetyOffset set to %{public}u", kLabInputSafetyOffsetFrames);
    }

    LAB_LOG("init - setting output latency to %{public}u", kLabOutputLatencyFrames);
    kr = SetOutputLatency(kLabOutputLatencyFrames);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetOutputLatency failed (kr = 0x%{public}08x)", kr);
    } else {
        LAB_LOG("init - SetOutputLatency set to %{public}u", kLabOutputLatencyFrames);
    }

    LAB_LOG("init - setting input latency to %{public}u", kLabInputLatencyFrames);
    kr = SetInputLatency(kLabInputLatencyFrames);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetInputLatency failed (kr = 0x%{public}08x)", kr);
    } else {
        LAB_LOG("init - SetInputLatency set to %{public}u", kLabInputLatencyFrames);
    }

    // Float32 streams shaped by the model/HAL caps. The second format is
    // advertised only to make the ADK configuration transaction observable;
    // the lab does not claim that its packet timing rig is playback-ready at
    // every advertised rate yet.
    double sampleRates[2] = {
        static_cast<double>(kAlternateSampleRate),
        static_cast<double>(kInitialSampleRate),
    };
    LAB_LOG("init - setting available sample rates to %{public}.1f and %{public}.1f",
            sampleRates[0], sampleRates[1]);
    kr = SetAvailableSampleRates(sampleRates, 2);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetAvailableSampleRates failed (kr = 0x%{public}08x)", kr);
        return false;
    }
    const double sampleRate = static_cast<double>(
        ivars->currentConfiguration.sampleRate);
    LAB_LOG("init - setting current sample rate to %{public}.1f", sampleRate);
    kr = SetSampleRate(sampleRate);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - SetSampleRate failed (kr = 0x%{public}08x)", kr);
        return false;
    }

    const uint32_t outputBytesPerFrame =
        halShape.outputChannels * kBytesPerSample;
    IOUserAudioStreamBasicDescription outputFormat = {
        .mSampleRate = sampleRate,
        .mFormatID = IOUserAudioFormatID::LinearPCM,
        .mFormatFlags = static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsFloat | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
        .mBytesPerPacket = outputBytesPerFrame,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = outputBytesPerFrame,
        .mChannelsPerFrame = halShape.outputChannels,
        .mBitsPerChannel = 32
    };
    IOUserAudioStreamBasicDescription alternateOutputFormat = outputFormat;
    alternateOutputFormat.mSampleRate =
        static_cast<double>(kAlternateSampleRate);
    IOUserAudioStreamBasicDescription availableOutputFormats[2] = {
        alternateOutputFormat,
        outputFormat,
    };

    const uint32_t inputBytesPerFrame =
        halShape.inputChannels * kBytesPerSample;
    IOUserAudioStreamBasicDescription inputFormat = outputFormat;
    inputFormat.mBytesPerPacket = inputBytesPerFrame;
    inputFormat.mBytesPerFrame = inputBytesPerFrame;
    inputFormat.mChannelsPerFrame = halShape.inputChannels;
    IOUserAudioStreamBasicDescription alternateInputFormat = inputFormat;
    alternateInputFormat.mSampleRate = static_cast<double>(kAlternateSampleRate);
    IOUserAudioStreamBasicDescription availableInputFormats[2] = {
        alternateInputFormat,
        inputFormat,
    };

    LAB_LOG("init - stream formats: output=%{public}u ch/%{public}u Bpf input=%{public}u ch/%{public}u Bpf rate=%{public}.1f",
            outputFormat.mChannelsPerFrame, outputFormat.mBytesPerFrame,
            inputFormat.mChannelsPerFrame, inputFormat.mBytesPerFrame,
            outputFormat.mSampleRate);

    ivars->outputBytesPerFrame = outputFormat.mBytesPerFrame;
    ivars->outputChannels = outputFormat.mChannelsPerFrame;

    // CoreAudio HAL wraps stream writes at zeroTimestampPeriod, so the ring
    // buffer size must match the period exactly to avoid a wrap mismatch where
    // the driver reads unwritten/silent buffer regions.
    ivars->ringFrames = in_zero_timestamp_period;

    LAB_LOG("init - creating output ring buffer (%{public}u bytes, ringFrames = %{public}u, zeroTimestampPeriod = %{public}u)",
            ivars->ringFrames * outputFormat.mBytesPerFrame,
            ivars->ringFrames, in_zero_timestamp_period);
    OSSharedPtr<IOBufferMemoryDescriptor> buffer;
    uint32_t bufferSize = ivars->ringFrames * outputFormat.mBytesPerFrame;
    kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, bufferSize, 0, buffer.attach());
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - Failed to create output IOBufferMemoryDescriptor (kr = 0x%{public}08x)", kr);
        return false;
    }
    LAB_LOG("init - IOBufferMemoryDescriptor created successfully. Length: %{public}u bytes", bufferSize);

    // Map the ring here, before SetIOOperationHandler ever runs: the RT IO
    // callback must use only cached state, and StartIO must not be the first
    // place the buffer becomes reachable (ADK contract: prepare IO state
    // before super::StartIO).
    LAB_LOG("init - creating memory mapping for output ring");
    kr = buffer->CreateMapping(0, 0, 0, 0, 0, ivars->outputMemoryMap.attach());
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - CreateMapping failed (kr = 0x%{public}08x)", kr);
        return false;
    }
    ivars->ringBase = reinterpret_cast<float*>(
        ivars->outputMemoryMap->GetAddress() + ivars->outputMemoryMap->GetOffset());
    LAB_LOG("init - output ring mapped: address = 0x%{public}llx, length = %{public}llu bytes",
            ivars->outputMemoryMap->GetAddress(), ivars->outputMemoryMap->GetLength());

    LAB_LOG("init - creating output stream object");
    ivars->outputStream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Output, buffer.get());
    if (!ivars->outputStream) {
        LAB_LOG("init - Failed to create output IOUserAudioStream");
        return false;
    }
    LAB_LOG("init - output stream object created successfully");

    LAB_LOG("init - configuring stream formats");
    kr = ivars->outputStream->SetAvailableStreamFormats(
        availableOutputFormats, 2);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - output SetAvailableStreamFormats failed (kr = 0x%{public}08x)", kr);
        return false;
    }
    kr = ivars->outputStream->SetCurrentStreamFormat(&outputFormat);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - output SetCurrentStreamFormat failed (kr = 0x%{public}08x)", kr);
        return false;
    }

    LAB_LOG("init - adding stream to device");
    kr = AddStream(ivars->outputStream.get());
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - AddStream failed (kr = 0x%{public}08x)", kr);
        return false;
    }
    LAB_LOG("init - stream added successfully to device");

    // C4 rig: publish the model's actual input width. Content remains zeroed;
    // it doesn't matter for the C4 question (BeginRead/WriteEnd
    // scheduling relationship) so the ring is left zeroed; only the
    // simulated hardware fill cursor (capturedFrames, advanced in
    // ZtsTimerOccurred_Impl) is load-bearing.
    ivars->inputBytesPerFrame = inputFormat.mBytesPerFrame;
    ivars->inputChannels = inputFormat.mChannelsPerFrame;

    LAB_LOG("init - creating input ring buffer (%{public}u bytes, ringFrames = %{public}u)",
            ivars->ringFrames * inputFormat.mBytesPerFrame,
            ivars->ringFrames);
    OSSharedPtr<IOBufferMemoryDescriptor> inputBuffer;
    uint32_t inputBufferSize = ivars->ringFrames * inputFormat.mBytesPerFrame;
    kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, inputBufferSize, 0, inputBuffer.attach());
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - Failed to create input IOBufferMemoryDescriptor (kr = 0x%{public}08x)", kr);
        return false;
    }

    kr = inputBuffer->CreateMapping(0, 0, 0, 0, 0, ivars->inputMemoryMap.attach());
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - input CreateMapping failed (kr = 0x%{public}08x)", kr);
        return false;
    }
    ivars->inputRingBase = reinterpret_cast<float*>(
        ivars->inputMemoryMap->GetAddress() + ivars->inputMemoryMap->GetOffset());
    LAB_LOG("init - input ring mapped: address = 0x%{public}llx, length = %{public}llu bytes",
            ivars->inputMemoryMap->GetAddress(), ivars->inputMemoryMap->GetLength());

    ivars->inputStream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Input, inputBuffer.get());
    if (!ivars->inputStream) {
        LAB_LOG("init - Failed to create input IOUserAudioStream");
        return false;
    }
    kr = ivars->inputStream->SetAvailableStreamFormats(
        availableInputFormats, 2);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - input SetAvailableStreamFormats failed (kr = 0x%{public}08x)", kr);
        return false;
    }
    kr = ivars->inputStream->SetCurrentStreamFormat(&inputFormat);
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - input SetCurrentStreamFormat failed (kr = 0x%{public}08x)", kr);
        return false;
    }

    kr = AddStream(ivars->inputStream.get());
    if (kr != kIOReturnSuccess) {
        LAB_LOG("init - input AddStream failed (kr = 0x%{public}08x)", kr);
        return false;
    }
    LAB_LOG("init - input stream added successfully to device");

    LAB_LOG("init - configuring controller output stream");
    ivars->controller->ConfigureOutputStream(caps.sampleRate, ivars->outputChannels,
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
                // Late/early callback outside the IO window: count it and do
                // nothing else — driver-side state may be mid-reset.
                ivarsPtr->ioAfterStop.fetch_add(1, std::memory_order_relaxed);
                return kIOReturnSuccess;
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

            // Cached-only IO state (no IOMemoryMap deref on the RT thread).
            if (ivarsPtr->controller && ivarsPtr->ringBase &&
                ivarsPtr->ringFrames != 0) {
                const uint32_t offsetFrames =
                    static_cast<uint32_t>(in_sample_time % ivarsPtr->ringFrames);

                ASFW::Protocols::Audio::AMDTP::HostAudioBufferView outputView {
                    .interleavedFloat32 =
                        &ivarsPtr->ringBase[offsetFrames * ivarsPtr->outputChannels],
                    .firstFrame = in_sample_time,
                    .frameCount = in_io_buffer_frame_size,
                    .frameCapacity = ivarsPtr->ringFrames,
                    .channels = ivarsPtr->outputChannels
                };

                ivarsPtr->controller->SubmitWriteEnd(outputView);

                ivarsPtr->payloadCommittedEndFrame.store(
                    in_sample_time + in_io_buffer_frame_size, std::memory_order_relaxed);
                ivarsPtr->payloadCommittedValid.store(true, std::memory_order_relaxed);
            }
        } else if (in_io_operation == IOUserAudioIOOperationBeginRead) {
            if (!ivarsPtr->ioRunning.load(std::memory_order_relaxed)) {
                ivarsPtr->readAfterStop.fetch_add(1, std::memory_order_relaxed);
                return kIOReturnSuccess;
            }

            // C4 shape instrumentation (RT-safe: counters only), mirroring
            // the WriteEnd block above.
            const uint64_t readCount =
                ivarsPtr->beginReadCount.fetch_add(1, std::memory_order_relaxed);
            if (readCount == 0) {
                ivarsPtr->firstBeginReadSampleTime.store(in_sample_time,
                                                         std::memory_order_relaxed);
                ivarsPtr->firstBeginReadHostTime.store(in_host_time,
                                                       std::memory_order_relaxed);
            }
            ivarsPtr->framesRequested.fetch_add(in_io_buffer_frame_size,
                                                std::memory_order_relaxed);
            if (in_io_buffer_frame_size <
                ivarsPtr->minReadIoFrames.load(std::memory_order_relaxed)) {
                ivarsPtr->minReadIoFrames.store(in_io_buffer_frame_size,
                                                std::memory_order_relaxed);
            }
            if (in_io_buffer_frame_size >
                ivarsPtr->maxReadIoFrames.load(std::memory_order_relaxed)) {
                ivarsPtr->maxReadIoFrames.store(in_io_buffer_frame_size,
                                                std::memory_order_relaxed);
            }
            if (ivarsPtr->expectedReadSampleTimeValid.load(std::memory_order_relaxed) &&
                ivarsPtr->expectedNextReadSampleTime.load(std::memory_order_relaxed) !=
                    in_sample_time) {
                ivarsPtr->readSampleTimeBreaks.fetch_add(1, std::memory_order_relaxed);
            }
            ivarsPtr->expectedNextReadSampleTime.store(
                in_sample_time + in_io_buffer_frame_size, std::memory_order_relaxed);
            ivarsPtr->expectedReadSampleTimeValid.store(true, std::memory_order_relaxed);

            // THE C4 MEASUREMENT: is the span the real HAL just asked for
            // already covered by the simulated hardware capture cursor
            // (advanced independently, on the work queue, by
            // ZtsTimerOccurred_Impl)? A negative margin here is a real,
            // HAL-scheduled capture-starvation event -- not a guess about
            // one.
            const int64_t captured = static_cast<int64_t>(
                ivarsPtr->capturedFrames.load(std::memory_order_relaxed));
            const int64_t requiredEnd =
                static_cast<int64_t>(in_sample_time) +
                static_cast<int64_t>(in_io_buffer_frame_size);
            const int64_t margin = captured - requiredEnd;
            if (margin < 0) {
                ivarsPtr->captureStarvations.fetch_add(1, std::memory_order_relaxed);
            }
            int64_t previousMinMargin =
                ivarsPtr->minCaptureMarginFrames.load(std::memory_order_relaxed);
            while (margin < previousMinMargin &&
                   !ivarsPtr->minCaptureMarginFrames.compare_exchange_weak(
                       previousMinMargin, margin, std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
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
        if (ivars->configurationCoordinator) {
            delete ivars->configurationCoordinator;
            ivars->configurationCoordinator = nullptr;
        }
        if (ivars->configLock) {
            IOLockFree(ivars->configLock);
            ivars->configLock = nullptr;
        }
        ivars->driver.reset();
        ivars->workQueue.reset();
        ivars->outputStream.reset();
        ivars->outputMemoryMap.reset();
        ivars->inputStream.reset();
        ivars->inputMemoryMap.reset();
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

    // C4 rig: this fire IS the simulated hardware capture interrupt --
    // "frames up through sampleTime have now been captured," independent of
    // whether/when the HAL has called BeginRead for them. Real capture
    // hardware behaves the same way (continuous background fill).
    ivars->capturedFrames.store(sampleTime, std::memory_order_relaxed);
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
        ivars->timebase.NsToTicks(NsForPeriodIndex(
            ivars->periodIndex,
            ivars->currentSampleRate.load(std::memory_order_relaxed),
            GetZeroTimestampPeriod()));
    const uint64_t leeway = ivars->timebase.NsToTicks(500000); // 0.5 ms
    ivars->ztsTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, deadline, leeway);
}

kern_return_t VirtualAudioDevice::StartIO(IOUserAudioStartStopFlags in_flags)
{
    LAB_LOG("StartIO - entering. flags = 0x%{public}x", in_flags);
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::StartIOEnter,
                      0, kIOReturnSuccess,
                      ivars != nullptr
                          ? ivars->currentSampleRate.load(std::memory_order_relaxed)
                          : 0,
                      ivars != nullptr
                          ? ivars->currentSampleRate.load(std::memory_order_relaxed)
                          : 0);

    __block kern_return_t kr = kIOReturnSuccess;
    ivars->workQueue->DispatchSync(^(){
        // ADK contract: do all driver-side start work first and call
        // super::StartIO last (it flips the device's IO state). The ring
        // mapping was created in init(), so nothing to map here.
        if (ivars->ringBase == nullptr) {
            LAB_LOG("StartIO - output ring is not mapped");
            kr = kIOReturnInternalError;
            return;
        }
        if (ivars->inputRingBase == nullptr) {
            LAB_LOG("StartIO - input ring is not mapped");
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
        ivars->payloadCommittedEndFrame.store(0, std::memory_order_relaxed);
        ivars->payloadCommittedValid.store(false, std::memory_order_relaxed);
        ivars->otherIoOperations.store(0, std::memory_order_relaxed);
        ivars->capturedFrames.store(0, std::memory_order_relaxed);
        ivars->beginReadCount.store(0, std::memory_order_relaxed);
        ivars->framesRequested.store(0, std::memory_order_relaxed);
        ivars->minReadIoFrames.store(0xFFFFFFFFu, std::memory_order_relaxed);
        ivars->maxReadIoFrames.store(0, std::memory_order_relaxed);
        ivars->readSampleTimeBreaks.store(0, std::memory_order_relaxed);
        ivars->expectedReadSampleTimeValid.store(false, std::memory_order_relaxed);
        ivars->firstBeginReadSampleTime.store(0, std::memory_order_relaxed);
        ivars->firstBeginReadHostTime.store(0, std::memory_order_relaxed);
        ivars->captureStarvations.store(0, std::memory_order_relaxed);
        ivars->minCaptureMarginFrames.store(INT64_MAX, std::memory_order_relaxed);
        ivars->readAfterStop.store(0, std::memory_order_relaxed);

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

        // Open the RT gate before super so the first WriteEnds are accepted,
        // then let super flip the device IO state.
        ivars->ioRunning.store(true, std::memory_order_relaxed);

        LAB_LOG("StartIO - calling super::StartIO");
        kr = super::StartIO(in_flags);
        if (kr != kIOReturnSuccess) {
            LAB_LOG("StartIO - super::StartIO failed (kr = 0x%{public}08x)", kr);
            ivars->ioRunning.store(false, std::memory_order_relaxed);
            return;
        }
        LAB_LOG("StartIO - super::StartIO succeeded");

        // Arm the wrap timer only once IO actually started; the t=0 anchor
        // above already seeds the clock chain.
        ivars->periodIndex = 1;
    const uint64_t deadline =
        ivars->startHostTime +
        ivars->timebase.NsToTicks(NsForPeriodIndex(
            1,
            ivars->currentSampleRate.load(std::memory_order_relaxed),
            GetZeroTimestampPeriod()));
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
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::StartIOReturn,
                      0, kr,
                      ivars != nullptr
                          ? ivars->currentSampleRate.load(std::memory_order_relaxed)
                          : 0,
                      ivars != nullptr
                          ? ivars->currentSampleRate.load(std::memory_order_relaxed)
                          : 0);
    return kr;
}

kern_return_t VirtualAudioDevice::StopIO(IOUserAudioStartStopFlags in_flags)
{
    LAB_LOG("StopIO - entering. flags = 0x%{public}x", in_flags);
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::StopIOEnter,
                      0, kIOReturnSuccess,
                      ivars != nullptr
                          ? ivars->currentSampleRate.load(std::memory_order_relaxed)
                          : 0,
                      ivars != nullptr
                          ? ivars->currentSampleRate.load(std::memory_order_relaxed)
                          : 0);

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

        // The ring mapping is deliberately NOT torn down here: a late RT
        // WriteEnd may still be executing against the cached base pointer.
        // The mapping lives until free() (the ioRunning gate stops new work).

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

        // C4 answer. min_margin is capturedFrames - (sample_time + size) at
        // its worst observed point: negative means the real HAL called
        // BeginRead for a span the simulated hardware cursor hadn't reached
        // yet (a genuine capture-starvation event under real AudioDriverKit
        // scheduling). read_vs_write_host_delta is the actual measured
        // offset between the first BeginRead and first WriteEnd host times —
        // this is the number that settles whether the real HAL schedules
        // the two operations coupled or independently (compare against
        // in_out_safety_delta_frames, the naive prediction from the
        // declared SafetyOffset values alone).
        const uint64_t firstReadHost =
            ivars->firstBeginReadHostTime.load(std::memory_order_relaxed);
        const int64_t readVsWriteHostDeltaTicks =
            (firstReadHost != 0 && firstHost != 0)
                ? (int64_t)(firstReadHost) - (int64_t)(firstHost)
                : 0;
        LAB_LOG("dump beginread: count=%{public}llu frames_req=%{public}llu min=%{public}u max=%{public}u "
                "sample_breaks=%{public}llu first_sample=%{public}llu first_host_delta=%{public}lld "
                "read_after_stop=%{public}llu starvations=%{public}llu min_margin_frames=%{public}lld "
                "read_vs_write_host_delta_ticks=%{public}lld "
                "declared_out_safety=%{public}u declared_in_safety=%{public}u",
                ivars->beginReadCount.load(std::memory_order_relaxed),
                ivars->framesRequested.load(std::memory_order_relaxed),
                ivars->minReadIoFrames.load(std::memory_order_relaxed),
                ivars->maxReadIoFrames.load(std::memory_order_relaxed),
                ivars->readSampleTimeBreaks.load(std::memory_order_relaxed),
                ivars->firstBeginReadSampleTime.load(std::memory_order_relaxed),
                (firstReadHost != 0)
                    ? (int64_t)(firstReadHost - ivars->startHostTime)
                    : (int64_t)0,
                ivars->readAfterStop.load(std::memory_order_relaxed),
                ivars->captureStarvations.load(std::memory_order_relaxed),
                (long long)ivars->minCaptureMarginFrames.load(std::memory_order_relaxed),
                readVsWriteHostDeltaTicks,
                kLabOutputSafetyOffsetFrames,
                kLabInputSafetyOffsetFrames);

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
                    "without_packet=%{public}llu outside_packet=%{public}llu "
                    "raced_reuse=%{public}llu",
                    payload.framesVisited.load(std::memory_order_relaxed),
                    payload.framesWritten.load(std::memory_order_relaxed),
                    payload.framesWithoutPacket.load(std::memory_order_relaxed),
                    payload.framesOutsidePacket.load(std::memory_order_relaxed),
                    payload.framesRacedReuse.load(std::memory_order_relaxed));
        }
    });

    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::StopIOReturn,
                      0, kr,
                      ivars != nullptr
                          ? ivars->currentSampleRate.load(std::memory_order_relaxed)
                          : 0,
                      ivars != nullptr
                          ? ivars->currentSampleRate.load(std::memory_order_relaxed)
                          : 0);
    return kr;
}

kern_return_t VirtualAudioDevice::CopyPacketDump(uint32_t in_count,
                                                 uint64_t in_anchor,
                                                 OSData** out_data)
{
    if (out_data == nullptr) {
        return kIOReturnBadArgument;
    }
    *out_data = nullptr;
    if (ivars == nullptr || ivars->controller == nullptr ||
        ivars->workQueue.get() == nullptr) {
        return kIOReturnNotReady;
    }

    const uint32_t count =
        (in_count == 0) ? ASFW::Lab::kPacketDumpDefaultRecords
        : (in_count > ASFW::Lab::kPacketDumpMaxRecords)
            ? ASFW::Lab::kPacketDumpMaxRecords
            : in_count;
    const size_t capacity = ASFW::Lab::PacketDumpBlobSize(count);

    uint8_t* buffer = IONewZero(uint8_t, capacity);
    if (buffer == nullptr) {
        return kIOReturnNoMemory;
    }

    // The copy runs on the work queue: serialized with the packet pump, so
    // slot metadata is consistent without touching the RT path at all.
    __block size_t blobSize = 0;
    auto ivarsPtr = ivars;
    ivars->workQueue->DispatchSync(^(){
        ASFW::Lab::PacketDumpContext context{};
        context.hostTimeTicks = mach_absolute_time();
        context.periodIndex = ivarsPtr->periodIndex;
        context.ztsPeriodFrames = GetZeroTimestampPeriod();
        context.ioRunning =
            ivarsPtr->ioRunning.load(std::memory_order_relaxed);
        context.exposedFrames = ivarsPtr->exposedFrames;
        context.nextPacketIndex = ivarsPtr->nextPacketIndex;
        context.prepareFailures = ivarsPtr->prepareFailures;
        context.writeEndCount =
            ivarsPtr->writeEndCount.load(std::memory_order_relaxed);
        context.expectedNextSampleTime =
            ivarsPtr->expectedNextSampleTime.load(std::memory_order_relaxed);
        context.expectedSampleTimeValid =
            ivarsPtr->expectedSampleTimeValid.load(std::memory_order_relaxed);
        context.payloadCommittedEndFrame =
            ivarsPtr->payloadCommittedEndFrame.load(std::memory_order_relaxed);
        context.payloadCommittedValid =
            ivarsPtr->payloadCommittedValid.load(std::memory_order_relaxed);

        blobSize = ASFW::Lab::BuildPacketDumpBlob(
            ivarsPtr->controller->FakeSlotProvider(),
            ivarsPtr->controller->Timeline(),
            ivarsPtr->controller->PayloadCounters(), context, count,
            in_anchor, buffer, capacity);
    });

    kern_return_t result = kIOReturnInternalError;
    if (blobSize != 0) {
        *out_data = OSData::withBytes(buffer, static_cast<uint32_t>(blobSize));
        result = (*out_data != nullptr) ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    IODelete(buffer, uint8_t, capacity);
    return result;
}

void VirtualAudioDevice::SetLabSlot(uint32_t in_slot)
{
    if (ivars == nullptr) {
        return;
    }
    ivars->labSlot = in_slot;
    ADK_CONFIG_LOG("slot=%{public}u object=0x%{public}x phase=SetLabSlot",
                   ivars->labSlot, static_cast<uint32_t>(GetObjectID()));
}

static bool IsExperimentSampleRate(uint32_t sampleRate) noexcept
{
    return sampleRate == ASFW::Lab::kADKConfigRateA ||
           sampleRate == ASFW::Lab::kADKConfigRateB;
}

static IOUserAudioStreamBasicDescription MakeFloat32Format(
    uint32_t sampleRate, uint32_t channels) noexcept
{
    const uint32_t bytesPerFrame = channels * kBytesPerSample;
    return IOUserAudioStreamBasicDescription{
        .mSampleRate = static_cast<double>(sampleRate),
        .mFormatID = IOUserAudioFormatID::LinearPCM,
        .mFormatFlags = static_cast<IOUserAudioFormatFlags>(
            IOUserAudioFormatFlags::FormatFlagIsFloat |
            IOUserAudioFormatFlags::FormatFlagsNativeEndian),
        .mBytesPerPacket = bytesPerFrame,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = bytesPerFrame,
        .mChannelsPerFrame = channels,
        .mBitsPerChannel = 32,
    };
}

static kern_return_t ApplyPreferredChannelLayouts(
    VirtualAudioDevice* device, const LabHALDeviceShape& shape) noexcept
{
    IOUserAudioChannelLabel outputLayout[kMaxLabChannels] = {};
    IOUserAudioChannelLabel inputLayout[kMaxLabChannels] = {};
    for (uint32_t channel = 0; channel < shape.outputChannels; ++channel) {
        outputLayout[channel] = static_cast<IOUserAudioChannelLabel>(
            static_cast<uint32_t>(IOUserAudioChannelLabel::Discrete_0) + channel);
    }
    for (uint32_t channel = 0; channel < shape.inputChannels; ++channel) {
        inputLayout[channel] = static_cast<IOUserAudioChannelLabel>(
            static_cast<uint32_t>(IOUserAudioChannelLabel::Discrete_0) + channel);
    }

    kern_return_t kr = device->SetPreferredOutputChannelLayout(
        outputLayout, shape.outputChannels);
    if (kr != kIOReturnSuccess) {
        return kr;
    }
    return device->SetPreferredInputChannelLayout(inputLayout, shape.inputChannels);
}

static kern_return_t ApplyExperimentConfiguration(
    VirtualAudioDevice* device,
    VirtualAudioDevice_IVars* ivars,
    const LabDeviceConfiguration& configuration,
    const LabHALDeviceShape& shape,
    uint64_t action) noexcept
{
    if (device == nullptr || ivars == nullptr ||
        !IsExperimentSampleRate(configuration.sampleRate) ||
        shape.inputChannels == 0 || shape.outputChannels == 0 ||
        shape.inputChannels > ivars->allocatedShape.inputChannels ||
        shape.outputChannels > ivars->allocatedShape.outputChannels) {
        return kIOReturnBadArgument;
    }

    const uint32_t deviceRateBefore =
        static_cast<uint32_t>(device->GetSampleRate());
    kern_return_t kr = device->SetSampleRate(
        static_cast<double>(configuration.sampleRate));
    const uint32_t deviceRateAfter =
        static_cast<uint32_t>(device->GetSampleRate());
    RecordConfigEvent(device, ivars,
                      ASFW::Lab::ADKConfigPhase::DeviceRateMutation,
                      action, kr, deviceRateBefore, deviceRateAfter);
    if (kr != kIOReturnSuccess) {
        return kr;
    }

    // SetSampleRate updates the clock device, but it does not select a
    // matching format on either stream. AudioDriverKit requires this explicit
    // propagation so the HAL observes a coherent device-rate/stream-rate
    // transition (IOUserAudioStream::DeviceSampleRateChanged).
    if (deviceRateBefore != configuration.sampleRate) {
        if (ivars->outputStream) {
            kr = ivars->outputStream->DeviceSampleRateChanged(
                static_cast<double>(configuration.sampleRate));
            ADK_CONFIG_LOG("slot=%{public}u phase=OutputDeviceSampleRateChanged rate=%{public}u result=0x%{public}08x",
                           ivars->labSlot, configuration.sampleRate,
                           static_cast<uint32_t>(kr));
            if (kr != kIOReturnSuccess) {
                return kr;
            }
        }
        if (ivars->inputStream) {
            kr = ivars->inputStream->DeviceSampleRateChanged(
                static_cast<double>(configuration.sampleRate));
            ADK_CONFIG_LOG("slot=%{public}u phase=InputDeviceSampleRateChanged rate=%{public}u result=0x%{public}08x",
                           ivars->labSlot, configuration.sampleRate,
                           static_cast<uint32_t>(kr));
            if (kr != kIOReturnSuccess) {
                return kr;
            }
        }
    }

    // This experiment only runs while I/O is stopped. The initial descriptors
    // are allocated for each profile's largest geometry and intentionally stay
    // mapped for the device lifetime, so a late RT callback cannot observe a
    // freed mapping. Buffer replacement belongs to the later streaming phase.
    kr = ApplyPreferredChannelLayouts(device, shape);
    if (kr != kIOReturnSuccess) {
        return kr;
    }

    const auto outputFormat = MakeFloat32Format(configuration.sampleRate,
                                                 shape.outputChannels);
    auto alternateOutputFormat = outputFormat;
    alternateOutputFormat.mSampleRate = static_cast<double>(
        configuration.sampleRate == kInitialSampleRate
            ? kAlternateSampleRate : kInitialSampleRate);
    const IOUserAudioStreamBasicDescription outputFormats[2] = {
        alternateOutputFormat, outputFormat,
    };

    const auto inputFormat = MakeFloat32Format(configuration.sampleRate,
                                                shape.inputChannels);
    auto alternateInputFormat = inputFormat;
    alternateInputFormat.mSampleRate = alternateOutputFormat.mSampleRate;
    const IOUserAudioStreamBasicDescription inputFormats[2] = {
        alternateInputFormat, inputFormat,
    };

    if (ivars->outputStream) {
        const auto before = ivars->outputStream->GetCurrentStreamFormat();
        kr = ivars->outputStream->SetAvailableStreamFormats(outputFormats, 2);
        if (kr == kIOReturnSuccess) {
            kr = ivars->outputStream->SetCurrentStreamFormat(&outputFormat);
        }
        const auto after = ivars->outputStream->GetCurrentStreamFormat();
        RecordConfigEvent(device, ivars,
                          ASFW::Lab::ADKConfigPhase::OutputStreamMutation,
                          action, kr,
                          static_cast<uint32_t>(before.mSampleRate),
                          static_cast<uint32_t>(after.mSampleRate),
                          before.mChannelsPerFrame,
                          after.mChannelsPerFrame);
        if (kr != kIOReturnSuccess) {
            return kr;
        }
    }

    if (ivars->inputStream) {
        const auto before = ivars->inputStream->GetCurrentStreamFormat();
        kr = ivars->inputStream->SetAvailableStreamFormats(inputFormats, 2);
        if (kr == kIOReturnSuccess) {
            kr = ivars->inputStream->SetCurrentStreamFormat(&inputFormat);
        }
        const auto after = ivars->inputStream->GetCurrentStreamFormat();
        RecordConfigEvent(device, ivars,
                          ASFW::Lab::ADKConfigPhase::InputStreamMutation,
                          action, kr,
                          static_cast<uint32_t>(before.mSampleRate),
                          static_cast<uint32_t>(after.mSampleRate),
                          before.mChannelsPerFrame,
                          after.mChannelsPerFrame);
        if (kr != kIOReturnSuccess) {
            return kr;
        }
    }

    ivars->outputBytesPerFrame = outputFormat.mBytesPerFrame;
    ivars->outputChannels = outputFormat.mChannelsPerFrame;
    ivars->inputBytesPerFrame = inputFormat.mBytesPerFrame;
    ivars->inputChannels = inputFormat.mChannelsPerFrame;
    ivars->currentSampleRate.store(configuration.sampleRate,
                                   std::memory_order_relaxed);
    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDevice::RequestSampleRateChange(uint32_t in_sample_rate)
{
    if (ivars == nullptr || ivars->configLock == nullptr ||
        ivars->configurationCoordinator == nullptr) {
        return kIOReturnNotReady;
    }

    LabDeviceConfiguration current{};
    IOLockLock(ivars->configLock);
    current = ivars->currentConfiguration;
    IOLockUnlock(ivars->configLock);
    return RequestConfigurationChange(in_sample_rate, current.opticalInput,
                                      current.opticalOutput);
}

kern_return_t VirtualAudioDevice::RequestConfigurationChange(
    uint32_t in_sample_rate, uint32_t in_optical_input,
    uint32_t in_optical_output)
{
    if (ivars == nullptr || ivars->configLock == nullptr ||
        ivars->deviceDefinition == nullptr ||
        ivars->configurationCoordinator == nullptr) {
        return kIOReturnNotReady;
    }

    const LabDeviceConfiguration requested{
        .sampleRate = in_sample_rate,
        .opticalInput = in_optical_input,
        .opticalOutput = in_optical_output,
    };
    ASFW::Device::DeviceConfiguration requestedModel{};
    LabHALDeviceShape requestedShape{};
    const bool valid = DecodeLabConfiguration(requested, requestedModel) &&
        ResolveLabHALDeviceShape(ivars->deviceDefinition, requested,
                                 requestedShape);

    LabDeviceConfiguration current{};
    LabHALDeviceShape currentShape{};
    if (ivars->ioRunning.load(std::memory_order_relaxed)) {
        return kIOReturnBusy;
    }

    IOLockLock(ivars->configLock);
    current = ivars->currentConfiguration;
    currentShape = ivars->currentShape;
    IOLockUnlock(ivars->configLock);

    if (!valid || requestedShape.inputChannels > ivars->allocatedShape.inputChannels ||
        requestedShape.outputChannels > ivars->allocatedShape.outputChannels) {
        ADK_CONFIG_LOG("slot=%{public}u object=0x%{public}x phase=RequestRejected rate=%{public}u optical_in=%{public}u optical_out=%{public}u result=0x%{public}08x",
                       ivars->labSlot, static_cast<uint32_t>(GetObjectID()),
                       in_sample_rate, in_optical_input, in_optical_output,
                       static_cast<uint32_t>(kIOReturnBadArgument));
        RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::RequestRejected,
                          0, kIOReturnBadArgument, current.sampleRate,
                          requested.sampleRate, currentShape.outputChannels,
                          requestedShape.outputChannels);
        return kIOReturnBadArgument;
    }

    ASFW::Configuration::TransitionResult transition{};
    kern_return_t coordinatorResult = kIOReturnSuccess;
    IOLockLock(ivars->configLock);
    const bool accepted = DispatchCoordinatorLocked(
        ivars, ASFW::Configuration::ConfigurationEvent{
                   ASFW::Configuration::ControlIntent{
                       .endpointId = 1,
                       .routeGeneration = 1,
                       .requested = requestedModel,
                   }},
        transition, coordinatorResult);
    const uint64_t action = ivars->pendingAction;
    IOLockUnlock(ivars->configLock);
    if (!accepted) {
        RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::CoordinatorRejected,
                          0, coordinatorResult, current.sampleRate,
                          requested.sampleRate, currentShape.outputChannels,
                          requestedShape.outputChannels);
        return coordinatorResult;
    }

    if (transition.disposition == ASFW::Configuration::TransitionDisposition::NoOp) {
        return kIOReturnSuccess;
    }
    if (transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::ResolveCandidateEffect>(
            transition.effects[0])) {
        return kIOReturnError;
    }

    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::HostRequest,
                      action, kIOReturnSuccess, current.sampleRate,
                      requested.sampleRate, currentShape.outputChannels,
                      requestedShape.outputChannels);
    IOLockLock(ivars->configLock);
    const bool candidateAccepted = DispatchCoordinatorLocked(
        ivars, ASFW::Configuration::ConfigurationEvent{
                   ASFW::Configuration::CandidateAccepted{
                       .identity = std::get<ASFW::Configuration::ResolveCandidateEffect>(
                           transition.effects[0]).identity,
                       .candidate = requestedModel,
                   }},
        transition, coordinatorResult);
    IOLockUnlock(ivars->configLock);
    if (!candidateAccepted) {
        RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::CandidateRejected,
                          action, coordinatorResult, current.sampleRate,
                          requested.sampleRate, currentShape.outputChannels,
                          requestedShape.outputChannels);
        return coordinatorResult;
    }
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::CandidateAccepted,
                      action, kIOReturnSuccess, current.sampleRate,
                      requested.sampleRate, currentShape.outputChannels,
                      requestedShape.outputChannels);
    if (transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::RequestADKWindowEffect>(
            transition.effects[0])) {
        return kIOReturnError;
    }

    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::RequestCalled,
                      action, kIOReturnSuccess, current.sampleRate,
                      requested.sampleRate, currentShape.outputChannels,
                      requestedShape.outputChannels);
    const kern_return_t requestResult =
        RequestDeviceConfigurationChange(action, nullptr);
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::RequestReturned,
                      action, requestResult, current.sampleRate,
                      requested.sampleRate, currentShape.outputChannels,
                      requestedShape.outputChannels);
    if (requestResult != kIOReturnSuccess) {
        IOLockLock(ivars->configLock);
        (void)DispatchCoordinatorLocked(
            ivars, ASFW::Configuration::ConfigurationEvent{
                       ASFW::Configuration::ADKWindowRejected{
                           .identity = std::get<ASFW::Configuration::RequestADKWindowEffect>(
                               transition.effects[0]).identity,
                       }},
            transition, coordinatorResult);
        IOLockUnlock(ivars->configLock);
    }
    return requestResult;
}

kern_return_t VirtualAudioDevice::NotifyHardwareObserved(
    uint32_t in_sample_rate, uint32_t in_optical_input,
    uint32_t in_optical_output)
{
    if (ivars == nullptr || ivars->configLock == nullptr ||
        ivars->deviceDefinition == nullptr ||
        ivars->configurationCoordinator == nullptr) {
        return kIOReturnNotReady;
    }
    if (ivars->ioRunning.load(std::memory_order_relaxed)) {
        return kIOReturnBusy;
    }

    const LabDeviceConfiguration observed{
        .sampleRate = in_sample_rate,
        .opticalInput = in_optical_input,
        .opticalOutput = in_optical_output,
    };
    ASFW::Device::DeviceConfiguration observedModel{};
    LabHALDeviceShape observedShape{};
    const bool valid = DecodeLabConfiguration(observed, observedModel) &&
        ResolveLabHALDeviceShape(ivars->deviceDefinition, observed, observedShape);

    LabDeviceConfiguration current{};
    LabHALDeviceShape currentShape{};
    IOLockLock(ivars->configLock);
    current = ivars->currentConfiguration;
    currentShape = ivars->currentShape;
    IOLockUnlock(ivars->configLock);
    if (!valid || observedShape.inputChannels > ivars->allocatedShape.inputChannels ||
        observedShape.outputChannels > ivars->allocatedShape.outputChannels) {
        RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::CoordinatorRejected,
                          0, kIOReturnBadArgument, current.sampleRate,
                          observed.sampleRate, currentShape.outputChannels,
                          observedShape.outputChannels);
        return kIOReturnBadArgument;
    }

    ASFW::Configuration::TransitionResult transition{};
    kern_return_t coordinatorResult = kIOReturnSuccess;
    IOLockLock(ivars->configLock);
    const bool accepted = DispatchCoordinatorLocked(
        ivars, ASFW::Configuration::ConfigurationEvent{
                   ASFW::Configuration::HardwareObserved{
                       .endpointId = 1,
                       .routeGeneration = 1,
                       .confirmed = ASFW::Configuration::ConfirmedHardwareConfiguration{
                           .configuration = observedModel,
                       },
                   }},
        transition, coordinatorResult);
    const uint64_t action = ivars->pendingAction;
    IOLockUnlock(ivars->configLock);
    if (!accepted) {
        RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::CoordinatorRejected,
                          0, coordinatorResult, current.sampleRate,
                          observed.sampleRate, currentShape.outputChannels,
                          observedShape.outputChannels);
        return coordinatorResult;
    }
    if (transition.disposition == ASFW::Configuration::TransitionDisposition::NoOp) {
        return kIOReturnSuccess;
    }
    if (transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::RequestADKWindowEffect>(
            transition.effects[0])) {
        return kIOReturnError;
    }

    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::HardwareObserved,
                      action, kIOReturnSuccess, current.sampleRate,
                      observed.sampleRate, currentShape.outputChannels,
                      observedShape.outputChannels);
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::RequestCalled,
                      action, kIOReturnSuccess, current.sampleRate,
                      observed.sampleRate, currentShape.outputChannels,
                      observedShape.outputChannels);
    const kern_return_t requestResult =
        RequestDeviceConfigurationChange(action, nullptr);
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::RequestReturned,
                      action, requestResult, current.sampleRate,
                      observed.sampleRate, currentShape.outputChannels,
                      observedShape.outputChannels);
    if (requestResult != kIOReturnSuccess) {
        IOLockLock(ivars->configLock);
        (void)DispatchCoordinatorLocked(
            ivars, ASFW::Configuration::ConfigurationEvent{
                       ASFW::Configuration::ADKWindowRejected{
                           .identity = std::get<ASFW::Configuration::RequestADKWindowEffect>(
                               transition.effects[0]).identity,
                       }},
            transition, coordinatorResult);
        IOLockUnlock(ivars->configLock);
    }
    return requestResult;
}

kern_return_t VirtualAudioDevice::PerformDeviceConfigurationChange(
    uint64_t change_action, OSObject* in_change_info)
{
    if (ivars == nullptr || ivars->configLock == nullptr ||
        ivars->configurationCoordinator == nullptr) {
        return super::PerformDeviceConfigurationChange(change_action, in_change_info);
    }

    LabDeviceConfiguration current{};
    LabDeviceConfiguration requested{};
    LabHALDeviceShape currentShape{};
    LabHALDeviceShape requestedShape{};
    bool ownsAction = false;
    IOLockLock(ivars->configLock);
    ownsAction = ivars->configurationPending &&
                 ivars->pendingAction == change_action;
    current = ivars->currentConfiguration;
    requested = ivars->pendingConfiguration;
    currentShape = ivars->currentShape;
    requestedShape = ivars->pendingShape;
    IOLockUnlock(ivars->configLock);

    if (!ownsAction) {
        const kern_return_t kr =
            super::PerformDeviceConfigurationChange(change_action, in_change_info);
        RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::PerformReturn,
                          change_action, kr, current.sampleRate,
                          requested.sampleRate, currentShape.outputChannels,
                          requestedShape.outputChannels);
        return kr;
    }

    ADK_CONFIG_LOG("slot=%{public}u object=0x%{public}x phase=PerformEnter action=%{public}llu shape=%{public}u/%{public}u->%{public}u/%{public}u",
                   ivars->labSlot, static_cast<uint32_t>(GetObjectID()), change_action,
                   currentShape.inputChannels, currentShape.outputChannels,
                   requestedShape.inputChannels, requestedShape.outputChannels);
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::PerformEnter,
                      change_action, kIOReturnSuccess, current.sampleRate,
                      requested.sampleRate, currentShape.outputChannels,
                      requestedShape.outputChannels);

    ASFW::Configuration::TransitionResult transition{};
    kern_return_t coordinatorResult = kIOReturnSuccess;
    IOLockLock(ivars->configLock);
    const bool granted = DispatchCoordinatorLocked(
        ivars, ASFW::Configuration::ConfigurationEvent{
                   ASFW::Configuration::ADKPerformGranted{
                       .identity = ASFW::Configuration::ConfigurationIdentity{
                           .endpointId = 1,
                           .token = change_action,
                           .routeGeneration = 1,
                       },
                   }},
        transition, coordinatorResult);
    IOLockUnlock(ivars->configLock);
    if (!granted || transition.effects.size() != 1) {
        const kern_return_t superResult =
            super::PerformDeviceConfigurationChange(change_action, in_change_info);
        return granted ? superResult : coordinatorResult;
    }

    std::optional<ASFW::Configuration::ProjectADKEffect> project{};
    kern_return_t mutation = kIOReturnSuccess;
    if (std::holds_alternative<ASFW::Configuration::ApplyHardwareEffect>(
            transition.effects[0])) {
        const auto apply = std::get<ASFW::Configuration::ApplyHardwareEffect>(
            transition.effects[0]);
        RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::HardwareApply,
                          change_action, kIOReturnSuccess, current.sampleRate,
                          requested.sampleRate, currentShape.outputChannels,
                          requestedShape.outputChannels);
        IOLockLock(ivars->configLock);
        const auto outcome = ivars->configurationCoordinator->CompleteHardware(apply);
        const auto outcomeKind = ASFW::Configuration::HardwareKind(outcome);
        const bool completed = DispatchCoordinatorLocked(
            ivars, ASFW::Configuration::ConfigurationEvent{
                       ASFW::Configuration::HardwareCompleted{
                           .identity = apply.transition.identity,
                           .outcome = outcome,
                       }},
            transition, coordinatorResult);
        IOLockUnlock(ivars->configLock);
        const auto outcomePhase = outcomeKind == ASFW::Configuration::HardwareOutcomeKind::Unchanged
            ? ASFW::Lab::ADKConfigPhase::HardwareUnchanged
            : outcomeKind == ASFW::Configuration::HardwareOutcomeKind::Unknown
                ? ASFW::Lab::ADKConfigPhase::HardwareUnknown
                : ASFW::Lab::ADKConfigPhase::HardwareCompleted;
        RecordConfigEvent(this, ivars, outcomePhase, change_action,
                          completed ? kIOReturnSuccess : coordinatorResult,
                          current.sampleRate, requested.sampleRate,
                          currentShape.outputChannels, requestedShape.outputChannels);
        if (completed && transition.effects.size() == 1 &&
            std::holds_alternative<ASFW::Configuration::ProjectADKEffect>(
                transition.effects[0])) {
            project = std::get<ASFW::Configuration::ProjectADKEffect>(
                transition.effects[0]);
        } else {
            const kern_return_t superResult =
                super::PerformDeviceConfigurationChange(change_action, in_change_info);
            RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::PerformSuper,
                              change_action, superResult, current.sampleRate,
                              requested.sampleRate, currentShape.outputChannels,
                              requestedShape.outputChannels);
            const kern_return_t result = completed ? superResult : coordinatorResult;
            RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::PerformReturn,
                              change_action, result, current.sampleRate,
                              ivars->currentSampleRate.load(std::memory_order_relaxed),
                              currentShape.outputChannels, requestedShape.outputChannels);
            return result;
        }
    } else if (std::holds_alternative<ASFW::Configuration::ProjectADKEffect>(
                   transition.effects[0])) {
        // A hardware observation is already confirmed.  Its ADK perform
        // window projects that state; it must not re-enter the hardware port.
        project = std::get<ASFW::Configuration::ProjectADKEffect>(
            transition.effects[0]);
    } else {
        const kern_return_t superResult =
            super::PerformDeviceConfigurationChange(change_action, in_change_info);
        RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::PerformSuper,
                          change_action, superResult, current.sampleRate,
                          requested.sampleRate, currentShape.outputChannels,
                          requestedShape.outputChannels);
        RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::PerformReturn,
                          change_action, kIOReturnError, current.sampleRate,
                          ivars->currentSampleRate.load(std::memory_order_relaxed),
                          currentShape.outputChannels, requestedShape.outputChannels);
        return kIOReturnError;
    }

    const auto& projection = *project;
    const LabDeviceConfiguration confirmed =
        EncodeLabConfiguration(projection.plan.confirmed.configuration);
    LabHALDeviceShape confirmedShape{};
    const bool resolved = ResolveLabHALDeviceShape(ivars->deviceDefinition,
                                                    confirmed, confirmedShape);
    mutation = resolved ? ApplyExperimentConfiguration(this, ivars, confirmed,
                                                        confirmedShape, change_action)
                        : kIOReturnBadArgument;
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::PerformMutation,
                      change_action, mutation, current.sampleRate,
                      confirmed.sampleRate, currentShape.outputChannels,
                      confirmedShape.outputChannels);
    const kern_return_t superResult =
        super::PerformDeviceConfigurationChange(change_action, in_change_info);
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::PerformSuper,
                      change_action, superResult, current.sampleRate,
                      confirmed.sampleRate, currentShape.outputChannels,
                      confirmedShape.outputChannels);

    IOLockLock(ivars->configLock);
    const bool projected = DispatchCoordinatorLocked(
        ivars, ASFW::Configuration::ConfigurationEvent{
                   ASFW::Configuration::ProjectionFinished{
                       .identity = projection.plan.identity,
                       .customProjectionSucceeded = mutation == kIOReturnSuccess,
                       .superclassSucceeded = superResult == kIOReturnSuccess,
                   }},
        transition, coordinatorResult);
    IOLockUnlock(ivars->configLock);
    const kern_return_t result = mutation != kIOReturnSuccess ? mutation
        : superResult != kIOReturnSuccess ? superResult
        : projected ? kIOReturnSuccess : coordinatorResult;
    RecordConfigEvent(this, ivars,
                      result == kIOReturnSuccess
                          ? ASFW::Lab::ADKConfigPhase::ProjectionCommitted
                          : ASFW::Lab::ADKConfigPhase::CoordinatorRejected,
                      change_action, result, current.sampleRate,
                      ivars->currentSampleRate.load(std::memory_order_relaxed),
                      currentShape.outputChannels, confirmedShape.outputChannels);
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::PerformReturn,
                      change_action, result, current.sampleRate,
                      ivars->currentSampleRate.load(std::memory_order_relaxed),
                      currentShape.outputChannels, confirmedShape.outputChannels);
    return result;
}

kern_return_t VirtualAudioDevice::AbortDeviceConfigurationChange(
    uint64_t change_action, OSObject* in_change_info)
{
    if (ivars == nullptr || ivars->configLock == nullptr) {
        return super::AbortDeviceConfigurationChange(change_action, in_change_info);
    }

    LabDeviceConfiguration current{};
    LabDeviceConfiguration requested{};
    LabHALDeviceShape currentShape{};
    LabHALDeviceShape requestedShape{};
    bool ownsAction = false;
    IOLockLock(ivars->configLock);
    ownsAction = ivars->configurationPending &&
                 ivars->pendingAction == change_action;
    current = ivars->currentConfiguration;
    requested = ivars->pendingConfiguration;
    currentShape = ivars->currentShape;
    requestedShape = ivars->pendingShape;
    IOLockUnlock(ivars->configLock);
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::AbortEnter,
                      change_action, kIOReturnSuccess, current.sampleRate,
                      requested.sampleRate, currentShape.outputChannels,
                      requestedShape.outputChannels);
    const kern_return_t superResult =
        super::AbortDeviceConfigurationChange(change_action, in_change_info);
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::AbortSuper,
                      change_action, superResult, current.sampleRate,
                      requested.sampleRate, currentShape.outputChannels,
                      requestedShape.outputChannels);

    if (ownsAction && ivars->configurationCoordinator != nullptr) {
        ASFW::Configuration::TransitionResult transition{};
        kern_return_t coordinatorResult = kIOReturnSuccess;
        IOLockLock(ivars->configLock);
        (void)DispatchCoordinatorLocked(
            ivars, ASFW::Configuration::ConfigurationEvent{
                       ASFW::Configuration::ADKAborted{
                           .identity = ASFW::Configuration::ConfigurationIdentity{
                               .endpointId = 1,
                               .token = change_action,
                               .routeGeneration = 1,
                           },
                       }},
            transition, coordinatorResult);
        IOLockUnlock(ivars->configLock);
    }

    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::AbortReturn,
                      change_action, superResult, current.sampleRate,
                      current.sampleRate, currentShape.outputChannels,
                      currentShape.outputChannels);
    return superResult;
}

kern_return_t VirtualAudioDevice::HandleChangeSampleRate(double in_sample_rate)
{
    if (ivars == nullptr || ivars->configLock == nullptr ||
        ivars->deviceDefinition == nullptr ||
        ivars->configurationCoordinator == nullptr) {
        return kIOReturnNotReady;
    }

    LabDeviceConfiguration requested{};
    LabHALDeviceShape requestedShape{};
    LabDeviceConfiguration current{};
    LabHALDeviceShape currentShape{};
    IOLockLock(ivars->configLock);
    current = ivars->currentConfiguration;
    currentShape = ivars->currentShape;
    IOLockUnlock(ivars->configLock);
    requested = current;
    requested.sampleRate = static_cast<uint32_t>(in_sample_rate);
    ASFW::Device::DeviceConfiguration requestedModel{};
    const bool valid = DecodeLabConfiguration(requested, requestedModel) &&
        ResolveLabHALDeviceShape(ivars->deviceDefinition, requested,
                                 requestedShape);

    RecordConfigEvent(this, ivars,
                      ASFW::Lab::ADKConfigPhase::HandleSampleRateEnter,
                      0, valid ? kIOReturnSuccess : kIOReturnBadArgument,
                      current.sampleRate, requested.sampleRate,
                      currentShape.outputChannels, requestedShape.outputChannels);
    if (!valid) {
        RecordConfigEvent(this, ivars,
                          ASFW::Lab::ADKConfigPhase::HandleSampleRateReturn,
                          0, kIOReturnBadArgument, current.sampleRate,
                          current.sampleRate, currentShape.outputChannels,
                          currentShape.outputChannels);
        return kIOReturnBadArgument;
    }

    ASFW::Configuration::TransitionResult transition{};
    kern_return_t coordinatorResult = kIOReturnSuccess;
    IOLockLock(ivars->configLock);
    const bool started = DispatchCoordinatorLocked(
        ivars, ASFW::Configuration::ConfigurationEvent{
                   ASFW::Configuration::CoreAudioRateIntent{
                       .endpointId = 1,
                       .routeGeneration = 1,
                       .sampleRate = requestedModel.sampleRate,
                   }},
        transition, coordinatorResult);
    IOLockUnlock(ivars->configLock);
    if (!started) {
        return coordinatorResult;
    }
    if (transition.disposition == ASFW::Configuration::TransitionDisposition::NoOp) {
        return kIOReturnSuccess;
    }
    if (transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::ResolveCandidateEffect>(
            transition.effects[0])) {
        return kIOReturnError;
    }

    const auto resolve = std::get<ASFW::Configuration::ResolveCandidateEffect>(
        transition.effects[0]);
    IOLockLock(ivars->configLock);
    const bool candidateAccepted = DispatchCoordinatorLocked(
        ivars, ASFW::Configuration::ConfigurationEvent{
                   ASFW::Configuration::CandidateAccepted{
                       .identity = resolve.identity,
                       .candidate = requestedModel,
                   }},
        transition, coordinatorResult);
    const uint64_t action = ivars->pendingAction;
    IOLockUnlock(ivars->configLock);
    if (!candidateAccepted || transition.effects.size() != 1 ||
        !std::holds_alternative<ASFW::Configuration::RequestADKWindowEffect>(
            transition.effects[0])) {
        RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::CandidateRejected,
                          action, candidateAccepted ? kIOReturnError : coordinatorResult,
                          current.sampleRate, requested.sampleRate,
                          currentShape.outputChannels, requestedShape.outputChannels);
        RecordConfigEvent(this, ivars,
                          ASFW::Lab::ADKConfigPhase::HandleSampleRateReturn,
                          action, candidateAccepted ? kIOReturnError : coordinatorResult,
                          current.sampleRate, current.sampleRate,
                          currentShape.outputChannels, currentShape.outputChannels);
        return candidateAccepted ? kIOReturnError : coordinatorResult;
    }
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::CandidateAccepted,
                      resolve.identity.token, kIOReturnSuccess, current.sampleRate,
                      requested.sampleRate, currentShape.outputChannels,
                      requestedShape.outputChannels);

    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::RequestCalled,
                      action, kIOReturnSuccess, current.sampleRate,
                      requested.sampleRate, currentShape.outputChannels,
                      requestedShape.outputChannels);
    const kern_return_t requestResult =
        RequestDeviceConfigurationChange(action, nullptr);
    RecordConfigEvent(this, ivars, ASFW::Lab::ADKConfigPhase::RequestReturned,
                      action, requestResult, current.sampleRate,
                      requested.sampleRate, currentShape.outputChannels,
                      requestedShape.outputChannels);
    if (requestResult != kIOReturnSuccess) {
        IOLockLock(ivars->configLock);
        (void)DispatchCoordinatorLocked(
            ivars, ASFW::Configuration::ConfigurationEvent{
                       ASFW::Configuration::ADKWindowRejected{
                           .identity = std::get<ASFW::Configuration::RequestADKWindowEffect>(
                               transition.effects[0]).identity,
                       }},
            transition, coordinatorResult);
        IOLockUnlock(ivars->configLock);
    }
    RecordConfigEvent(this, ivars,
                      ASFW::Lab::ADKConfigPhase::HandleSampleRateReturn,
                      action, requestResult, current.sampleRate,
                      requested.sampleRate, currentShape.outputChannels,
                      requestedShape.outputChannels);
    return requestResult;
}

kern_return_t VirtualAudioDevice::SetScriptedHardwareOutcome(uint32_t in_outcome)
{
    if (ivars == nullptr || ivars->configLock == nullptr ||
        ivars->configurationCoordinator == nullptr) {
        return kIOReturnNotReady;
    }
    ASFW::Lab::ScriptedHardwareOutcome outcome{};
    switch (static_cast<ASFW::Lab::ADKConfigHardwareOutcome>(in_outcome)) {
    case ASFW::Lab::ADKConfigHardwareOutcome::ConfirmRequested:
        outcome = ASFW::Lab::ScriptedHardwareOutcome::ConfirmRequested;
        break;
    case ASFW::Lab::ADKConfigHardwareOutcome::Unchanged:
        outcome = ASFW::Lab::ScriptedHardwareOutcome::Unchanged;
        break;
    case ASFW::Lab::ADKConfigHardwareOutcome::Unknown:
        outcome = ASFW::Lab::ScriptedHardwareOutcome::Unknown;
        break;
    default:
        return kIOReturnBadArgument;
    }
    IOLockLock(ivars->configLock);
    ivars->configurationCoordinator->SetNextHardwareOutcome(outcome);
    IOLockUnlock(ivars->configLock);
    ADK_CONFIG_LOG("slot=%{public}u object=0x%{public}x scripted_hardware_outcome=%{public}u",
                   ivars->labSlot, static_cast<uint32_t>(GetObjectID()), in_outcome);
    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDevice::CopyADKConfigState(OSData** out_data)
{
    if (out_data == nullptr) {
        return kIOReturnBadArgument;
    }
    *out_data = nullptr;
    if (ivars == nullptr || ivars->configLock == nullptr) {
        return kIOReturnNotReady;
    }

    ASFW::Lab::ADKConfigState state{};
    IOLockLock(ivars->configLock);
    state.deviceSlot = ivars->labSlot;
    state.deviceObjectID = static_cast<uint32_t>(GetObjectID());
    state.currentSampleRate =
        ivars->currentSampleRate.load(std::memory_order_relaxed);
    state.pendingSampleRate = ivars->pendingConfiguration.sampleRate;
    state.configurationPending = ivars->configurationPending ? 1u : 0u;
    state.currentOpticalInput = ivars->currentConfiguration.opticalInput;
    state.currentOpticalOutput = ivars->currentConfiguration.opticalOutput;
    state.pendingOpticalInput = ivars->pendingConfiguration.opticalInput;
    state.pendingOpticalOutput = ivars->pendingConfiguration.opticalOutput;
    state.currentInputChannels = ivars->currentShape.inputChannels;
    state.currentOutputChannels = ivars->currentShape.outputChannels;
    state.pendingAction = ivars->pendingAction;
    state.nextSequence = ivars->nextConfigSequence;
    IOLockUnlock(ivars->configLock);

    *out_data = OSData::withBytes(&state, sizeof(state));
    return *out_data != nullptr ? kIOReturnSuccess : kIOReturnNoMemory;
}

kern_return_t VirtualAudioDevice::CopyADKConfigLog(uint32_t in_max_events,
                                                   OSData** out_data)
{
    if (out_data == nullptr) {
        return kIOReturnBadArgument;
    }
    *out_data = nullptr;
    if (ivars == nullptr || ivars->configLock == nullptr) {
        return kIOReturnNotReady;
    }

    const uint32_t requested =
        (in_max_events == 0) ? ASFW::Lab::kADKConfigLogDefaultEvents
        : (in_max_events > ASFW::Lab::kADKConfigLogMaxEvents)
            ? ASFW::Lab::kADKConfigLogMaxEvents
            : in_max_events;
    const size_t capacity = ASFW::Lab::ADKConfigLogBlobSize(requested);
    uint8_t* buffer = IONewZero(uint8_t, capacity);
    if (buffer == nullptr) {
        return kIOReturnNoMemory;
    }

    ASFW::Lab::ADKConfigLogHeader header{};
    uint32_t copied = 0;
    IOLockLock(ivars->configLock);
    const uint32_t available = ivars->configEventCount;
    copied = available < requested ? available : requested;
    header.eventCount = copied;
    header.eventStride = sizeof(ASFW::Lab::ADKConfigEvent);
    header.hostTimeTicks = mach_absolute_time();
    header.deviceSlot = ivars->labSlot;
    header.deviceObjectID = static_cast<uint32_t>(GetObjectID());
    header.currentSampleRate =
        ivars->currentSampleRate.load(std::memory_order_relaxed);
    header.pendingSampleRate = ivars->pendingConfiguration.sampleRate;
    header.configurationPending = ivars->configurationPending ? 1u : 0u;
    if (copied != 0) {
        const uint32_t oldestIndex =
            (ivars->configEventCount == ASFW::Lab::kADKConfigLogMaxEvents)
                ? ivars->configWriteIndex
                : 0u;
        const uint32_t skip = available - copied;
        const uint32_t firstIndex =
            (oldestIndex + skip) % ASFW::Lab::kADKConfigLogMaxEvents;
        const auto* first = &ivars->configEvents[firstIndex];
        header.oldestSequence = first->sequence;
        const uint32_t lastIndex =
            (firstIndex + copied - 1u) % ASFW::Lab::kADKConfigLogMaxEvents;
        header.newestSequence = ivars->configEvents[lastIndex].sequence;

        uint8_t* cursor = buffer + sizeof(header);
        for (uint32_t i = 0; i < copied; ++i) {
            const uint32_t index =
                (firstIndex + i) % ASFW::Lab::kADKConfigLogMaxEvents;
            std::memcpy(cursor + i * sizeof(ASFW::Lab::ADKConfigEvent),
                        &ivars->configEvents[index],
                        sizeof(ASFW::Lab::ADKConfigEvent));
        }
    }
    std::memcpy(buffer, &header, sizeof(header));
    IOLockUnlock(ivars->configLock);

    const size_t blobSize = ASFW::Lab::ADKConfigLogBlobSize(copied);
    *out_data = OSData::withBytes(buffer, static_cast<uint32_t>(blobSize));
    IODelete(buffer, uint8_t, capacity);
    return *out_data != nullptr ? kIOReturnSuccess : kIOReturnNoMemory;
}
