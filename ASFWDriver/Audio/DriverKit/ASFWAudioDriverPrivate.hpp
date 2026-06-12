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
        // timestamp Saffire's DCL path passes to tstampToOffsets() at 0xe9bf.
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

        // immediateData[0] = isoch packet header: spd=2 (S400) at [18:16],
        // tag=1 (standard CIP) at [15:14], channel at [13:8], tcode=0xA (isoch
        // data block transmit) at [7:4], sy=0. The speed field is mandatory —
        // omitting it transmits at S100 and produces a header the device/
        // analyzer treats as malformed.
        // Cross-validated with Linux: firewire/ohci.h:277-286 and
        // firewire/ohci.c:3377-3381.
        const uint32_t isochHeaderQ0 = (static_cast<uint32_t>(2 & 0x7) << 16) |
                                       (static_cast<uint32_t>(1 & 0x3) << 14) |
                                       (static_cast<uint32_t>(isoChannel & 0x3F) << 8) |
                                       (static_cast<uint32_t>(0xA & 0xF) << 4);
        meta.immediateHeader[0] = OSSwapHostToLittleInt32(isochHeaderQ0);

        // immediateData[1] = data_length (payload bytes) in bits [31:16]. The
        // CIP header is the first 8 bytes of the payload buffer and is shipped
        // by the OUTPUT_LAST descriptor — it does NOT belong in the packet
        // header immediate. Cross-validated with Linux:
        // firewire/ohci.h:287-288 and firewire/ohci.c:3383.
        meta.immediateHeader[1] = OSSwapHostToLittleInt32(
            static_cast<uint32_t>(packet.byteCount & 0xFFFF) << 16);

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

    ASFW::Protocols::Audio::DICE::DiceTxStreamEngine txStreamEngine;
    ASFW::Driver::TxTimingModel txTimingModel;
    DextTxSlotProvider txSlotProvider;
    DextTxExecutionTimeline txExecutionTimeline;
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
    OSSharedPtr<OSAction> txPreparationAction;
    OSSharedPtr<OSAction> ztsAnchorAction;



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

// Prepares transmit slots [startPacketIndex, targetPacketIndex) into the shared
// metadata ring, committing each slot's generation so the IT DMA refill never
// observes an uncommitted slot. Returns the number of slots prepared. Shared by
// the steady-state ZTS pump and the pre-RUN prefill; with an unseeded transmit
// clock the normal AMDTP cadence is preserved but every packet carries NO_INFO
// (SYT=0xffff), matching the reference Saffire seed behavior.
uint32_t PrepareTransmitSlots(ASFWAudioDriver_IVars& ivars,
                              uint64_t startPacketIndex,
                              uint64_t targetPacketIndex,
                              uint32_t maxToPrepare) noexcept;

// Synchronously seeds the transmit ring with cadence-correct NO_INFO packets
// before the IT DMA context starts, so the first refill finds committed slots.
void PrefillTxRingBeforeStart(ASFWAudioDriver_IVars& ivars) noexcept;


void PerformLoudTeardown(ASFWAudioDriver_IVars& ivars, const char* reason) noexcept;
} // namespace ASFW::Audio::DriverKit
