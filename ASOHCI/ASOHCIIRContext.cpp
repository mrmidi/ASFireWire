//
// ASOHCIIRContext.cpp
// Isochronous Receive (IR) context implementation (DriverKit-friendly)
//
// Spec anchors:
//   Host interrupt + IsoRx event/mask registers: OHCI 1.1 Chapter 6 (event bits demux; not IR-specific semantics)
//   IR DMA programs & descriptor usage: §10.1
//   IR Context registers / channel/tag/sync filtering: §10.3
//   IR receive modes (bufferFill, packet-per-buffer, dual-buffer): §10.2
//   IR interrupt meanings (buffer management, overrun handling): §10.5
//   IR data format (header/trailer inclusion, status fields): §10.6
//

#include "ASOHCIIRContext.hpp"
#include "ASOHCICtxRegMap.hpp"
#include "ASOHCIDescriptorUtils.hpp"
#include "ASOHCIMemoryBarrier.hpp"
#include "Shared/ASOHCIContextBase.hpp"
#include "Shared/ASOHCITypes.hpp"
#include "LogHelper.hpp"
#include "OHCIConstants.hpp"

#include <DriverKit/IOReturn.h>
#include <os/log.h>
#include <DriverKit/IOLib.h>
#include <string.h>

// INPUT_* command constants available in IRDescOps namespace

kern_return_t ASOHCIIRContext::Initialize(IOPCIDevice* pci,
                                          uint8_t barIndex,
                                          uint32_t ctxIndex)
{
    if (!pci) return kIOReturnBadArgument;
    _ctxIndex = ctxIndex;
    _policy = {};
    _channelFilter = {};
    _stats = {};
    
    // Clear buffer ring
    memset(_buffers, 0, sizeof(_buffers));
    _bufferHead = 0;
    _bufferTail = 0;
    _bufferRingFull = false;
    
    // Compute per-context register offsets (read/base + set/clear/cmd)
    ASContextOffsets offs{};
    if (!ASOHCICtxRegMap::Compute(ASContextKind::kIR_Receive, _ctxIndex, &offs)) {
        return kIOReturnBadArgument;
    }
    return ASOHCIContextBase::Initialize(pci, barIndex, ASContextKind::kIR_Receive, offs);
}

kern_return_t ASOHCIIRContext::Start()
{
    if (!_pci) return kIOReturnNotReady;
    
    // Clear run bit to ensure clean state
    WriteContextClear(kOHCI_ContextControl_run);
    
    // Configure channel filtering and sync matching before starting
    kern_return_t kr = ConfigureContextMatch(_channelFilter.singleChannel, 
                                           _channelFilter.tag, 
                                           _channelFilter.sync);
    if (kr != kIOReturnSuccess) {
        os_log(ASLog(), "IR%u: Failed to configure context match: 0x%x", _ctxIndex, kr);
        return kr;
    }
    
    // Set multi-channel mode if enabled
    if (_channelFilter.multiChannelMode && _ctxIndex == 0) {
        kr = SetMultiChannelMode(true, _channelFilter.channelMask);
        if (kr != kIOReturnSuccess) {
            os_log(ASLog(), "IR%u: Failed to set multi-channel mode: 0x%x", _ctxIndex, kr);
            return kr;
        }
    }
    
    os_log(ASLog(), "IR%u: Start deferred (will run on first enqueue)", _ctxIndex);
    return kIOReturnSuccess;
}

void ASOHCIIRContext::ApplyPolicy(const IRPolicy& policy)
{
    _policy = policy;
    os_log(ASLog(), "IR%u: Policy applied - dropOnOverrun=%s, watermark=%uμs", 
           _ctxIndex, policy.dropOnOverrun ? "true" : "false", policy.bufferWatermarkUs);
}

void ASOHCIIRContext::ApplyChannelFilter(const IRChannelFilter& filter)
{
    _channelFilter = filter;
    
    // If already started, reconfigure the match register
    if (IsRunning()) {
        ConfigureContextMatch(filter.singleChannel, filter.tag, filter.sync);
        if (filter.multiChannelMode && _ctxIndex == 0) {
            SetMultiChannelMode(true, filter.channelMask);
        }
    }
    
    os_log(ASLog(), "IR%u: Channel filter applied - channel=%u, tag=%u, sync=%u, multiCh=%s", 
           _ctxIndex, filter.singleChannel, filter.tag, filter.sync, 
           filter.multiChannelMode ? "true" : "false");
}

