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
#include "../Core/IsochDmaGeometry.hpp"
#include "../Memory/IIsochDMAMemory.hpp"

#include "IsochRxDmaRing.hpp"

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
        IsochDmaGeometry::kReceiveDescriptorPackets;
    static constexpr size_t kMaxPacketSize = 4096;
    static_assert(kNumDescriptors %
                          IsochDmaGeometry::kPacketsPerInterrupt ==
                      0,
                  "IR descriptor ring must be an integer number of interrupt "
                  "groups or the interrupt cadence breaks at the ring wrap");

    kern_return_t Configure(uint8_t channel, uint8_t contextIndex);
    kern_return_t Start();
    // Clearing RUN only prevents new descriptor fetches.  The caller must not
    // release any DMA-visible memory until this returns success (ACTIVE clear).
    [[nodiscard]] kern_return_t Stop();
    uint32_t Poll();

    void SetCallback(IsochReceiveCallback callback);
    // Content-neutral synchronous seam. The consumer owns all payload
    // interpretation and any state derived from it.
    void SetReceiveConsumer(IIsochReceiveConsumer* consumer) noexcept;

    void LogHardwareState();

    void DrainZtsTelemetry(uint32_t maxRecords);
    void DrainPayloadWriterTelemetry();
    void LogTxSytTrace();

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
    IIsochReceiveConsumer* receiveConsumer_{nullptr};
    std::atomic_flag rxLock_ = ATOMIC_FLAG_INIT;

    Registers GetRegisters(uint8_t index) const;
};

} // namespace ASFW::Isoch
