//
//  ASOHCIARContext.cpp
//  ASOHCI
//
//  OHCI 1.1 Asynchronous Receive Context Implementation
//

#include "ASOHCIARContext.hpp"
#include <os/log.h>
#include <DriverKit/IOLib.h>

// Logging helper
#include "LogHelper.hpp"

// Constructor
ASOHCIARContext::ASOHCIARContext() :
    fPCIDevice(nullptr),
    fContextType(AR_REQUEST_CONTEXT),
    fContextBaseOffset(0),
    fContextControlSetOffset(0), 
    fContextControlClearOffset(0),
    fCommandPtrOffset(0),
    fBufferCount(0),
    fBufferSize(0),
    fBufferDescriptors(nullptr),
    fBufferMaps(nullptr),
    fDescriptorChain(nullptr),
    fDescriptorMap(nullptr),
    fDescriptors(nullptr),
    fDescriptorCount(0),
    fCurrentDescriptor(0),
    fInitialized(false),
    fRunning(false)
{
}

// Destructor  
ASOHCIARContext::~ASOHCIARContext()
{
    if (fRunning) {
        Stop();
    }
    
    FreeDescriptorChain();
    FreeBuffers();
}

// Initialize the AR context
kern_return_t ASOHCIARContext::Initialize(IOPCIDevice* pciDevice, 
                                        ContextType contextType,
                                        uint32_t bufferCount,
                                        uint32_t bufferSize)
{
    kern_return_t result;
    
    if (fInitialized) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Already initialized");
        return kIOReturnError;
    }
    
    if (!pciDevice) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Invalid PCI device");
        return kIOReturnBadArgument;
    }
    
    // Validate buffer parameters
    if (bufferCount < 2 || bufferCount > 32) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Invalid buffer count %u (must be 2-32)", bufferCount);
        return kIOReturnBadArgument;
    }
    
    if (bufferSize < 1024 || bufferSize > 65536 || (bufferSize % 4) != 0) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Invalid buffer size %u (must be 1024-65536, quadlet-aligned)", bufferSize);
        return kIOReturnBadArgument;
    }
    
    fPCIDevice = pciDevice;
    fPCIDevice->retain();
    
    fContextType = contextType;
    fBufferCount = bufferCount;
    fBufferSize = bufferSize;
    fDescriptorCount = bufferCount;  // One descriptor per buffer
    
    // Set context register offsets based on type
    SetContextOffsets(contextType);
    
    // Allocate receive buffers
    result = AllocateBuffers();
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to allocate buffers: 0x%x", result);
        goto error;
    }
    
    // Allocate and setup descriptor chain
    result = AllocateDescriptorChain();
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to allocate descriptor chain: 0x%x", result);
        goto error;
    }
    
    result = SetupDescriptorChain();
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to setup descriptor chain: 0x%x", result);
        goto error;
    }
    
    fInitialized = true;
    
    os_log(ASLog(), "ASOHCIARContext: Initialized %s context with %u buffers of %u bytes", 
                (contextType == AR_REQUEST_CONTEXT) ? "Request" : "Response",
                bufferCount, bufferSize);
    
    return kIOReturnSuccess;
    
error:
    FreeDescriptorChain();
    FreeBuffers();
    if (fPCIDevice) {
        fPCIDevice->release();
        fPCIDevice = nullptr;
    }
    return result;
}

// Start the AR context
kern_return_t ASOHCIARContext::Start()
{
    kern_return_t result;
    uint32_t contextControl;
    
    if (!fInitialized) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Not initialized");
        return kIOReturnError;
    }
    
    if (fRunning) {
        os_log(ASLog(), "ASOHCIARContext: Already running");
        return kIOReturnSuccess;
    }
    
    // Verify context is not active
    result = ReadContextControl(&contextControl);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to read context control: 0x%x", result);
        return result;
    }
    
    if (contextControl & (kOHCI_ContextControl_run | kOHCI_ContextControl_active)) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Context already running/active: 0x%x", contextControl);
        return kIOReturnError;
    }
    
    // Write CommandPtr register with first descriptor address
    // For DriverKit, we use the virtual address since DMA is handled by the system
    uint64_t descriptorVirtAddr = fDescriptorMap->GetAddress();
    if (!descriptorVirtAddr) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to get descriptor virtual address");
        return kIOReturnError;
    }
    
    result = WriteCommandPtr((uint32_t)(descriptorVirtAddr >> 4), fDescriptorCount);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to write command pointer: 0x%x", result);
        return result;
    }
    
    // Set run bit to start context
    result = WriteContextControl(kOHCI_ContextControl_run, true);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to set run bit: 0x%x", result);
        return result;
    }
    
    fRunning = true;
    
    os_log(ASLog(), "ASOHCIARContext: Started %s context", 
                (fContextType == AR_REQUEST_CONTEXT) ? "Request" : "Response");
    
    return kIOReturnSuccess;
}

