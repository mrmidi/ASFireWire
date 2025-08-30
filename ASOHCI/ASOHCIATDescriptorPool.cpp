//
// ASOHCIATDescriptorPool.cpp
// ASOHCI
//
// OHCI 1.1 AT Descriptor Pool Management Implementation
// Based on Linux OHCI implementation - dynamic buffer allocation instead of large pre-allocated pools
// Based on OHCI 1.1 Specification §7.1 (List management), §7.7 (Descriptor formats)
//

#include "ASOHCIATDescriptorPool.hpp"
#include "LogHelper.hpp"
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSCollections.h>

ASOHCIATDescriptorPool::~ASOHCIATDescriptorPool() { Deallocate(); }

kern_return_t ASOHCIATDescriptorPool::Initialize(IOPCIDevice* pciDevice, uint8_t barIndex)
{
    kern_return_t result = kIOReturnError;
    
    os_log(ASLog(), "ASOHCIATDescriptorPool: Initialize called with dynamic allocation (Linux-style), barIndex=%u", barIndex);
    
    if (fInitialized) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Already initialized");
        return kIOReturnInvalid;
    }
    
    if (!pciDevice) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Invalid PCI device");
        return kIOReturnBadArgument;
    }
    
    fPCIDevice = pciDevice;
    fBARIndex = barIndex;
    fTotalAllocation = 0;
    fBufferList = nullptr;
    fCurrentBuffer = nullptr;
    
    // Start with one buffer (Linux approach)
    result = AddBuffer();
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Failed to add initial buffer: 0x%x", result);
        goto cleanup;
    }
    
    fInitialized = true;
    
    os_log(ASLog(), "ASOHCIATDescriptorPool: SUCCESS - Initialized with dynamic allocation");
    
    return kIOReturnSuccess;
    
cleanup:
    Deallocate();
    return result;
}

kern_return_t ASOHCIATDescriptorPool::AddBuffer()
{
    kern_return_t result;
    DescriptorBuffer* newBuffer;
    
    // Check allocation limit (Linux has 16MB limit)
    if (fTotalAllocation >= kMaxAllocation) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Allocation limit reached (%zu bytes)", kMaxAllocation);
        return kIOReturnNoMemory;
    }
    
    // Allocate new buffer structure
    newBuffer = new DescriptorBuffer();
    if (!newBuffer) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Failed to allocate buffer structure");
        return kIOReturnNoMemory;
    }
    
    // Allocate PAGE_SIZE buffer (Linux approach)
    result = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                             kPageSize,
                                             ATDesc::kDescriptorAlignBytes,  // 16-byte alignment
                                             &newBuffer->memory);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: IOBufferMemoryDescriptor::Create failed: 0x%x (size=%zu)", 
               result, kPageSize);
        delete newBuffer;
        return result;
    }
    
    // Map memory for CPU access
    result = newBuffer->memory->CreateMapping(0, 0, 0, 0, 0, &newBuffer->map);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Failed to map memory: 0x%x", result);
        newBuffer->memory->release();
        delete newBuffer;
        return result;
    }
    
    // Get virtual address
    newBuffer->virtualAddress = reinterpret_cast<void*>(newBuffer->map->GetAddress());
    if (!newBuffer->virtualAddress) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Failed to get virtual address");
        newBuffer->map->release();
        newBuffer->memory->release();
        delete newBuffer;
        return kIOReturnNoMemory;
    }
    
    // Get physical address
    IOAddressSegment segment{};
    result = newBuffer->memory->GetAddressRange(&segment);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Failed to get physical address: 0x%x", result);
        newBuffer->map->release();
        newBuffer->memory->release();
        delete newBuffer;
        return result;
    }
    
    newBuffer->physicalAddress = segment.address;
    newBuffer->bufferSize = kPageSize;
    newBuffer->used = 0;
    newBuffer->next = nullptr;
    
    // Validate 32-bit addressability (OHCI requirement)
    if (newBuffer->physicalAddress + kPageSize > 0x100000000ULL) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Memory not 32-bit addressable (PA=0x%llx)", 
               newBuffer->physicalAddress);
        newBuffer->map->release();
        newBuffer->memory->release();
        delete newBuffer;
        return kIOReturnNoMemory;
    }
    
    // Add to buffer list
    if (!fBufferList) {
        fBufferList = newBuffer;
    } else {
        // Find end of list
        DescriptorBuffer* last = fBufferList;
        while (last->next) {
            last = last->next;
        }
        last->next = newBuffer;
    }
    
    // Set as current buffer if we don't have one
    if (!fCurrentBuffer) {
        fCurrentBuffer = newBuffer;
    }
    
    fTotalAllocation += kPageSize;
    
    os_log(ASLog(), "ASOHCIATDescriptorPool: Added new buffer (PA=0x%llx, VA=%p, size=%zu)", 
           newBuffer->physicalAddress, newBuffer->virtualAddress, kPageSize);
    
    return kIOReturnSuccess;
}

