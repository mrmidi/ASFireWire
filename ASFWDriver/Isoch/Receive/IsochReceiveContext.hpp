#pragma once

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOLib.h>
#include <atomic>
#include <memory>
#include <new>
#include <span>
#include <vector>

#include "../../Hardware/HardwareInterface.hpp"
#include "../../Shared/Contexts/DmaContextManagerBase.hpp"
#include "../../Shared/Memory/IDMAMemory.hpp"
#include "../../Shared/Rings/DescriptorRing.hpp"
#include "../Core/IsochTypes.hpp"
#include "../Memory/IIsochDMAMemory.hpp"

#include "../../Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "../../Audio/Engine/Direct/AudioClockPublisher.hpp"
#include "../../Audio/Engine/Direct/DirectInputWriter.hpp"
#include "../../Audio/Engine/Direct/Rx/RxAudioPacketProcessor.hpp"
#include "../../Shared/Isoch/AudioTimingGeometry.hpp"
#include "IsochRxDmaRing.hpp"
#include "IsochRxTiming.hpp"
#include "ZtsTelemetry.hpp"

namespace ASFW {
namespace Audio::Runtime {
class IDirectAudioBindingSource;
}
} // namespace ASFW

namespace ASFW::Isoch {

// Policy trait for Isoch Receive Context to satisfy DmaContextManagerBase requirements
struct IRPolicy {
    enum class State { Stopped, Running, Stopping };

    static constexpr State kInitialState = State::Stopped;