kern_return_t ASOHCIIRContext::EnqueueStandard(const IRDesc::Program& program,
                                              const IRQueueOptions& opts)
{
    if (!_pci) return kIOReturnNotReady;
    
    _currentMode = opts.receiveMode;
    _queueOptions = opts;
    
    // Configure buffer-fill mode bit if needed
    if (opts.receiveMode == IRMode::kBufferFill) {
        WriteContextSet(kOHCI_IR_BufferFill);
    } else {
        WriteContextClear(kOHCI_IR_BufferFill);
    }
    
    // Write CommandPtr to start reception
    kern_return_t kr = WriteCommandPtr(program.headPA >> 4, program.zHead);
    if (kr != kIOReturnSuccess) {
        os_log(ASLog(), "IR%u: Failed to write command pointer: 0x%x", _ctxIndex, kr);
        return kr;
    }
    
    // Memory barrier to ensure descriptor setup is complete
    OHCI_MEMORY_BARRIER();
    
    // Set run bit to enable context
    WriteContextSet(kOHCI_ContextControl_run);
    
    _outstanding++;
    
    os_log(ASLog(), "IR%u: Enqueued standard program - mode=%u, headPA=0x%x, Z=%u", 
           _ctxIndex, (uint32_t)opts.receiveMode, program.headPA, program.zHead);
    
    return kIOReturnSuccess;
}

kern_return_t ASOHCIIRContext::EnqueueDualBuffer(const IRProgram::DualBufferProgram& program,
                                                const IRQueueOptions& opts)
{
    if (!_pci) return kIOReturnNotReady;
    if (opts.receiveMode != IRMode::kDualBuffer) return kIOReturnBadArgument;
    
    _currentMode = opts.receiveMode;
    _queueOptions = opts;
    
    // Dual-buffer mode uses different descriptor format
    // Clear buffer-fill mode bit (dual-buffer is its own mode)
    WriteContextClear(kOHCI_IR_BufferFill);
    
    // Write CommandPtr to start reception
    kern_return_t kr = WriteCommandPtr(program.headPA >> 4, program.zHead);
    if (kr != kIOReturnSuccess) {
        os_log(ASLog(), "IR%u: Failed to write dual-buffer command pointer: 0x%x", _ctxIndex, kr);
        return kr;
    }
    
    // Memory barrier to ensure descriptor setup is complete
    OHCI_MEMORY_BARRIER();
    
    // Set run bit to enable context
    WriteContextSet(kOHCI_ContextControl_run);
    
    _outstanding++;
    
    os_log(ASLog(), "IR%u: Enqueued dual-buffer program - headPA=0x%x, Z=%u, firstSize=%u", 
           _ctxIndex, program.headPA, program.zHead, opts.firstSize);
    
    return kIOReturnSuccess;
}

void ASOHCIIRContext::OnInterruptRx()
{
    if (!_pci) return;
    
    // Process completed buffers and update statistics
    ProcessCompletedBuffers();
    
    // Check if we need to refill buffers
    if (NeedsRefill() && _policy.dropOnOverrun) {
        os_log(ASLog(), "IR%u: Buffer refill needed", _ctxIndex);
        RefillBuffers();
    }
    
    os_log(ASLog(), "IR%u: Interrupt processed - outstanding=%u", _ctxIndex, _outstanding);
}

void ASOHCIIRContext::SetCompletionCallback(void (*callback)(const IRCompletion&, void*), void* context)
{
    _completionCallback = callback;
    _completionContext = context;
}

kern_return_t ASOHCIIRContext::RefillBuffers()
{
    // TODO: Implement buffer refill logic
    // This would allocate new receive buffers and link them into the descriptor chain
    os_log(ASLog(), "IR%u: RefillBuffers placeholder", _ctxIndex);
    return kIOReturnSuccess;
}

bool ASOHCIIRContext::NeedsRefill() const
{
    // Simple heuristic: refill if we have fewer than 25% of buffers available
    uint32_t available = (_bufferTail <= _bufferHead) ? 
                        (kMaxReceiveBuffers - (_bufferHead - _bufferTail)) :
                        (_bufferTail - _bufferHead);
    
    return available < (kMaxReceiveBuffers / 4);
}

void ASOHCIIRContext::RecoverDeadContext()
{
    os_log(ASLog(), "IR%u: Recovering dead context", _ctxIndex);
    
    // Clear dead bit and stop context
    WriteContextClear(kOHCI_ContextControl_dead | kOHCI_ContextControl_run);
    
    // Update error statistics
    _stats.bufferOverruns++;
    
    // Clear outstanding count
    _outstanding = 0;
}

