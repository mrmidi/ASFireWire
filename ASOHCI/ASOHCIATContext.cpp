//
//  ASOHCIATContext.cpp
//  ASOHCI
//
//  OHCI 1.1 Asynchronous Transmit Context Implementation
//

#include "ASOHCIATContext.hpp"
#include <os/log.h>
#include <DriverKit/IOLib.h>

#include "LogHelper.hpp"

// Constructor
ASOHCIATContext::ASOHCIATContext() :
    fPCIDevice(nullptr),
    fContextType(AT_REQUEST_CONTEXT),
    fBARIndex(0),
    fContextBaseOffset(0),
    fContextControlSetOffset(0),
    fContextControlClearOffset(0),
    fCommandPtrOffset(0),
    fDescriptorPool(nullptr),
    fDescriptorPoolMap(nullptr),
    fDescriptorPoolAddress(nullptr),
    fDescriptorPoolSize(0),
    fInitialized(false),
    fRunning(false)
{
}

// Destructor
ASOHCIATContext::~ASOHCIATContext()
{
    if (fRunning) {
        Stop();
    }
    
    FreeDescriptorPool();
    
    if (fPCIDevice) {
        fPCIDevice->release();
        fPCIDevice = nullptr;
    }
}

// Initialize the AT context

kern_return_t ASOHCIATContext::Initialize(IOPCIDevice* pciDevice, ContextType contextType, uint8_t barIndex)
{
    if (fInitialized) {
        return kIOReturnSuccess;
    }

    fPCIDevice = pciDevice;
    if (fPCIDevice) {
        fPCIDevice->retain();
    }
    fContextType = contextType;
    fBARIndex = barIndex;
    SetContextOffsets(contextType);

    // Future: Allocate descriptor pool
    // kern_return_t kr = AllocateDescriptorPool();
    // if (kr != kIOReturnSuccess) {
    //     return kr;
    // }

    fInitialized = true;
    return kIOReturnSuccess;
}

// Start the AT context
kern_return_t ASOHCIATContext::Start()
{
    kern_return_t result;
    uint32_t contextControl;
    
    if (!fInitialized) {
        os_log(ASLog(), "ASOHCIATContext: Not initialized");
        return kIOReturnError;
    }
    
    if (fRunning) {
        os_log(ASLog(), "ASOHCIATContext: Already running");
        return kIOReturnSuccess;
    }
    
    // Verify context is not active
    result = ReadContextSet();
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATContext: Failed to read context control: 0x%x", result);
        return result;
    }
    
    if (contextControl & (kOHCI_ContextControl_run | kOHCI_ContextControl_active)) {
        os_log(ASLog(), "ASOHCIATContext: Context already running/active: 0x%x", contextControl);
        return kIOReturnError;
    }
    
    // Initialize CommandPtr to empty program (Z=0)
    result = WriteCommandPtr(0, 0);  // No descriptors initially
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATContext: Failed to write command pointer: 0x%x", result);
        return result;
    }
    
    // Set run bit to enable context (but it will be inactive until packets are queued)
    result = WriteContextControl(kOHCI_ContextControl_run, true);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATContext: Failed to set run bit: 0x%x", result);
        return result;
    }
    
    fRunning = true;
    
    os_log(ASLog(), "ASOHCIATContext: Started %s context", 
                (fContextType == AT_REQUEST_CONTEXT) ? "Request" : "Response");
    
    return kIOReturnSuccess;
}

// Stop the AT context
kern_return_t ASOHCIATContext::Stop()
{
    kern_return_t result;
    uint32_t contextControl;
    int retries = 100;
    
    if (!fRunning) {
        return kIOReturnSuccess;
    }
    
    // Clear run bit
    result = WriteContextControl(kOHCI_ContextControl_run, false);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATContext: Failed to clear run bit: 0x%x", result);
        return result;
    }
    
    // Wait for context to become inactive (OHCI 1.1 §3.1.1.1)
    while (retries-- > 0) {
    result = ReadContextSet();
        if (result != kIOReturnSuccess) {
            os_log(ASLog(), "ASOHCIATContext: Failed to read context control: 0x%x", result);
            return result;
        }
        
        if (!(contextControl & kOHCI_ContextControl_active)) {
            break;  // Context is now inactive
        }
        
        IOSleep(1);  // Wait 1ms
    }
    
    if (contextControl & kOHCI_ContextControl_active) {
        os_log(ASLog(), "ASOHCIATContext: Context failed to stop (still active): 0x%x", contextControl);
        return kIOReturnTimeout;
    }
    
    fRunning = false;
    
    os_log(ASLog(), "ASOHCIATContext: Stopped %s context", 
                (fContextType == AT_REQUEST_CONTEXT) ? "Request" : "Response");
    
    return kIOReturnSuccess;
}

// Handle context interrupt
kern_return_t ASOHCIATContext::HandleInterrupt()
{
    // TODO: Implement interrupt handling for completed transmissions
    // This will process completed descriptors and handle acknowledgments
    os_log(ASLog(), "ASOHCIATContext: Interrupt handled for %s context",
                 (fContextType == AT_REQUEST_CONTEXT) ? "Request" : "Response");
    return kIOReturnSuccess;
}

// Wake the context
kern_return_t ASOHCIATContext::Wake()
{
    if (!fRunning) {
        return kIOReturnError;
    }
    
    return WriteContextControl(kOHCI_ContextControl_wake, true);
}

