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
#include "../IsochTypes.hpp"
#include "../Memory/IIsochDMAMemory.hpp"

#include "IsochRxDmaRing.hpp"
#include "IsochAudioRxPipeline.hpp"

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
    static constexpr std::string_view kContextName = "IsochReceiveContext";
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

    kern_return_t Configure(uint8_t channel, uint8_t contextIndex);
    kern_return_t Start();
    void Stop();
    uint32_t Poll();

    void SetCallback(IsochReceiveCallback callback);

    StreamProcessor& GetStreamProcessor() { return audio_.StreamProcessorRef(); }

    void SetSharedRxQueue(void* base, uint64_t bytes);
    void SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept;

    void LogHardwareState();

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
    Rx::IsochAudioRxPipeline audio_{};

    IsochReceiveCallback callback_{nullptr};
    std::atomic_flag rxLock_ = ATOMIC_FLAG_INIT;

    Registers GetRegisters(uint8_t index) const;
};

} // namespace ASFW::Isoch