// Stop the AR context
kern_return_t ASOHCIARContext::Stop()
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
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to clear run bit: 0x%x", result);
        return result;
    }
    
    // Wait for context to become inactive (OHCI 1.1 §3.1.1.1)
    while (retries-- > 0) {
        result = ReadContextControl(&contextControl);
        if (result != kIOReturnSuccess) {
            os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to read context control: 0x%x", result);
            return result;
        }
        
        if (!(contextControl & kOHCI_ContextControl_active)) {
            break;  // Context is now inactive
        }
        
        IOSleep(1);  // Wait 1ms
    }
    
    if (contextControl & kOHCI_ContextControl_active) {
        os_log(ASLog(), "ASOHCIARContext: Context failed to stop (still active): 0x%x", contextControl);
        return kIOReturnTimeout;
    }
    
    fRunning = false;
    
    os_log(ASLog(), "ASOHCIARContext: Stopped %s context", 
                (fContextType == AR_REQUEST_CONTEXT) ? "Request" : "Response");
    
    return kIOReturnSuccess;
}

// Handle context interrupt
kern_return_t ASOHCIARContext::HandleInterrupt()
{
    // TODO: Implement interrupt handling for completed buffers
    // This will process completed descriptors and prepare new ones
    os_log(ASLog(), "ASOHCIARContext: Interrupt handled for %s context",
                 (fContextType == AR_REQUEST_CONTEXT) ? "Request" : "Response");
    return kIOReturnSuccess;
}

// Wake the context
kern_return_t ASOHCIARContext::Wake()
{
    if (!fRunning) {
        return kIOReturnError;
    }
    
    return WriteContextControl(kOHCI_ContextControl_wake, true);
}

// Get current context status
kern_return_t ASOHCIARContext::GetStatus(uint32_t* outStatus)
{
    if (!outStatus) {
        return kIOReturnBadArgument;
    }
    
    return ReadContextControl(outStatus);
}

// Check if context is active
bool ASOHCIARContext::IsActive()
{
    uint32_t status = 0;
    if (ReadContextControl(&status) == kIOReturnSuccess) {
        return (status & kOHCI_ContextControl_active) != 0;
    }
    return false;
}

#pragma mark - Private Methods

// Set context register offsets based on type
void ASOHCIARContext::SetContextOffsets(ContextType contextType)
{
    if (contextType == AR_REQUEST_CONTEXT) {
        fContextBaseOffset = kOHCI_AsReqRcvContextBase;
        fContextControlSetOffset = kOHCI_AsReqRcvContextControlS;
        fContextControlClearOffset = kOHCI_AsReqRcvContextControlC;
        fCommandPtrOffset = kOHCI_AsReqRcvCommandPtr;
    } else {
        fContextBaseOffset = kOHCI_AsRspRcvContextBase;
        fContextControlSetOffset = kOHCI_AsRspRcvContextControlS;
        fContextControlClearOffset = kOHCI_AsRspRcvContextControlC;
        fCommandPtrOffset = kOHCI_AsRspRcvCommandPtr;
    }
}

// Allocate receive buffers
kern_return_t ASOHCIARContext::AllocateBuffers()
{
    kern_return_t result;
    
    fBufferDescriptors = new IOBufferMemoryDescriptor*[fBufferCount];
    fBufferMaps = new IOMemoryMap*[fBufferCount];
    
    if (!fBufferDescriptors || !fBufferMaps) {
        return kIOReturnNoMemory;
    }
    
    // Initialize arrays
    for (uint32_t i = 0; i < fBufferCount; i++) {
        fBufferDescriptors[i] = nullptr;
        fBufferMaps[i] = nullptr;
    }
    
    // Allocate each buffer
    for (uint32_t i = 0; i < fBufferCount; i++) {
        result = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                                 fBufferSize,
                                                 4, // quadlet alignment
                                                 &fBufferDescriptors[i]);
        if (result != kIOReturnSuccess) {
            os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to create buffer descriptor %u: 0x%x", i, result);
            return result;
        }
        
        result = fBufferDescriptors[i]->CreateMapping(0, 0, 0, 0, 0, &fBufferMaps[i]);
        if (result != kIOReturnSuccess) {
            os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to map buffer %u: 0x%x", i, result);
            return result;
        }
    }
    
    return kIOReturnSuccess;
}

