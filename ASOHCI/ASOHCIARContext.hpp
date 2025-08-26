//
//  ASOHCIARContext.hpp
//  ASOHCI
//
//  OHCI 1.1 Asynchronous Receive Context Implementation
//  Based on OHCI 1.1 Specification §8 (Asynchronous Receive DMA)
//

#ifndef ASOHCIARContext_hpp
#define ASOHCIARContext_hpp

#include <Availability.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>

#include "OHCIConstants.hpp"
#include "LogHelper.hpp"

// Forward declarations
class ASOHCI;

/**
 * ASOHCIARContext
 * 
 * Manages OHCI Asynchronous Receive (AR) DMA context for FireWire packet reception.
 * Implements buffer-fill mode where multiple packets are concatenated into supplied buffers.
 * 
 * Based on OHCI 1.1 §8 Asynchronous Receive DMA specification requirements.
 */
class ASOHCIARContext
{
public:
    /**
     * Context types for AR Request and AR Response contexts
     */
    enum ContextType {
        AR_REQUEST_CONTEXT  = 0,  // AR Request context (offset 0x200)
        AR_RESPONSE_CONTEXT = 1   // AR Response context (offset 0x220)  
    };

    // Constructor and Destructor
    ASOHCIARContext();
    virtual ~ASOHCIARContext();

    /**
     * Initialize the AR context
     * 
     * @param pciDevice     PCI device for memory operations
     * @param contextType   AR_REQUEST_CONTEXT or AR_RESPONSE_CONTEXT
     * @param bufferCount   Number of receive buffers to allocate
     * @param bufferSize    Size of each receive buffer in bytes
     * @return kIOReturnSuccess on success
     */
    virtual kern_return_t Initialize(IOPCIDevice* pciDevice,
                                     ContextType contextType,
                                     uint8_t barIndex,
                                     uint32_t bufferCount = 4,
                                     uint32_t bufferSize = 4096);

    /**
     * Start the AR context (set run bit and activate DMA)
     * 
     * @return kIOReturnSuccess on success
     */
    virtual kern_return_t Start();

    /**
     * Stop the AR context (clear run bit and wait for inactive)
     * 
     * @return kIOReturnSuccess on success  
     */
    virtual kern_return_t Stop();

    /**
     * Handle context interrupt (ARRQ or ARRS completion)
     * Called from main OHCI interrupt handler
     * 
     * @return kIOReturnSuccess on success
     */
    virtual kern_return_t HandleInterrupt();

    /**
     * Wake the context (signal new descriptors available)
     * Used when appending new buffers to active context
     * 
     * @return kIOReturnSuccess on success
     */
    virtual kern_return_t Wake();

    /**
     * Get current context status
     * 
     * @param outStatus     Current ContextControl register value
     * @return kIOReturnSuccess on success
     */
    virtual kern_return_t GetStatus(uint32_t* outStatus);

    /**
     * Check if context is currently active
     * 
     * @return true if context is running and active
     */
    virtual bool IsActive();

private:
    // Context configuration
    IOPCIDevice*                    fPCIDevice;
    ContextType                     fContextType;
    uint32_t                        fContextBaseOffset;
    uint32_t                        fContextControlSetOffset;
    uint32_t                        fContextControlClearOffset;
    uint32_t                        fCommandPtrOffset;
    uint8_t                         fBARIndex;

    // Buffer management
    uint32_t                        fBufferCount;
    uint32_t                        fBufferSize;
    IOBufferMemoryDescriptor**      fBufferDescriptors;
    IOMemoryMap**                   fBufferMaps;
    IODMACommand**                  fBufferDMA;         // DMA commands for buffers
    IOAddressSegment*               fBufferSegs;        // IOVA for each buffer

    // Descriptor chain management  
    IOBufferMemoryDescriptor*       fDescriptorChain;
    IOMemoryMap*                    fDescriptorMap;
    OHCI_ARInputMoreDescriptor*     fDescriptors;
    IODMACommand*                   fDescriptorDMA;     // DMA command for descriptor chain
    IOAddressSegment                fDescriptorSeg;     // IOVA for descriptor chain
    uint32_t                        fDescriptorCount;
    uint32_t                        fCurrentDescriptor;

    // Context state
    bool                            fInitialized;
    bool                            fRunning;

    // Helper methods
    kern_return_t                   AllocateBuffers();
    kern_return_t                   AllocateDescriptorChain();
    kern_return_t                   SetupDescriptorChain();
    kern_return_t                   FreeBuffers();
    kern_return_t                   FreeDescriptorChain();
    
    kern_return_t                   WriteContextControl(uint32_t value, bool setRegister);
    kern_return_t                   ReadContextControl(uint32_t* value);
    kern_return_t                   WriteCommandPtr(uint32_t descriptorAddress, uint32_t zValue);
    
    void                           SetContextOffsets(ContextType contextType);
    uint32_t                       DescriptorIndexToZ(uint32_t index);

    // Disable copy constructor and assignment operator
    ASOHCIARContext(const ASOHCIARContext&) = delete;
    ASOHCIARContext& operator=(const ASOHCIARContext&) = delete;
};

#endif /* ASOHCIARContext_hpp */
