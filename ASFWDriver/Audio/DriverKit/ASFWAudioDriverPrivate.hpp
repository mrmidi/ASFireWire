#pragma once

#include "ASFWAudioDriver.h"
#include "ASFWAudioNub.h"
#include "Config/AudioDriverConfig.hpp"
#include "Config/ResolvedAudioStreamProfile.hpp"
#include "Controls/AudioControlBuilder.hpp"
#include "Runtime/AudioGraphBinding.hpp"
#include "Runtime/AudioTransportControlBlock.hpp"
#include "Runtime/DirectAudioDebugSnapshot.hpp"
#include "../Engine/Direct/FireWireAudioEngine.hpp"
#include "../Config/AudioTxProfiles.hpp"
#include "../Shared/AudioRuntimeTuning.hpp"
#include "../Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "../Families/BeBoB/MAudio/MAudioDuplexPolicy.hpp"
#include "../Families/BeBoB/MAudio/MAudioInternalTxTiming.hpp"
#include "../Families/BeBoB/MAudio/MAudioPresentationObserver.hpp"
#include "../Runtime/PcmPublicationCache.hpp"
#include "../Runtime/PublicationRangeRing.hpp"
#include "../Runtime/TxLatencySession.hpp"
#include "../Shared/Configuration/DeviceConfigurationStateMachine.hpp"
#include "../Shared/TxCycleAnchor.hpp"
#include "../../Isoch/Core/IsochTxQueue.hpp"
#include "../../Shared/Isoch/TxPayloadSeal.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Common/TimingUtils.hpp"

#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IOMemoryDescriptor.h>

class ASFWAudioDevice;
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSSharedPtr.h>

#include <atomic>
#include <cstdint>

static constexpr uint32_t kReportedDeviceLatencyFrames = 24;
static constexpr uint32_t kReportedSafetyOffsetFrames =
    ASFW::Audio::Config::kTxBufferProfile.safetyOffsetFrames;
struct AudioDriverDeviceState {
    ASFWAudioNub* audioNub{nullptr};
    uint64_t endpointId{0};
    uint64_t deviceInstanceId{0};
    uint64_t observedGuid{0};
    char deviceName[128]{};
    char vendorName[128]{};
    char coreAudioUid[192]{};
    uint32_t channelCount{0};
    uint32_t inputChannelCount{0};
    uint32_t outputChannelCount{0};
    double sampleRates[8]{};
    uint32_t sampleRateCount{0};
    double currentSampleRate{0};
    uint32_t streamModeRaw{0};
    uint32_t boolControlCount{0};
    ASFW::Isoch::Audio::BoolControlSlot boolControls[ASFW::Isoch::Audio::kMaxBoolControls]{};

    char inputPlugName[64]{};
    char outputPlugName[64]{};
    char inputChannelNames[ASFW::Isoch::Audio::kMaxNamedChannels][64]{};
    char outputChannelNames[ASFW::Isoch::Audio::kMaxNamedChannels][64]{};
};

#include "../Engine/DextTxSlotProvider.hpp"

using ASFW::Audio::DextTxExecutionTimeline;
using ASFW::Audio::DextTxSlotProvider;
using ASFW::Audio::kAmdtpCipHeaderBytes;


