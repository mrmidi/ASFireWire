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
#include "../Wire/AMDTP/TxTimingModel.hpp"
#include "../../Shared/Isoch/IsochAudioTransport.hpp"
#include "../../Common/TimingUtils.hpp"

#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSSharedPtr.h>

#include <atomic>
#include <cstdint>

static constexpr uint32_t kReportedDeviceLatencyFrames = 24;
static constexpr uint32_t kReportedSafetyOffsetFrames =
    ASFW::Isoch::Config::kTxBufferProfile.safetyOffsetFrames;
static constexpr uint64_t kZtsMirrorPumpPeriodUsec = 1000;

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
    uint32_t streamModeRaw{0};
    bool hasPhantomOverride{false};
    uint32_t phantomSupportedMask{0};
    uint32_t phantomInitialMask{0};
    uint32_t boolControlCount{0};
    ASFW::Isoch::Audio::BoolControlSlot boolControls[ASFW::Isoch::Audio::kMaxBoolControls]{};

    char inputPlugName[64]{};
    char outputPlugName[64]{};
    char inputChannelNames[8][64]{};
    char outputChannelNames[8][64]{};
};

class DextCycleTimeline final : public ASFW::Ports::ICycleTimeline {
public:
    const ASFW::IsochTransport::TxStreamControl* controlBlock{nullptr};
    mutable uint64_t wrapCount{0};
    mutable uint32_t lastSeconds{0};

    int64_t NowTicks() const noexcept override {
        if (!controlBlock) return 0;
        
        ASFW::IsochTransport::ClockPairSample sample{};
        if (!controlBlock->clockPair.TryRead(sample)) {
            return 0;
        }
        
        const auto decoded = ASFW::Timing::decodeCycleTimer(sample.cycleTimer32);
        if (decoded.seconds < lastSeconds) {
            wrapCount++;
        }
        lastSeconds = decoded.seconds;
        
        const int64_t unwrappedTicks = (static_cast<int64_t>(wrapCount) * 128LL * 24576000LL)
                                     + ASFW::Timing::tstampToOffsets(decoded.seconds, decoded.cycle, decoded.offset);
        return unwrappedTicks;
    }
};

class DextTxSlotProvider final : public ASFW::Protocols::Audio::AMDTP::IAmdtpTxSlotProvider {
public:
    uint8_t* payloadBase{nullptr};
    ASFW::IsochTransport::TxPacketMeta* metadataRing{nullptr};
    ASFW::IsochTransport::TxStreamControl* controlBlock{nullptr};
    uint32_t numSlots{0};
    uint32_t slotStrideBytes{0};
    uint8_t isoChannel{0};

    bool AcquireWritableSlot(uint32_t packetIndex,
                             ASFW::Protocols::Audio::AMDTP::TxPacketSlotView& outSlot) noexcept override {
        if (!payloadBase) return false;
        const uint32_t slotIdx = packetIndex % numSlots;
        outSlot.packetIndex = packetIndex;
        outSlot.bytes = payloadBase + (slotIdx * slotStrideBytes);
        outSlot.capacityBytes = slotStrideBytes;
        return true;
    }

    void PublishSlot(const ASFW::Protocols::Audio::AMDTP::PreparedTxPacket& packet) noexcept override {
        if (!metadataRing || !controlBlock) return;
        const uint32_t slotIdx = packet.packetIndex % numSlots;
        auto& meta = metadataRing[slotIdx];

        meta.packetIndex = packet.packetIndex;
        meta.payloadLength = packet.byteCount;

        // tag = 1 (standard CIP), channel = isoChannel, tcode = 0xA (isoch data block transmit), sy = 0
        const uint32_t isochHeaderHost = (static_cast<uint32_t>(1 & 0x3) << 14) |
                                         (static_cast<uint32_t>(isoChannel & 0x3F) << 8) |
                                         (static_cast<uint32_t>(0xA & 0xF) << 4);
        meta.immediateHeader[0] = OSSwapHostToLittleInt32(isochHeaderHost);

        // CIP Q0 is the first 4 bytes of slot bytes
        const uint8_t* slotBytes = payloadBase + (slotIdx * slotStrideBytes);
        meta.immediateHeader[1] = *reinterpret_cast<const uint32_t*>(slotBytes);

        // Compute expectedGen and release-store commitGen
        const uint64_t gen = ASFW::IsochTransport::ExpectedCommitGen(packet.packetIndex, numSlots);
        meta.commitGen.store(gen, std::memory_order_release);

        // Expose cursor progress to core
        controlBlock->exposeCursor.store(packet.packetIndex + 1, std::memory_order_release);
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

    uint64_t metricsLogCounter{0};
    bool rxStartupDrained{false};

    ASFW::Audio::Runtime::AudioTransportControlBlock directAudioControl;
    ASFW::Audio::Runtime::AudioGraphBinding directAudioGraph;
    ASFW::AudioEngine::Direct::FireWireAudioEngine directAudioEngine;
    ASFW::Audio::Runtime::DirectAudioDebugLogState directAudioDebugLog;
    std::atomic<bool> directAudioSkeletonBound{false};
    std::atomic<uint64_t> ioDebugCallbacks{0};
    std::atomic<bool> ztsTimelineInitialized{false};
    std::atomic<uint64_t> nextExpectedZtsFrame{0};
    std::atomic<bool> txPreparationNotificationScheduled{false};

    ASFW::Protocols::Audio::DICE::DiceTxStreamEngine txStreamEngine;
    ASFW::Driver::TxTimingModel txTimingModel;
    DextTxSlotProvider txSlotProvider;
    DextCycleTimeline txCycleTimeline;
    float* txFloatScratchBuffer{nullptr};
};

struct ASFWAudioDriver_IVars {
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<IOUserAudioDevice> audioDevice;
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

    OSSharedPtr<IOTimerDispatchSource> ztsMirrorTimer;
    OSSharedPtr<OSAction> ztsMirrorAction;
    std::atomic<uint64_t> ztsMirrorTimerTicks{0};

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

[[nodiscard]] ASFW::Audio::Runtime::ZtsMirrorPublishResult PublishSharedZeroTimestampToHAL(ASFWAudioDriver_IVars& ivars,
                                                                                           const char* reason,
                                                                                           bool logSuccess) noexcept;
[[nodiscard]] bool PrimeSharedZeroTimestampToHAL(ASFWAudioDriver_IVars& ivars) noexcept;
void ScheduleZtsMirrorTimer(ASFWAudioDriver_IVars& ivars) noexcept;
void StopZtsMirrorTimer(ASFWAudioDriver_IVars& ivars) noexcept;
bool EnsureZtsMirrorTimer(ASFWAudioDriver& driver, ASFWAudioDriver_IVars& ivars) noexcept;
void PerformLoudTeardown(ASFWAudioDriver_IVars& ivars, const char* reason) noexcept;
} // namespace ASFW::Audio::DriverKit
