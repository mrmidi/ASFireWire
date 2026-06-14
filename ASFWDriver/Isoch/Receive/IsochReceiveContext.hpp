#pragma once

#include <DriverKit/OSObject.h>
#include <new>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <memory>
#include <vector>
#include <atomic>
#include <span>

#include "../../Shared/Contexts/DmaContextManagerBase.hpp"
#include "../../Shared/Memory/IDMAMemory.hpp"
#include "../../Shared/Rings/DescriptorRing.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../Core/IsochTypes.hpp"
#include "../Memory/IIsochDMAMemory.hpp"

#include "IsochRxDmaRing.hpp"
#include "IsochRxTiming.hpp"
#include "ZtsTelemetry.hpp"
#include "../../Shared/Isoch/AudioTimingGeometry.hpp"
#include "../../Audio/Engine/Direct/DirectInputWriter.hpp"
#include "../../Audio/Engine/Direct/AudioClockPublisher.hpp"
#include "../../Audio/Engine/Direct/Rx/RxAudioPacketProcessor.hpp"
#include "../../Audio/DriverKit/Runtime/AudioGraphBinding.hpp"

namespace ASFW {
namespace Audio::Runtime {
class IDirectAudioBindingSource;
}
}

namespace ASFW::Isoch {

// Policy trait for Isoch Receive Context to satisfy DmaContextManagerBase requirements
struct IRPolicy {
    enum class State {
        Stopped,
        Running,
        Stopping
    };

    static constexpr State kInitialState = State::Stopped;

    static const char* ToStr(State s) {
        switch (s) {
            case State::Stopped: return "Stopped";
            case State::Running: return "Running";
            case State::Stopping: return "Stopping";
            default: return "Unknown";
        }
    }
};

struct IRTag {
    static constexpr const char kContextName[] = "IsochReceiveContext";
};

class IsochReceiveContext : public OSObject,
                            public ::ASFW::Shared::DmaContextManagerBase<IsochReceiveContext,
                                                                         ::ASFW::Shared::DescriptorRing,
                                                                         IRTag,
                                                                         IRPolicy> {
public:
    IsochReceiveContext()
        : ::ASFW::Shared::DmaContextManagerBase<IsochReceiveContext, ::ASFW::Shared::DescriptorRing, IRTag, IRPolicy>(*this, descriptorRing_) {
    }

    virtual bool init() override;
    virtual void free() override;

    void* operator new(size_t size) { return IOMallocZero(size); }
    void* operator new(size_t size, std::nothrow_t const&) { return IOMallocZero(size); }
    void operator delete(void* ptr, size_t size) { IOFree(ptr, size); }

    static OSSharedPtr<IsochReceiveContext> Create(::ASFW::Driver::HardwareInterface* hw,
                                                  std::shared_ptr<::ASFW::Isoch::Memory::IIsochDMAMemory> dmaMemory);

    static constexpr size_t kNumDescriptors = 512;
    static constexpr size_t kMaxPacketSize = 4096;
    static_assert(kNumDescriptors %
                      ASFW::IsochTransport::AudioTimingGeometry::
                          kTimingGroupPackets ==
                  0,
                  "IR descriptor ring must be an integer number of interrupt "
                  "groups or the interrupt cadence breaks at the ring wrap");

    kern_return_t Configure(uint8_t channel,
                            uint8_t contextIndex,
                            Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
                            uint32_t am824Slots = 0);
    kern_return_t Start();
    void Stop();
    uint32_t Poll();

    void SetCallback(IsochReceiveCallback callback);

    void SetDirectAudioBindingSource(ASFW::Audio::Runtime::IDirectAudioBindingSource* source) noexcept;
    
    using TimingLossCallback = std::function<void()>;
    void SetTimingLossCallback(TimingLossCallback callback) noexcept;
    using ZtsAnchorReadyCallback = std::function<void(uint64_t)>;
    void SetZtsAnchorReadyCallback(ZtsAnchorReadyCallback callback) noexcept;

    void LogHardwareState();

    // Off-hot-path drain of the ZTS clock telemetry captured in Poll(). Called
    // by the watchdog; formats up to `maxRecords` evenly-strided records (plus
    // any seed) into the Zts log category. Never call from the interrupt path.
    void DrainZtsTelemetry(uint32_t maxRecords);

    // Off-hot-path drain of the Isoch-Transmit SYT resync telemetry, captured by
    // the audio driver into the shared AudioTransportControlBlock. Formats up to
    // `maxRecords` strided records (plus every seed/reseed) into the TxSyt log
    // category. Called by the watchdog; the producer runs on the audio queue.
    void DrainTxSytTelemetry(uint32_t maxRecords);

private:
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

    uint64_t absoluteFrameCursor_{0};
    bool cursorInitialized_{false};
    uint64_t rxZtsPublishCount_{0};
    uint64_t rxTimestampValidCount_{0};
    uint64_t rxTimestampInvalidCount_{0};

    bool rxCadenceEstablishedLogged_{false};
    Rx::ZtsTelemetryRing ztsTelemetry_{};
    TimingLossCallback timingLossCallback_{nullptr};
    ZtsAnchorReadyCallback ztsAnchorReadyCallback_{nullptr};

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