// Runtime layout is intentionally organized around hot-path state ownership, not field packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct AudioDriverRuntimeState {
    uint64_t hostTicksPerBuffer{0};
    std::atomic<bool> isRunning{false};
    std::atomic<uint64_t> lastHalZeroTimestampGeneration{0};
    std::atomic<uint64_t> lastHalZeroTimestampSampleFrame{0};
    std::atomic<uint64_t> lastHalZeroTimestampHostTicks{0};
    uint64_t mAudioTxClockEpoch{0};

    uint64_t metricsLogCounter{0};
    bool rxStartupDrained{false};
    bool txPlanBusTicksValid{false};
    uint64_t lastTxPlanBusTicks{0};
    // Correlation-only high-water mark for the completion-stamp expansion.
    // Named for what it holds: the pair it replaced was written with completion
    // times and checked against correlation times.
    ASFW::Audio::Shared::TxCorrelationUnwrapState txCorrelationUnwrap{};

    // Silent-NO-DATA attribution. A plan that never reaches the PCM cache
    // leaves every copy counter at zero, so the two paths that can drop a
    // DATA decision before the cache is consulted count themselves here.
    uint64_t txNoCycleAnchorEvents{0};
    uint64_t txNoPresentationOriginEvents{0};
    // Geometry actually in force. Defaults to the shipping constants, so every
    // read is valid before an operator has ever touched the panel. Written only
    // inside a configuration-change window, when IO is stopped.
    ASFW::Audio::Shared::AudioRuntimeTuning activeTuning{};
    // Last reported [TxAlign] divergence between the TX content cursor and the
    // receive-derived projection for the same presentation time. Held so the
    // probe can log on change rather than per packet; reset at the seed and on
    // stream start. Single-writer on the TX preparation queue.
    int64_t txAlignmentDeltaFrames{0};
    bool txAlignmentValid{false};
    uint64_t txReplayResyncs{0};
    /// Lowest packet whose content has not been settled yet. It advances past
    /// packets that were filled, frozen, or carry no samples, and stops at the
    /// first whose content has not been published -- content arrives in order,
    /// so there is nothing beyond it worth trying this pass.
    uint64_t txFillCursor{0};
    /// Times the content cursor was found ahead of the committed end, which
    /// means a start path left it stale and no packet can be filled.
    uint64_t txFillCursorAheadEvents{0};
    /// Next unread TX completion stamp. Completion stamps are pushed one per
    /// completed packet, so reading only the newest one skipped every other
    /// packet in the wake -- and with it every ZTS boundary that fell in one.
    /// Owned by the same serialized TxPreparation queue as the observer.
    uint64_t txCompletionStampCursor{0};

    ASFW::Audio::Runtime::AudioTransportControlBlock directAudioControl;
    ASFW::Audio::Runtime::AudioGraphBinding directAudioGraph;
    ASFW::AudioEngine::Direct::FireWireAudioEngine directAudioEngine;
    ASFW::Audio::Runtime::DirectAudioDebugLogState directAudioDebugLog;
    std::atomic<bool> directAudioSkeletonBound{false};
    std::atomic<uint64_t> ioDebugCallbacks{0};
    std::atomic<uint64_t> ioCallbacksOutsideRun{0};
    std::atomic<bool> txActive{false};
    // Owned by the serial TxPreparation queue while active. StartIO arms it
    // before TX DMA starts; StopIO drains that queue before disarming it.
    ASFW::Audio::Families::BeBoB::MAudio::PresentationObserver
        mAudioPresentationObserver;
    // Shares the same serialized owner as mAudioPresentationObserver but produces
    // wire packet timing from actual OUTPUT_LAST completion stamps.
    ASFW::Audio::Families::BeBoB::MAudio::InternalTxTiming
        mAudioInternalTxTiming;

    ASFW::Audio::Runtime::PcmPublicationCache pcmPublicationCache;
    ASFW::Audio::Runtime::PublicationRangeRing publicationHistory;
    std::shared_ptr<ASFW::Audio::Runtime::TxLatencySession> txLatencySession{
        std::make_shared<ASFW::Audio::Runtime::TxLatencySession>()};

    ASFW::Protocols::Audio::DICE::DiceTxStreamEngine txStreamEngine;
    ASFW::Audio::Runtime::RxSequenceReplayReader txReplayReader;
    DextTxSlotProvider txSlotProvider;
    DextTxExecutionTimeline txExecutionTimeline;

    // Secondary playback stream (multi-stream DICE, e.g. Venice F32 = 2×16). It
    // shadows the master's per-packet timing in lockstep (same packetIndex/SYT/
    // disposition) and differs only in payload: it encodes host output channels
    // [pcmChannels, 2×pcmChannels). Inactive (txSecondaryActive == false) for
    // single-stream devices, leaving the master path untouched.
    ASFW::Protocols::Audio::DICE::DiceTxStreamEngine txStreamEngineSecondary;
    DextTxSlotProvider txSlotProviderSecondary;
    bool txSecondaryActive{false};

    // One-shot SYT seed trace for the M-Audio internal-clock path. The first
    // DATA packet after the transmit anchor lands prints the seed; the next few
    // print their own SYT and the tick delta from the previous one, which is
    // the whole diagnostic — it must equal the rate's exact SYT step (4096 at
    // 48 kHz). Then it goes quiet for the life of the stream, so this is a
    // bounded burst rather than hot-path logging. Rearmed by StartIO.
    static constexpr uint32_t kSytSeedTracePackets = 8;
    uint32_t sytSeedTraceRemaining{0};
    uint16_t sytSeedTracePrevSyt{0};
    bool sytSeedTraceHavePrev{false};
};