// Allocate descriptor chain
kern_return_t ASOHCIARContext::AllocateDescriptorChain()
{
    kern_return_t result;
    size_t chainSize = fDescriptorCount * sizeof(OHCI_ARInputMoreDescriptor);
    
    result = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                             chainSize,
                                             kOHCI_DescriptorAlign,
                                             &fDescriptorChain);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to create descriptor chain: 0x%x", result);
        return result;
    }
    
    result = fDescriptorChain->CreateMapping(0, 0, 0, 0, 0, &fDescriptorMap);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to map descriptor chain: 0x%x", result);
        return result;
    }
    
    fDescriptors = (OHCI_ARInputMoreDescriptor*)fDescriptorMap->GetAddress();
    if (!fDescriptors) {
        os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to get descriptor chain address");
        return kIOReturnError;
    }
    
    return kIOReturnSuccess;
}

// Setup descriptor chain
kern_return_t ASOHCIARContext::SetupDescriptorChain()
{
    kern_return_t result;
    
    // Initialize each descriptor in the chain
    for (uint32_t i = 0; i < fDescriptorCount; i++) {
        OHCI_ARInputMoreDescriptor* desc = &fDescriptors[i];
        
        // Clear descriptor
        memset(desc, 0, sizeof(*desc));
        
        // Set command fields per OHCI 1.1 §8.1.1
        desc->cmd = 0x2;        // INPUT_MORE command
        desc->key = 0x0;        // Must be 0 for AR
        desc->i = 0x3;          // Generate interrupt on completion
        desc->b = 0x3;          // Branch control (must be 0x3)
        desc->reqCount = fBufferSize;
        
        // Get buffer virtual address (DriverKit handles DMA translation)
        uint64_t bufferVirtAddr = fBufferMaps[i]->GetAddress();
        if (!bufferVirtAddr) {
            os_log(ASLog(), "ASOHCIARContext: ERROR: Failed to get buffer %u virtual address", i);
            return kIOReturnError;
        }
        desc->dataAddress = (uint32_t)bufferVirtAddr;
        
        // Set branch address and Z value
        if (i < fDescriptorCount - 1) {
            // Point to next descriptor
            uint64_t nextDescVirtAddr = fDescriptorMap->GetAddress() + (i + 1) * sizeof(*desc);
            desc->branchAddress = (uint32_t)(nextDescVirtAddr >> 4); // 16-byte aligned
            desc->Z = 1;  // Next block has 1 descriptor
        } else {
            // Last descriptor - end of chain
            desc->branchAddress = 0;
            desc->Z = 0;  // End of program
        }
        
        // Initialize status fields (will be updated by hardware)
        desc->resCount = fBufferSize;  // Initially no data received
        desc->xferStatus = 0;
    }
    
    return kIOReturnSuccess;
}

// Free buffers
kern_return_t ASOHCIARContext::FreeBuffers()
{
    if (fBufferMaps) {
        for (uint32_t i = 0; i < fBufferCount; i++) {
            if (fBufferMaps[i]) {
                fBufferMaps[i]->release();
            }
        }
        delete[] fBufferMaps;
        fBufferMaps = nullptr;
    }
    
    if (fBufferDescriptors) {
        for (uint32_t i = 0; i < fBufferCount; i++) {
            if (fBufferDescriptors[i]) {
                fBufferDescriptors[i]->release();
            }
        }
        delete[] fBufferDescriptors;
        fBufferDescriptors = nullptr;
    }
    
    return kIOReturnSuccess;
}

// Free descriptor chain
kern_return_t ASOHCIARContext::FreeDescriptorChain()
{
    if (fDescriptorMap) {
        fDescriptorMap->release();
        fDescriptorMap = nullptr;
    }
    
    if (fDescriptorChain) {
        fDescriptorChain->release();
        fDescriptorChain = nullptr;
    }
    
    fDescriptors = nullptr;
    return kIOReturnSuccess;
}

// Write to context control register
kern_return_t ASOHCIARContext::WriteContextControl(uint32_t value, bool setRegister)
{
    uint32_t offset = setRegister ? fContextControlSetOffset : fContextControlClearOffset;
    fPCIDevice->MemoryWrite32(0, offset, value);
    return kIOReturnSuccess;
}

// Read context control register
kern_return_t ASOHCIARContext::ReadContextControl(uint32_t* value)
{
    if (!value) {
        return kIOReturnBadArgument;
    }
    fPCIDevice->MemoryRead32(0, fContextControlSetOffset, value);
    return kIOReturnSuccess;
}

// Write command pointer register
kern_return_t ASOHCIARContext::WriteCommandPtr(uint32_t descriptorAddress, uint32_t zValue)
{
    uint32_t commandPtr = (descriptorAddress << 4) | (zValue & 0xF);
    fPCIDevice->MemoryWrite32(0, fCommandPtrOffset, commandPtr);
    return kIOReturnSuccess;
}