#pragma once

#include "ASFWAudioDriver.h"
#include "ASFWAudioNub.h"
#include "Config/AudioDriverConfig.hpp"
#include "Controls/AudioControlBuilder.hpp"
#include "Runtime/AudioGraphBinding.hpp"
#include "Runtime/AudioTransportControlBlock.hpp"
#include "Runtime/DirectAudioDebugSnapshot.hpp"
#include "../Engine/Direct/FireWireAudioEngine.hpp"
#include "../Config/AudioTxProfiles.hpp"
#include "../Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "../../Shared/Isoch/IsochAudioTransport.hpp"
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
    ASFW::Isoch::Config::kTxBufferProfile.safetyOffsetFrames;
struct AudioDriverDeviceState {
    ASFWAudioNub* audioNub{nullptr};
    uint64_t guid{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
    char deviceName[128]{};
    uint32_t channelCount{0};
    uint32_t inputChannelCount{0};
    uint32_t outputChannelCount{0};
    double sampleRates[8]{};
    uint32_t sampleRateCount{0};
    double currentSampleRate{0};
    // Rate reported by a device-initiated clock change (front panel/external
    // sync), pending until PerformDeviceConfigurationChange commits it.
    std::atomic<uint32_t> pendingExternalRateHz{0};
    uint32_t streamModeRaw{0};
    bool hasPhantomOverride{false};
    uint32_t phantomSupportedMask{0};
    uint32_t phantomInitialMask{0};
    uint32_t boolControlCount{0};
    ASFW::Isoch::Audio::BoolControlSlot boolControls[ASFW::Isoch::Audio::kMaxBoolControls]{};

    char inputPlugName[64]{};
    char outputPlugName[64]{};
    char inputChannelNames[ASFW::Isoch::Audio::kMaxNamedChannels][64]{};
    char outputChannelNames[ASFW::Isoch::Audio::kMaxNamedChannels][64]{};
};

class DextTxExecutionTimeline final {
public:
    const ASFW::IsochTransport::TxStreamControl* controlBlock{nullptr};

    [[nodiscard]] bool AnchorForPacket(uint64_t packetIndex,
                                       int64_t& outTicks) const noexcept {
        if (!controlBlock) {
            return false;
        }

        const uint64_t count =
            controlBlock->completionStampCount.load(std::memory_order_acquire);
        if (count == 0) {
            return false;
        }

        uint64_t completedPacketIndex = 0;
        uint32_t timestamp = 0;
        if (!controlBlock->ReadCompletionStamp(
                count - 1, completedPacketIndex, timestamp) ||
            packetIndex < completedPacketIndex) {
            return false;
        }

        // Linux consumes OHCI's 16-bit OUTPUT_LAST status timestamp at
        // firewire/ohci.c:3055. The core expands that stamp at publication
        // with the same refill's CYCLE_TIMER subcycle, yielding the full
        // timestamp Saffire's transmit path passes to tstampToOffsets() at 0xe9bf.
        const auto completed = ASFW::Timing::decodeCycleTimer(timestamp);
        const uint64_t packetDistance = packetIndex - completedPacketIndex;

        outTicks = ASFW::Timing::normalizeOffsetDomain(
            ASFW::Timing::tstampToOffsets(completed.seconds,
                                          completed.cycle %
                                              ASFW::Timing::kCyclesPerSecond,
                                          completed.offset) +
            static_cast<int64_t>(packetDistance) *
                static_cast<int64_t>(ASFW::Timing::kTicksPerCycle));
        return true;
    }
};

class DextTxSlotProvider final : public ASFW::Protocols::Audio::AMDTP::IAmdtpTxSlotProvider {
public:
    uint8_t* payloadBase{nullptr};
    ASFW::IsochTransport::TxPacketMeta* metadataRing{nullptr};
    ASFW::IsochTransport::TxStreamControl* controlBlock{nullptr};
    uint32_t numSlots{0};
    uint32_t slotStrideBytes{0};
    ASFW::IsochTransport::IsochPacketTag isochTag{
        ASFW::IsochTransport::IsochPacketTag::kCip};

    bool AcquireWritableSlot(
        uint32_t packetIndex,
        ASFW::Protocols::Audio::AMDTP::TxPacketSlotView& outSlot)
        noexcept override {
        if (!payloadBase || numSlots == 0 || slotStrideBytes == 0) {
            return false;
        }
        const uint32_t slotIdx = packetIndex % numSlots;
        outSlot.packetIndex = packetIndex;
        outSlot.bytes = payloadBase + (slotIdx * slotStrideBytes);
        outSlot.capacityBytes = slotStrideBytes;
        return true;
    }