ASOHCIATDescriptorPool::DescriptorBuffer* ASOHCIATDescriptorPool::FindBufferForAllocation(size_t neededSize)
{
    DescriptorBuffer* buffer = fCurrentBuffer;
    
    // Check current buffer first
    if (buffer && (buffer->bufferSize - buffer->used) >= neededSize) {
        return buffer;
    }
    
    // Search through all buffers
    buffer = fBufferList;
    while (buffer) {
        if ((buffer->bufferSize - buffer->used) >= neededSize) {
            fCurrentBuffer = buffer;  // Update current buffer
            return buffer;
        }
        buffer = buffer->next;
    }
    
    return nullptr;
}

kern_return_t ASOHCIATDescriptorPool::Deallocate()
{
    DescriptorBuffer* buffer = fBufferList;
    DescriptorBuffer* next;
    
    while (buffer) {
        next = buffer->next;
        
        if (buffer->map) {
            buffer->map->release();
        }
        
        if (buffer->memory) {
            buffer->memory->release();
        }
        
        delete buffer;
        buffer = next;
    }
    
    fBufferList = nullptr;
    fCurrentBuffer = nullptr;
    fTotalAllocation = 0;
    fPCIDevice = nullptr;
    fInitialized = false;
    
    return kIOReturnSuccess;
}

ASOHCIATDescriptorPool::Block ASOHCIATDescriptorPool::AllocateBlock(uint32_t descriptorCount)
{
    Block block = {};
    size_t neededSize = descriptorCount * sizeof(ATDesc::Descriptor);
    DescriptorBuffer* buffer;
    
    if (!fInitialized || descriptorCount == 0) {
        return block;
    }
    
    // Find a buffer with enough space
    buffer = FindBufferForAllocation(neededSize);
    if (!buffer) {
        // Try to add a new buffer
        kern_return_t result = AddBuffer();
        if (result != kIOReturnSuccess) {
            os_log(ASLog(), "ASOHCIATDescriptorPool: Failed to add buffer for allocation");
            return block;
        }
        buffer = fCurrentBuffer;
    }
    
    // Check if buffer has enough space (should be true after FindBufferForAllocation/AddBuffer)
    if ((buffer->bufferSize - buffer->used) < neededSize) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Buffer allocation error - insufficient space");
        return block;
    }
    
    // Allocate from buffer
    uint32_t allocOffset = static_cast<uint32_t>(buffer->used);
    uint32_t allocPhysAddr = static_cast<uint32_t>(buffer->physicalAddress) + allocOffset;
    void* allocVirtAddr = static_cast<uint8_t*>(buffer->virtualAddress) + allocOffset;
    
    // Calculate Z nibble (OHCI §7.1: descriptor count encoding)
    uint8_t zValue;
    if (descriptorCount == 0) {
        zValue = 0;  // End of list
    } else if (descriptorCount >= 2 && descriptorCount <= 8) {
        zValue = static_cast<uint8_t>(descriptorCount);  // Valid descriptor block
    } else {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Invalid descriptor count %u for Z nibble", descriptorCount);
        return block;
    }
    
    // Update buffer usage
    buffer->used += neededSize;
    
    // Return allocated block
    block.physicalAddress = allocPhysAddr;
    block.virtualAddress = allocVirtAddr;
    block.descriptorCount = descriptorCount;
    block.zValue = zValue;
    block.valid = true;
    
    // Clear allocated descriptors
    memset(allocVirtAddr, 0, neededSize);
    
    os_log(ASLog(), "ASOHCIATDescriptorPool: Allocated block with %u descriptors (PA=0x%x, Z=%u) from buffer at 0x%llx", 
           descriptorCount, allocPhysAddr, zValue, buffer->physicalAddress);
    
    return block;
}

kern_return_t ASOHCIATDescriptorPool::FreeBlock(const Block& block)
{
    if (!fInitialized || !block.valid) {
        return kIOReturnBadArgument;
    }
    
    // In Linux-style dynamic allocation, we don't actually free individual blocks
    // within a buffer. The buffer remains allocated until the pool is destroyed.
    // This is acceptable since OHCI descriptors are typically used for the
    // lifetime of the context.
    
    os_log(ASLog(), "ASOHCIATDescriptorPool: FreeBlock called - block remains allocated (Linux-style)");
    
    return kIOReturnSuccess;
}

uint32_t ASOHCIATDescriptorPool::GetAvailableDescriptors() const
{
    if (!fInitialized) {
        return 0;
    }
    
    uint32_t available = 0;
    DescriptorBuffer* buffer = fBufferList;
    
    while (buffer) {
        size_t remaining = buffer->bufferSize - buffer->used;
        available += remaining / sizeof(ATDesc::Descriptor);
        buffer = buffer->next;
    }
    
    return available;
}

uint32_t ASOHCIATDescriptorPool::GetTotalDescriptors() const
{
    if (!fInitialized) {
        return 0;
    }
    
    return fTotalAllocation / sizeof(ATDesc::Descriptor);
}