struct ASFWAudioDriver_IVars {
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<ASFWAudioDevice> audioDevice;
    OSSharedPtr<IOUserAudioStream> inputStream;
    OSSharedPtr<IOUserAudioStream> outputStream;
    OSSharedPtr<IOMemoryDescriptor> inputBuffer;
    OSSharedPtr<IOMemoryDescriptor> outputBuffer;
    OSSharedPtr<IOMemoryDescriptor> controlBuffer;
    OSSharedPtr<IOMemoryMap> inputMap;
    OSSharedPtr<IOMemoryMap> outputMap;
    OSSharedPtr<IOMemoryMap> controlMap;

    OSSharedPtr<IOMemoryDescriptor> txPayloadBuffer;
    OSSharedPtr<IOMemoryDescriptor> txMetadataBuffer;
    OSSharedPtr<IOMemoryDescriptor> txControlBuffer;
    OSSharedPtr<IOMemoryMap> txPayloadMap;
    OSSharedPtr<IOMemoryMap> txMetadataMap;
    OSSharedPtr<IOMemoryMap> txControlMap;

    // Secondary playback stream shared resources (Venice F32 = 2×16). Mirrors the
    // master set above; unused for single-stream devices.
    OSSharedPtr<IOMemoryDescriptor> txPayloadBufferSecondary;
    OSSharedPtr<IOMemoryDescriptor> txMetadataBufferSecondary;
    OSSharedPtr<IOMemoryDescriptor> txControlBufferSecondary;
    OSSharedPtr<IOMemoryMap> txPayloadMapSecondary;

    // MIDI byte seam for transmit. The descriptor belongs to ASFWMidiNub and is
    // relayed through ASFWAudioNub; both are retained for the stream's lifetime
    // because the TX engine holds a pointer into the mapping. WP-6 moves this
    // to the shared session.
    OSSharedPtr<IOMemoryDescriptor> txMidiTransportBuffer;
    OSSharedPtr<IOMemoryMap> txMidiTransportMap;
    OSSharedPtr<IOMemoryMap> txMetadataMapSecondary;
    OSSharedPtr<IOMemoryMap> txControlMapSecondary;
    OSSharedPtr<OSAction> txPreparationAction;
    OSSharedPtr<OSAction> txTransportFaultAction;
    OSSharedPtr<IODispatchQueue> txPreparationQueue;
    OSSharedPtr<OSAction> ztsAnchorAction;
    OSSharedPtr<OSAction> deviceConfigurationRequestedAction;
    OSSharedPtr<OSAction> runtimeTuningRequestedAction;
    OSSharedPtr<IODispatchQueue> ztsQueue;



    AudioDriverDeviceState device;
    ASFW::Audio::DriverKit::ResolvedAudioStreamProfile resolvedProfile;
    AudioDriverRuntimeState runtime;
};

struct AudioGraphStartState {
    bool inputStreamAdded{false};
    bool outputStreamAdded{false};
    bool audioDeviceAdded{false};
};