    [[nodiscard]] bool PublishSlot(
        const ASFW::Protocols::Audio::AMDTP::PreparedTxPacket& packet)
        noexcept override {
        if (!metadataRing || !controlBlock || numSlots == 0) {
            return false;
        }
        const uint32_t slotIdx = packet.packetIndex % numSlots;
        auto& meta = metadataRing[slotIdx];

        meta.packetIndex = packet.packetIndex;
        meta.payloadLength = packet.byteCount;

        // immediateData[0] = OHCI isoch packet header. Tag 1 identifies a
        // standard CIP packet; tag 0 identifies raw payload with no CIP header
        // (the Fireface 800 former-generation wire format). The owning ring
        // stamps the configured channel immediately before DMA publication.
        const uint32_t isochHeaderQ0 =
            ASFW::IsochTransport::BuildIsochTxHeaderQ0(isochTag);
        meta.immediateHeader[0] = OSSwapHostToLittleInt32(isochHeaderQ0);

        // immediateData[1] = data_length (all bytes transmitted after the OHCI
        // immediate packet header) in bits [31:16]. This is 0 for a true empty
        // no-CIP idle cycle, raw PCM bytes for tag 0, or CIP+payload for tag 1.
        meta.immediateHeader[1] = OSSwapHostToLittleInt32(
            ASFW::IsochTransport::BuildIsochTxHeaderQ1(packet.byteCount));

        // Compute expectedGen and release-store commitGen
        const uint64_t gen = ASFW::IsochTransport::ExpectedCommitGen(packet.packetIndex, numSlots);
        meta.commitGen.store(gen, std::memory_order_release);

        // Expose cursor progress to core
        controlBlock->exposeCursor.store(packet.packetIndex + 1, std::memory_order_release);
        return true;
    }

    uint32_t SlotCount() const noexcept override {
        return numSlots;
    }
};

// Runtime layout is intentionally organized around hot-path state ownership, not field packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct AudioDriverRuntimeState {
    uint64_t hostTicksPerBuffer{0};
    std::atomic<bool> isRunning{false};
    std::atomic<uint64_t> lastHalZeroTimestampGeneration{0};
    std::atomic<uint64_t> lastHalZeroTimestampSampleFrame{0};
    std::atomic<uint64_t> lastHalZeroTimestampHostTicks{0};

    uint64_t metricsLogCounter{0};
    bool rxStartupDrained{false};

    ASFW::Audio::Runtime::AudioTransportControlBlock directAudioControl;
    ASFW::Audio::Runtime::AudioGraphBinding directAudioGraph;
    ASFW::AudioEngine::Direct::FireWireAudioEngine directAudioEngine;
    ASFW::Audio::Runtime::DirectAudioDebugLogState directAudioDebugLog;
    std::atomic<bool> directAudioSkeletonBound{false};
    std::atomic<uint64_t> ioDebugCallbacks{0};
    std::atomic<uint64_t> ioCallbacksOutsideRun{0};
    std::atomic<bool> txActive{false};

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
    OSSharedPtr<IOMemoryMap> txMetadataMapSecondary;
    OSSharedPtr<IOMemoryMap> txControlMapSecondary;
    OSSharedPtr<OSAction> txPreparationAction;
    OSSharedPtr<IODispatchQueue> txPreparationQueue;
    OSSharedPtr<OSAction> ztsAnchorAction;
    OSSharedPtr<IODispatchQueue> ztsQueue;
    OSSharedPtr<OSAction> deviceClockChangedAction;



    AudioDriverDeviceState device;
    AudioDriverRuntimeState runtime;
};

struct AudioGraphStartState {
    bool inputStreamAdded{false};
    bool outputStreamAdded{false};
    bool audioDeviceAdded{false};
};

namespace ASFW::Audio::DriverKit {

[[nodiscard]] uint32_t FrameCapacityFromSegment(const IOAddressSegment& segment,
                                                uint32_t channels) noexcept;
[[nodiscard]] bool BindDirectAudioSkeleton(ASFWAudioDriver_IVars& ivars) noexcept;
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
                                                                                           bool logSuccess) noexcept;
// Prepares transmit slots from startPacketIndex until both producer invariants
// are true or limitPacketIndex is reached:
//   * requiredPacketIndex covers the core refill / commit-generation invariant.
//   * targetFrameEnd covers the AMDTP frame-exposure invariant for WriteEnd.
// Returns the number of slots prepared. With an unseeded transmit clock the
// normal AMDTP cadence is preserved but every packet carries NO_INFO
// (SYT=0xffff), matching the reference Saffire seed behavior. Set
// allowRecoveredClock only after HAL has accepted the first real RX anchor.
uint32_t PrepareTransmitSlots(ASFWAudioDriver_IVars& ivars,
                              uint64_t startPacketIndex,
                              uint64_t requiredPacketIndex,
                              uint64_t limitPacketIndex,
                              uint32_t maxToPrepare,
                              uint64_t targetFrameEnd,
                              bool allowRecoveredClock) noexcept;

// Synchronously seeds the transmit ring with cadence-correct NO_INFO packets
// before the IT DMA context starts, so the first refill finds committed slots.
void PrefillTxRingBeforeStart(ASFWAudioDriver_IVars& ivars) noexcept;


void PerformLoudTeardown(ASFWAudioDriver_IVars& ivars, const char* reason) noexcept;
} // namespace ASFW::Audio::DriverKit