// Get current context status
kern_return_t ASOHCIATContext::GetStatus(uint32_t* outStatus)
{
    if (!outStatus) {
        return kIOReturnBadArgument;
    }
    
    return ReadContextSet();
}

// Check if context is active
bool ASOHCIATContext::IsActive()
{
    uint32_t status = 0;
    if (ReadContextSet() == kIOReturnSuccess) {
        return (status & kOHCI_ContextControl_active) != 0;
    }
    return false;
}

// Queue a packet for transmission (placeholder)
kern_return_t ASOHCIATContext::QueuePacket(PacketType packetType,
                                          const uint32_t* headerData,
                                          uint32_t headerSize,
                                          const void* payloadData,
                                          uint32_t payloadSize)
{
    if (!fInitialized) {
        return kIOReturnError;
    }
    
    // TODO: Implement packet queueing with descriptor block creation
    // This is a future implementation that will:
    // 1. Validate packet parameters
    // 2. Build descriptor block using OUTPUT_MORE*/OUTPUT_LAST* descriptors
    // 3. Link to existing context program
    // 4. Wake context to process new packet
    
    os_log(ASLog(), "ASOHCIATContext: QueuePacket placeholder - type=%u, headerSize=%u, payloadSize=%u", 
                 packetType, headerSize, payloadSize);
    
    return kIOReturnUnsupported;  // Not implemented yet
}

#pragma mark - Private Methods

// Set context register offsets based on type
void ASOHCIATContext::SetContextOffsets(ContextType contextType)
{
    if (contextType == AT_REQUEST_CONTEXT) {
        fContextBaseOffset = kOHCI_AsReqTrContextBase;
        fContextControlSetOffset = kOHCI_AsReqTrContextControlS;
        fContextControlClearOffset = kOHCI_AsReqTrContextControlC;
        fCommandPtrOffset = kOHCI_AsReqTrCommandPtr;
    } else {
        fContextBaseOffset = kOHCI_AsRspTrContextBase;
        fContextControlSetOffset = kOHCI_AsRspTrContextControlS;
        fContextControlClearOffset = kOHCI_AsRspTrContextControlC;
        fCommandPtrOffset = kOHCI_AsRspTrCommandPtr;
    }
}

// Allocate descriptor pool
kern_return_t ASOHCIATContext::AllocateDescriptorPool()
{
    kern_return_t result;
    
    // Allocate pool for descriptor blocks (future implementation)
    // For now, allocate a minimal pool to establish the infrastructure
    fDescriptorPoolSize = 4096;  // 4KB pool for descriptor blocks
    
    result = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                             fDescriptorPoolSize,
                                             kOHCI_DescriptorAlign,
                                             &fDescriptorPool);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATContext: Failed to create descriptor pool: 0x%x", result);
        return result;
    }
    
    result = fDescriptorPool->CreateMapping(0, 0, 0, 0, 0, &fDescriptorPoolMap);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATContext: Failed to map descriptor pool: 0x%x", result);
        return result;
    }
    
    fDescriptorPoolAddress = (void*)fDescriptorPoolMap->GetAddress();
    if (!fDescriptorPoolAddress) {
        os_log(ASLog(), "ASOHCIATContext: Failed to get descriptor pool address");
        return kIOReturnError;
    }
    
    // Clear the descriptor pool
    memset(fDescriptorPoolAddress, 0, fDescriptorPoolSize);
    
    return kIOReturnSuccess;
}

// Free descriptor pool
kern_return_t ASOHCIATContext::FreeDescriptorPool()
{
    if (fDescriptorPoolMap) {
        fDescriptorPoolMap->release();
        fDescriptorPoolMap = nullptr;
    }
    
    if (fDescriptorPool) {
        fDescriptorPool->release();
        fDescriptorPool = nullptr;
    }
    
    fDescriptorPoolAddress = nullptr;
    fDescriptorPoolSize = 0;
    
    return kIOReturnSuccess;
}

// Write to context control register
kern_return_t ASOHCIATContext::WriteContextControl(uint32_t value, bool setRegister)
{
    uint32_t offset = setRegister ? fContextControlSetOffset : fContextControlClearOffset;
    fPCIDevice->MemoryWrite32(fBARIndex, offset, value);
    return kIOReturnSuccess;
}

// Read context control register
kern_return_t ASOHCIATContext::ReadContextControl(uint32_t* value)
{
    if (!value) {
        return kIOReturnBadArgument;
    }
    fPCIDevice->MemoryRead32(fBARIndex, fContextControlSetOffset, value);
    return kIOReturnSuccess;
}

// Write command pointer register
kern_return_t ASOHCIATContext::WriteCommandPtr(uint32_t descriptorAddress, uint32_t zValue)
{
    uint32_t commandPtr = (descriptorAddress << 4) | (zValue & 0xF);
    fPCIDevice->MemoryWrite32(fBARIndex, fCommandPtrOffset, commandPtr);
    return kIOReturnSuccess;
}

// Build descriptor block (future implementation)
kern_return_t ASOHCIATContext::BuildDescriptorBlock(PacketType packetType,
                                                   const uint32_t* headerData,
                                                   uint32_t headerSize,
                                                   const void* payloadData,
                                                   uint32_t payloadSize)
{
    // TODO: Implement descriptor block building per OHCI 1.1 §7.1
    // This will create OUTPUT_MORE*/OUTPUT_LAST* descriptor sequences
    return kIOReturnUnsupported;
}