kern_return_t ASOHCIIRContext::ConfigureContextMatch(uint8_t channel, uint8_t tag, uint8_t sync)
{
    if (!_pci) return kIOReturnNotReady;
    
    // Calculate ContextMatch register offset (OHCI §10.3)
    uint32_t matchOffset = kOHCI_IsoRcvContextMatch(_ctxIndex);
    
    // Build match value: sync[11:8] | tag[7:6] | channel[5:0]
    uint32_t matchValue = ((sync & 0xF) << 8) | ((tag & 0x3) << 6) | (channel & 0x3F);
    
    _pci->MemoryWrite32(_bar, matchOffset, matchValue);
    
    return kIOReturnSuccess;
}

kern_return_t ASOHCIIRContext::SetMultiChannelMode(bool enable, uint64_t channelMask)
{
    if (!_pci) return kIOReturnNotReady;
    if (_ctxIndex != 0) return kIOReturnBadArgument; // Multi-channel only on context 0
    
    if (enable) {
        WriteContextSet(kOHCI_IR_MultiChannelMode);
        // TODO: Program the channel mask registers (implementation-specific)
        os_log(ASLog(), "IR%u: Multi-channel mode enabled, mask=0x%llx", _ctxIndex, channelMask);
    } else {
        WriteContextClear(kOHCI_IR_MultiChannelMode);
        os_log(ASLog(), "IR%u: Multi-channel mode disabled", _ctxIndex);
    }
    
    return kIOReturnSuccess;
}

ASOHCIIRContext::ReceiveBuffer* ASOHCIIRContext::GetNextFreeBuffer()
{
    if (_bufferRingFull) return nullptr;
    if (_bufferHead == _bufferTail && _buffers[_bufferHead].inUse) return nullptr;
    
    ReceiveBuffer* buffer = &_buffers[_bufferHead];
    if (!buffer->inUse) {
        return buffer;
    }
    
    return nullptr;
}

void ASOHCIIRContext::RetireBuffer(ReceiveBuffer* buffer)
{
    if (!buffer || !buffer->inUse) return;
    
    buffer->inUse = false;
    
    // Create completion info for callback
    if (_completionCallback) {
        IRCompletion completion{};
        completion.success = (buffer->status == kOHCI_EvtCode_NoStatus);
        completion.dataLength = buffer->size - buffer->resCount;
        completion.status = buffer->status;
        
        _completionCallback(completion, _completionContext);
    }
    
    // Update statistics
    if (buffer->status == kOHCI_EvtCode_NoStatus) {
        _stats.packetsReceived++;
        _stats.bytesReceived += (buffer->size - buffer->resCount);
        UpdateStatsOnPacket(IRCompletion{}); // Use default completion for now
    } else {
        UpdateStatsOnError(buffer->status);
    }
    
    // Advance tail pointer
    _bufferTail = (_bufferTail + 1) % kMaxReceiveBuffers;
    _bufferRingFull = false;
}

void ASOHCIIRContext::ProcessCompletedBuffers()
{
    // TODO: Read descriptor status and retire completed buffers
    // This would examine the descriptor completion status and call RetireBuffer()
    
    if (_outstanding > 0) {
        _outstanding--;
    }
}

void ASOHCIIRContext::UpdateStatsOnPacket(const IRCompletion& completion)
{
    _stats.packetsReceived++;
    _stats.bytesReceived += completion.dataLength;
}

void ASOHCIIRContext::UpdateStatsOnError(uint16_t status)
{
    switch (status) {
        case kOHCI_EvtCode_Overrun:
            _stats.bufferOverruns++;
            break;
        default:
            _stats.packetsDropped++;
            break;
    }
    
    if (_policy.enableErrorLogging) {
        os_log(ASLog(), "IR%u: Error status 0x%x", _ctxIndex, status);
    }
}

void ASOHCIIRContext::OnBusReset()
{
    // Clear context state on bus reset per OHCI §10.5
    // Stop context and clear any pending buffers
    if (IsRunning()) {
        // Set run bit to false to stop context
        WriteContextClear(kOHCI_ContextControl_run);
    }
    
    // Reset buffer ring tracking
    _bufferHead = 0;
    _bufferTail = 0;
    _bufferRingFull = false;
    
    // Clear completion callback state if set
    _completionCallback = nullptr;
    _completionContext = nullptr;
    
    os_log(ASLog(), "IRContext: ctx%u reset on bus reset", _ctxIndex);
}

void ASOHCIIRContext::ResetStats()
{
    // Reset all statistics counters
    _stats = {};
    os_log(ASLog(), "IRContext: ctx%u stats reset", _ctxIndex);
}