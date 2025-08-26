//
//  ASOHCIATContext.hpp
//  ASOHCI
//
//  OHCI 1.1 Asynchronous Transmit Context Implementation
//  Based on OHCI 1.1 Specification §7 (Asynchronous Transmit DMA)
//

#ifndef ASOHCIATContext_hpp
#define ASOHCIATContext_hpp

#include <Availability.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <PCIDriverKit/IOPCIDevice.h>

#include "OHCIConstants.hpp"
#include "LogHelper.hpp"

// Forward declarations
class ASOHCI;

/**
 * ASOHCIATContext
 * 
 * Manages OHCI Asynchronous Transmit (AT) DMA context for FireWire packet transmission.
 * Handles descriptor block management and packet assembly for AT Request and AT Response contexts.
 * 
 * Based on OHCI 1.1 §7 Asynchronous Transmit DMA specification requirements.
 */
class ASOHCIATContext
{
public:
    /**
     * Context types for AT Request and AT Response contexts
     */
    enum ContextType {
        AT_REQUEST_CONTEXT  = 0,  // AT Request context (offset 0x180)
        AT_RESPONSE_CONTEXT = 1   // AT Response context (offset 0x1A0)
    };

    /**
     * Packet types supported by AT contexts
     */
    enum PacketType {
        PACKET_REQUEST_READ,
        PACKET_REQUEST_WRITE,
        PACKET_REQUEST_LOCK,
        PACKET_RESPONSE_READ,
        PACKET_RESPONSE_WRITE,
        PACKET_RESPONSE_LOCK
    };

    // Constructor and Destructor
    ASOHCIATContext();
    virtual ~ASOHCIATContext();

    /**
     * Initialize the AT context
     * 
     * @param pciDevice     PCI device for memory operations
     * @param contextType   AT_REQUEST_CONTEXT or AT_RESPONSE_CONTEXT
     * @return kIOReturnSuccess on success
     */
    virtual kern_return_t Initialize(IOPCIDevice* pciDevice, ContextType contextType);

    /**
     * Start the AT context (set run bit and activate DMA)
     * 
     * @return kIOReturnSuccess on success
     */
    virtual kern_return_t Start();

    /**
     * Stop the AT context (clear run bit and wait for inactive)
     * 
     * @return kIOReturnSuccess on success
     */
    virtual kern_return_t Stop();

    /**
     * Handle context interrupt (request/response TX complete)
     * Called from main OHCI interrupt handler
     * 
     * @return kIOReturnSuccess on success
     */
    virtual kern_return_t HandleInterrupt();

    /**
     * Wake the context (signal new descriptors available)
     * Used when appending new packets to active context
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

    /**
     * Queue a packet for transmission (future implementation)
     * 
     * @param packetType    Type of packet to transmit
     * @param headerData    IEEE 1394 packet header quadlets
     * @param headerSize    Size of header in bytes (8, 12, or 16)
     * @param payloadData   Optional payload data
     * @param payloadSize   Size of payload in bytes
     * @return kIOReturnSuccess on success
     */
    virtual kern_return_t QueuePacket(PacketType packetType,
                                    const uint32_t* headerData,
                                    uint32_t headerSize,
                                    const void* payloadData = nullptr,
                                    uint32_t payloadSize = 0);

private:
    // Context configuration
    IOPCIDevice*                    fPCIDevice;
    ContextType                     fContextType;
    uint32_t                        fContextBaseOffset;
    uint32_t                        fContextControlSetOffset;
    uint32_t                        fContextControlClearOffset;
    uint32_t                        fCommandPtrOffset;

    // Descriptor management (future implementation)
    IOBufferMemoryDescriptor*       fDescriptorPool;
    IOMemoryMap*                    fDescriptorPoolMap;
    void*                           fDescriptorPoolAddress;
    uint32_t                        fDescriptorPoolSize;
    
    // Context state
    bool                            fInitialized;
    bool                            fRunning;

    // Helper methods
    kern_return_t                   AllocateDescriptorPool();
    kern_return_t                   FreeDescriptorPool();
    
    kern_return_t                   WriteContextControl(uint32_t value, bool setRegister);
    kern_return_t                   ReadContextControl(uint32_t* value);
    kern_return_t                   WriteCommandPtr(uint32_t descriptorAddress, uint32_t zValue);
    
    void                           SetContextOffsets(ContextType contextType);

    // Future implementation methods
    kern_return_t                   BuildDescriptorBlock(PacketType packetType,
                                                        const uint32_t* headerData,
                                                        uint32_t headerSize,
                                                        const void* payloadData,
                                                        uint32_t payloadSize);

    // Disable copy constructor and assignment operator
    ASOHCIATContext(const ASOHCIATContext&) = delete;
    ASOHCIATContext& operator=(const ASOHCIATContext&) = delete;
};

#endif /* ASOHCIATContext_hpp */