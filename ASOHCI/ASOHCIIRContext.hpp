#pragma once
//
// ASOHCIIRContext.hpp
// Per-IR-context plumbing built on ASOHCIContextBase.
//
// Spec refs (OHCI 1.1):
//   §10.1 IR DMA programs (descriptor forms, INPUT_MORE/INPUT_LAST, DUALBUFFER)
//   §10.2 Receive modes (bufferFill, packet-per-buffer, dual-buffer)
//   §10.3 IR Context registers (ContextMatch channel/tag/sync filtering)
//   §10.5 Interrupts (IsoRx events, buffer management)
//   §10.6 Data format (header/trailer inclusion, status fields)
//   Chapter 6 for host IntEvent / IsoRxIntEvent register demux

#include <stdint.h>
#include "Shared/ASOHCIContextBase.hpp"
#include "ASOHCIIRTypes.hpp"
#include "ASOHCIIRDescriptor.hpp"

class ASOHCIIRContext : public ASOHCIContextBase {
public:
    // ctxIndex: hardware IR context number (0..N-1). Offsets computed in .cpp (see §10.3)
    virtual kern_return_t Initialize(IOPCIDevice* pci,
                                     uint8_t barIndex,
                                     uint32_t ctxIndex);

    // Override Start: configure channel filtering and sync matching before starting
    virtual kern_return_t Start() override;

    virtual void ApplyPolicy(const IRPolicy& policy);
    virtual void ApplyChannelFilter(const IRChannelFilter& filter);

    // Enqueue receive buffers for one of three receive modes (§10.1, §10.2)
    virtual kern_return_t EnqueueStandard(const IRDesc::Program& program,
                                         const IRQueueOptions& opts);
    
    virtual kern_return_t EnqueueDualBuffer(const IRProgram::DualBufferProgram& program,
                                          const IRQueueOptions& opts);

    // Called by manager when isoRxIntEvent indicates this context fired (§10.5; demux via Chapter 6)
    virtual void OnInterruptRx();

    // Expose completion callback and statistics
    virtual void SetCompletionCallback(void (*callback)(const IRCompletion&, void*), void* context);
    virtual const IRStats& GetStats() const { return _stats; }

    // Buffer management for continuous receive
    virtual kern_return_t RefillBuffers();
    virtual bool NeedsRefill() const;

protected:
    virtual void RecoverDeadContext() override; // handle buffer overrun recovery (§10.5)

private:
    uint32_t _ctxIndex = 0;
    IRPolicy _policy{};
    IRChannelFilter _channelFilter{};
    IRStats _stats{};

    // Completion callback
    void (*_completionCallback)(const IRCompletion&, void*) = nullptr;
    void* _completionContext = nullptr;

    // Current receive mode and configuration
    IRMode _currentMode = IRMode::kPacketPerBuffer;
    IRQueueOptions _queueOptions{};

    // Buffer tracking for continuous receive
    struct ReceiveBuffer {
        uint32_t physAddr;
        void* virtAddr;
        uint32_t size;
        bool inUse;
        uint16_t resCount;    // Updated by hardware (bytes remaining)
        uint16_t status;      // Status from descriptor completion
    };
    
    static constexpr uint32_t kMaxReceiveBuffers = 32;
    ReceiveBuffer _buffers[kMaxReceiveBuffers] = {};
    uint32_t _bufferHead = 0;  // Next buffer to fill
    uint32_t _bufferTail = 0;  // Next buffer to retire
    bool _bufferRingFull = false;

    // Context register helpers (§10.3)
    kern_return_t ConfigureContextMatch(uint8_t channel, uint8_t tag, uint8_t sync);
    kern_return_t SetMultiChannelMode(bool enable, uint64_t channelMask);
    
    // Buffer management
    ReceiveBuffer* GetNextFreeBuffer();
    void RetireBuffer(ReceiveBuffer* buffer);
    void ProcessCompletedBuffers();
    
    // Statistics updates
    void UpdateStatsOnPacket(const IRCompletion& completion);
    void UpdateStatsOnError(uint16_t status);
};