    static const char* ToStr(State s) {
        switch (s) {
        case State::Stopped:
            return "Stopped";
        case State::Running:
            return "Running";
        case State::Stopping:
            return "Stopping";
        default:
            return "Unknown";
        }
    }
};

struct IRTag {
    static constexpr const char kContextName[] = "IsochReceiveContext";
};

// Plain C++ class, deliberately NOT an OSObject: nothing needs OSObject
// semantics (no OSAction target, no cast, no IIG surface), and an OSObject
// created with plain `new` instead of OSTypeAlloc is born with refcount 0 —
// its first release() aborts the dext with an over-release assert at teardown.
// Owned by std::unique_ptr, matching IsochTransmitContext.
class IsochReceiveContext final
    : public ::ASFW::Shared::DmaContextManagerBase<
          IsochReceiveContext, ::ASFW::Shared::DescriptorRing, IRTag, IRPolicy> {
  public:
    IsochReceiveContext()
        : ::ASFW::Shared::DmaContextManagerBase<IsochReceiveContext, ::ASFW::Shared::DescriptorRing,
                                                IRTag, IRPolicy>(*this, descriptorRing_) {}
    ~IsochReceiveContext();

    IsochReceiveContext(const IsochReceiveContext&) = delete;
    IsochReceiveContext& operator=(const IsochReceiveContext&) = delete;

    static std::unique_ptr<IsochReceiveContext>
    Create(::ASFW::Driver::HardwareInterface* hw,
           std::shared_ptr<::ASFW::Isoch::Memory::IIsochDMAMemory> dmaMemory);

    static constexpr size_t kNumDescriptors =
        ASFW::IsochTransport::AudioTimingGeometry::kRxDescriptorPackets;
    static constexpr size_t kMaxPacketSize = 4096;
    static_assert(kNumDescriptors %
                          ASFW::IsochTransport::AudioTimingGeometry::kTimingGroupPackets ==
                      0,
                  "IR descriptor ring must be an integer number of interrupt "
                  "groups or the interrupt cadence breaks at the ring wrap");

    // channelOffset/streamChannels/isSecondary configure multi-stream
    // de-interleave: this context writes `streamChannels` PCM channels (0 == the
    // binding's full width) into the shared input buffer at `channelOffset`. A
    // secondary context writes PCM only and does not own the clock/ZTS/replay.
    kern_return_t
    Configure(uint8_t channel, uint8_t contextIndex,
              Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
              uint32_t am824Slots = 0, uint32_t channelOffset = 0, uint32_t streamChannels = 0,
              bool isSecondary = false);
    kern_return_t Start();
    void Stop();
    uint32_t Poll();

    void SetCallback(IsochReceiveCallback callback);

    void
    SetDirectAudioBindingSource(ASFW::Audio::Runtime::IDirectAudioBindingSource* source) noexcept;

    using TimingLossCallback = std::function<void()>;
    void SetTimingLossCallback(TimingLossCallback callback) noexcept;
    using ZtsAnchorReadyCallback = std::function<void(uint64_t)>;
    void SetZtsAnchorReadyCallback(ZtsAnchorReadyCallback callback) noexcept;
    using ReplayReadyCallback = std::function<void()>;
    void SetReplayReadyCallback(ReplayReadyCallback callback) noexcept;
    [[nodiscard]] bool IsReplayEstablished() const noexcept;

    void LogHardwareState();

    // Off-hot-path drain of the ZTS clock telemetry captured in Poll(). Called
    // by the watchdog; formats up to `maxRecords` evenly-strided records (plus
    // any seed) into the Zts log category. Never call from the interrupt path.
    void DrainZtsTelemetry(uint32_t maxRecords);

    // Off-hot-path drain of the audio payload writer telemetry, captured by the
    // audio driver into the shared AudioTransportControlBlock. Formats up to
    // `maxRecords` strided records into the PayloadWriter log category.
    // Called by the watchdog.
    void DrainPayloadWriterTelemetry(uint32_t maxRecords);

    // Off-hot-path log of the latest live replay TX SYT decision, published by
    // the audio driver into the shared AudioTransportControlBlock. Emits one
    // line into the TxSyt log category. Called by the watchdog (~1 s).
    void LogTxSytTrace();

  private:
    void ResetReplayEpochForDiscontinuity() noexcept;

    struct Registers {
        ::ASFW::Driver::Register32 CommandPtr;
        ::ASFW::Driver::Register32 ContextControlSet;
        ::ASFW::Driver::Register32 ContextControlClear;
        ::ASFW::Driver::Register32 ContextMatch;
    };

    Registers registers_{};
    uint8_t contextIndex_{0xFF};
    uint8_t channel_{0xFF};

    ::ASFW::Driver::HardwareInterface* hardware_{nullptr};
    std::shared_ptr<::ASFW::Isoch::Memory::IIsochDMAMemory> dmaMemory_{nullptr};

    ::ASFW::Shared::DescriptorRing descriptorRing_{};

    Rx::IsochRxDmaRing rxRing_{};

    IsochReceiveCallback callback_{nullptr};
    std::atomic_flag rxLock_ = ATOMIC_FLAG_INIT;

    ASFW::Audio::Runtime::IDirectAudioBindingSource* directAudioBindingSource_{nullptr};
    uint64_t lastDirectAudioGeneration_{0};

    ASFW::AudioEngine::Direct::DirectInputWriter directInputWriter_{};
    ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor directProcessor_{directInputWriter_};
    ASFW::Audio::Runtime::AudioGraphBinding directInputView_{};
    ASFW::AudioEngine::Direct::AudioClockPublisher clockPublisher_{};

    Encoding::AudioWireFormat wireFormat_{Encoding::AudioWireFormat::kAM824};
    uint32_t am824Slots_{0};

    // Multi-stream de-interleave: this context decodes `streamChannels_` PCM
    // channels (0 == use the binding's full inputChannels) and writes them into
    // the shared interleaved input buffer at `channelOffset_`. A secondary
    // stream writes PCM only — the master (isSecondary_ == false) owns the
    // clock/ZTS/replay timeline and the producer cursor.
    uint32_t channelOffset_{0};
    uint32_t streamChannels_{0};
    bool isSecondary_{false};

    // Secondary-slice frame anchoring. A secondary context runs on its own OHCI
    // IR context that arms/starts independently of the master, so its private
    // cursor (from 0) has no relation to the master's ring position. We anchor it
    // to the master's published inputProducedEndFrame on the first write of each
    // replay epoch so both halves of a frame land in the same ring slot; the two
    // streams are frame-locked by the device clock and stay aligned thereafter.
    bool secondaryAnchored_{false};
    uint64_t secondaryAnchorEpoch_{0};

    uint64_t absoluteFrameCursor_{0};
    bool cursorInitialized_{false};
    uint64_t rxZtsPublishCount_{0};
    uint64_t rxTimestampValidCount_{0};
    uint64_t rxTimestampInvalidCount_{0};
    // Negative age = the per-packet cycle stamp expanded to AFTER the master
    // drain read. Occasional small negatives (< one bus cycle) are normal on
    // coalesced multi-group drains and are handled (host time advances instead
    // of rewinds). FREQUENT or LARGE (>= one cycle) negatives indicate the
    // timestamp expansion / drain-pair reference is wrong — watch these.
    uint64_t rxNegativeAgeCount_{0};
    uint64_t rxLargeNegativeAgeCount_{0};

    bool rxCadenceEstablishedLogged_{false};
    Rx::ZtsTelemetryRing ztsTelemetry_{};
    TimingLossCallback timingLossCallback_{nullptr};
    ZtsAnchorReadyCallback ztsAnchorReadyCallback_{nullptr};
    ReplayReadyCallback replayReadyCallback_{nullptr};
    bool replayReadyNotified_{false};
    bool replayResetForStart_{false};
    bool replayCycleInitialized_{false};
    uint32_t lastReplayCycleOrdinal_{0};

    // DBC tracking for device-domain frame count.
    // Updated on every packet in Poll(), exposed via rxDbcFrameCount in ATCB.
    uint8_t lastDbc_{0};
    bool dbcInitialized_{false};

    // Drain-only state for consecutive-anchor clock comparison.
    // Not hot-path: only touched in DrainZtsTelemetry.
    uint64_t prevAnchorFrame_{0};
    uint64_t prevAnchorHostTicks_{0};
    bool prevAnchorValid_{false};

    Registers GetRegisters(uint8_t index) const;
};

} // namespace ASFW::Isoch