namespace ASFW::Audio::DriverKit {

// Physical direct-memory geometry may be wider than the CoreAudio-visible
// topology. DICE devices can require a hidden return stream for clock/control
// purposes even when their user-facing device has no input stream.
struct DirectAudioMemoryGeometry final {
    // Logical ring lengths are supplied by AudioEndpointRuntime.  The backing
    // descriptors may be allocated for a wider configuration, so deriving the
    // count from descriptor bytes and an active channel stride would invent
    // frames after an ADAT -> S/PDIF transition.
    uint32_t inputFrames{0};
    uint32_t outputFrames{0};
    uint32_t inputChannels{0};
    uint32_t outputChannels{0};
};

[[nodiscard]] bool BindDirectAudioSkeleton(
    ASFWAudioDriver_IVars& ivars,
    DirectAudioMemoryGeometry physicalGeometry) noexcept;
// Re-shapes the active view of the lifetime-owned descriptors while I/O is
// stopped. It never replaces a descriptor or mapping.
[[nodiscard]] bool UpdateDirectAudioGeometry(
    ASFWAudioDriver_IVars& ivars,
    DirectAudioMemoryGeometry physicalGeometry) noexcept;
void UnbindDirectAudioSkeleton(ASFWAudioDriver_IVars& ivars) noexcept;

namespace DirectDiagnostics {
void MaybeLogDirectAudioDebugSnapshot(AudioDriverRuntimeState& runtime) noexcept;
void ForceLogDirectAudioDebugSnapshot(AudioDriverRuntimeState& runtime, const char* context) noexcept;
} // namespace DirectDiagnostics

[[nodiscard]] kern_return_t InstallIOOperationHandler(IOUserAudioDevice& audioDevice,
                                                      ASFWAudioDriver_IVars& ivars) noexcept;

[[nodiscard]] kern_return_t BuildAudioGraph(ASFWAudioDriver& driver,
                                            IOService* provider,
                                            ASFWAudioDriver_IVars& ivars,
                                            AudioGraphStartState& state) noexcept;
void TearDownAudioGraph(ASFWAudioDriver& driver,
                        ASFWAudioDriver_IVars& ivars,
                        AudioGraphStartState* state) noexcept;
void ResetDeviceStateFromDefaultConfig(ASFWAudioDriver_IVars& ivars) noexcept;

// Single construction point for the HAL-facing Float32 stream format. The
// format set as a stream's current format on a rate change must be
// byte-identical to the advertised entry built at graph creation, so both
// call this.
void FillFloat32Format(IOUserAudioStreamBasicDescription& fmt,
                       double sampleRate,
                       uint32_t channels) noexcept;

[[nodiscard]] ASFW::Audio::Runtime::ZtsMirrorPublishResult PublishSharedZeroTimestampToHAL(ASFWAudioDriver_IVars& ivars,
                                                                                           const char* reason,
                                                                                           bool logSuccess,
                                                                                           bool countAsRx = true) noexcept;
// Prepares explicit physical presentation plans through requiredPacketIndex.
// WriteEnd does not provide a frame horizon and cannot move this scheduler.
uint32_t PrepareTransmitSlots(ASFWAudioDriver_IVars& ivars,
                              uint64_t startPacketIndex,
                              uint64_t requiredPacketIndex,
                              bool useMAudioInternalTiming) noexcept;

// Synchronously seeds the transmit ring with cadence-correct NO_INFO packets
// before the IT DMA context starts, so the first refill finds committed slots.
void PrefillTxRingBeforeStart(ASFWAudioDriver_IVars& ivars) noexcept;
/// Producer-side republish of the transmit prefill for a restart that re-arms
/// an already-prepared context. See the definition for why StartIO's prefill is
/// not enough.
void RepublishTxRingForRestart(ASFWAudioDriver_IVars& ivars) noexcept;


void PerformLoudTeardown(ASFWAudioDriver_IVars& ivars, const char* reason) noexcept;
} // namespace ASFW::Audio::DriverKit
