#pragma once

#include <DriverKit/OSObject.h>
#include <new>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <memory>
#include <vector>
#include <atomic>
#include <span>

#include "../Shared/Contexts/DmaContextManagerBase.hpp"
#include "../Shared/Memory/IDMAMemory.hpp"
#include "../Shared/Rings/BufferRing.hpp"
#include "../Shared/Rings/DescriptorRing.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "IsochTypes.hpp"
#include "Memory/IIsochDMAMemory.hpp"
#include "Receive/StreamProcessor.hpp"
#include "../Shared/TxSharedQueue.hpp"

namespace ASFW::Isoch {

// Policy trait for Isoch Receive Context to satisfy DmaContextManagerBase requirements
struct IRPolicy {
    // State machine states for the context
    enum class State {
        Stopped,
        Running,
        Stopping
    };
    
    static constexpr State kInitialState = State::Stopped;
    
    // Convert state to string for logging
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
                            public ::ASFW::Shared::DmaContextManagerBase<IsochReceiveContext, ::ASFW::Shared::DescriptorRing, IRTag, IRPolicy> {
    
public:
    IsochReceiveContext() 
        : ::ASFW::Shared::DmaContextManagerBase<IsochReceiveContext, ::ASFW::Shared::DescriptorRing, IRTag, IRPolicy>(*this, descriptorRing_) 
    { }

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

    StreamProcessor& GetStreamProcessor() { return streamProcessor_; }

    /// Attach shared RX queue from ASFWAudioNub (called before Start)
    void SetSharedRxQueue(void* base, uint64_t bytes);

    void LogHardwareState();

private:
    struct Registers {
        ::ASFW::Driver::Register32 CommandPtr;
        ::ASFW::Driver::Register32 ContextControlSet;
        ::ASFW::Driver::Register32 ContextControlClear;
        ::ASFW::Driver::Register32 ContextMatch;
    };
    
    Registers registers_;
    uint8_t contextIndex_{0xFF};
    uint8_t channel_{0xFF};
    
    ::ASFW::Driver::HardwareInterface* hardware_{nullptr};
    std::shared_ptr<::ASFW::Isoch::Memory::IIsochDMAMemory> dmaMemory_{nullptr};
    
    ::ASFW::Shared::DescriptorRing descriptorRing_;
    ::ASFW::Shared::BufferRing bufferRing_;

    StreamProcessor streamProcessor_;

    /// Shared RX queue: producer=Poll()/ProcessPacket, consumer=ASFWAudioDriver (via shared memory)
    ASFW::Shared::TxSharedQueueSPSC rxSharedQueue_;

    /// Cycle-time rate estimation state (per Apple NUDCLREAD pattern)
    struct CycleTimeCorrelation {
        uint32_t prevCycleTimer{0};
        uint64_t prevHostTicks{0};
        bool     hasPrevious{false};
        uint32_t pollsSinceLastUpdate{0};
        double   sampleRate{48000.0};
    } cycleCorr_;

    IsochReceiveCallback callback_{nullptr};
    
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    
    uint32_t lastProcessedIndex_{0};
    
    kern_return_t SetupRings();
    
    Registers GetRegisters(uint8_t index) const;
};

} // namespace ASFW::Isoch
