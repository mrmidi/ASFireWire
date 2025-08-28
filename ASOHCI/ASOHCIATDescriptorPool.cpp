//
// ASOHCIATDescriptorPool.cpp
// ASOHCI
//
// OHCI 1.1 AT Descriptor Pool Management Implementation
// Based on OHCI 1.1 Specification §7.1 (List management), §7.7 (Descriptor formats)
//

#include "ASOHCIATDescriptorPool.hpp"
#include "LogHelper.hpp"
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSCollections.h>

ASOHCIATDescriptorPool::~ASOHCIATDescriptorPool() { Deallocate(); }

kern_return_t ASOHCIATDescriptorPool::Initialize(IOPCIDevice* pciDevice, uint8_t barIndex, uint32_t poolSizeBytes)
{
    kern_return_t result = kIOReturnError;
    
    if (fInitialized) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Already initialized");
        return kIOReturnInvalid;
    }
    
    if (!pciDevice) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Invalid PCI device");
        return kIOReturnBadArgument;
    }
    
    // Validate pool size - must be multiple of descriptor alignment (16 bytes)
    if (poolSizeBytes == 0 || (poolSizeBytes % ATDesc::kDescriptorAlignBytes) != 0) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Pool size must be multiple of %u bytes", 
                    ATDesc::kDescriptorAlignBytes);
        return kIOReturnBadArgument;
    }
    
    fPCIDevice = pciDevice;
    fBARIndex = barIndex;
    fPoolSizeBytes = poolSizeBytes;
    fDescriptorCount = poolSizeBytes / sizeof(ATDesc::Descriptor);
    IOAddressSegment segment{};
    
    // Allocate DMA-coherent memory for descriptor pool
    // OHCI requires 32-bit addressable memory for descriptors (§7.1)
    result = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                             fPoolSizeBytes,
                                             ATDesc::kDescriptorAlignBytes,  // 16-byte alignment
                                             &fPoolMemory);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Failed to create buffer memory descriptor: 0x%x", result);
        goto cleanup;
    }
    
    // Map memory for CPU access
    result = fPoolMemory->CreateMapping(0, 0, 0, 0, 0, &fPoolMap);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Failed to map memory: 0x%x", result);
        goto cleanup;
    }
    
    // Get virtual address for CPU access
    fPoolVirtualAddress = reinterpret_cast<void*>(fPoolMap->GetAddress());
    if (!fPoolVirtualAddress) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Failed to get virtual address");
        result = kIOReturnNoMemory;
        goto cleanup;
    }
    
    // Get physical address for DMA operations
    result = fPoolMemory->GetAddressRange(&segment);
    if (result != kIOReturnSuccess) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Failed to get physical address: 0x%x", result);
        goto cleanup;
    }
    
    fPoolPhysicalAddress = segment.address;
    
    // Validate 32-bit addressability requirement (OHCI limitation)
    if (fPoolPhysicalAddress + fPoolSizeBytes > 0x100000000ULL) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Memory not 32-bit addressable (PA=0x%llx)", 
                    fPoolPhysicalAddress);
        result = kIOReturnNoMemory;
        goto cleanup;
    }
    
    // Initialize free block tracking
    fFreeSpanCount = 0;
    
    // Clear descriptor pool memory
    memset(fPoolVirtualAddress, 0, fPoolSizeBytes);
    
    // Initialize all blocks as free (single large span initially)
    if (fDescriptorCount > 0 && fFreeSpanCount < kMaxFreeSpans) {
        fFreeBlocks[0] = {0, fDescriptorCount};
        fFreeSpanCount = 1;
    }
    
    fInitialized = true;
    
    os_log(ASLog(), "ASOHCIATDescriptorPool: Initialized pool with %u descriptors (PA=0x%x, VA=%p)", 
           fDescriptorCount, (uint32_t)fPoolPhysicalAddress, fPoolVirtualAddress);
    
    return kIOReturnSuccess;
    
cleanup:
    Deallocate();
    return result;
}

kern_return_t ASOHCIATDescriptorPool::Deallocate()
{
    // Reset free span tracking
    fFreeSpanCount = 0;
    
    if (fPoolMap) {
        fPoolMap->release();
        fPoolMap = nullptr;
    }
    
    if (fPoolMemory) {
        fPoolMemory->release();
        fPoolMemory = nullptr;
    }
    
    fPoolVirtualAddress = nullptr;
    fPoolPhysicalAddress = 0;
    fPoolSizeBytes = 0;
    fDescriptorCount = 0;
    fPCIDevice = nullptr;
    fInitialized = false;
    
    return kIOReturnSuccess;
}

ASOHCIATDescriptorPool::Block ASOHCIATDescriptorPool::AllocateBlock(uint32_t descriptorCount)
{
    Block block = {};
    
    if (!fInitialized || descriptorCount == 0) {
        return block;
    }
    
    // Find suitable free block
    for (uint32_t i = 0; i < fFreeSpanCount; i++) {
        const FreeSpan& freeSpan = fFreeBlocks[i];
        if (freeSpan.descriptorCount >= descriptorCount) {
            // Found suitable block - allocate from it
            uint32_t allocOffset = freeSpan.offset;
            uint32_t allocPhysAddr = static_cast<uint32_t>(fPoolPhysicalAddress) + (allocOffset * sizeof(ATDesc::Descriptor));
            void* allocVirtAddr = static_cast<uint8_t*>(fPoolVirtualAddress) + (allocOffset * sizeof(ATDesc::Descriptor));
            
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
            
            // Update free block or remove if fully consumed
            if (freeSpan.descriptorCount > descriptorCount) {
                // Split span - update remaining free space
                fFreeBlocks[i].offset = freeSpan.offset + descriptorCount;
                fFreeBlocks[i].descriptorCount = freeSpan.descriptorCount - descriptorCount;
            } else {
                // Span fully consumed - remove from list
                for (uint32_t j = i + 1; j < fFreeSpanCount; j++) {
                    fFreeBlocks[j - 1] = fFreeBlocks[j];
                }
                if (fFreeSpanCount > 0) fFreeSpanCount--;
            }
            
            // Return allocated block
            block.physicalAddress = allocPhysAddr;
            block.virtualAddress = allocVirtAddr;
            block.descriptorCount = descriptorCount;
            block.zValue = zValue;
            block.valid = true;
            
            // Clear allocated descriptors
            memset(allocVirtAddr, 0, descriptorCount * sizeof(ATDesc::Descriptor));
            
            os_log(ASLog(), "ASOHCIATDescriptorPool: Allocated block with %u descriptors (PA=0x%x, Z=%u)", 
                        descriptorCount, allocPhysAddr, zValue);
            
            return block;
        }
    }
    
    os_log(ASLog(), "ASOHCIATDescriptorPool: No suitable free block for %u descriptors", descriptorCount);
    return block;
}

kern_return_t ASOHCIATDescriptorPool::FreeBlock(const Block& block)
{
    if (!fInitialized || !block.valid) {
        return kIOReturnBadArgument;
    }
    
    // Calculate offset from physical address
    uint32_t poolPhysStart = static_cast<uint32_t>(fPoolPhysicalAddress);
    if (block.physicalAddress < poolPhysStart || 
        block.physicalAddress >= poolPhysStart + fPoolSizeBytes) {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Invalid physical address 0x%x for free", 
                    block.physicalAddress);
        return kIOReturnBadArgument;
    }
    
    uint32_t offset = (block.physicalAddress - poolPhysStart) / sizeof(ATDesc::Descriptor);
    
    // Add block back to free list (simple implementation - could be optimized with coalescing)
    if (fFreeSpanCount < kMaxFreeSpans) {
        fFreeBlocks[fFreeSpanCount++] = {offset, block.descriptorCount};
    } else {
        os_log(ASLog(), "ASOHCIATDescriptorPool: Free list full, dropping span (offset=%u,count=%u)", offset, block.descriptorCount);
    }
    
    // Clear freed memory
    memset(block.virtualAddress, 0, block.descriptorCount * sizeof(ATDesc::Descriptor));
    
    os_log(ASLog(), "ASOHCIATDescriptorPool: Freed block with %u descriptors (PA=0x%x)", 
                block.descriptorCount, block.physicalAddress);
    
    return kIOReturnSuccess;
}

uint32_t ASOHCIATDescriptorPool::GetAvailableDescriptors() const
{
    if (!fInitialized) {
        return 0;
    }
    
    uint32_t available = 0;
    for (uint32_t i = 0; i < fFreeSpanCount; i++) available += fFreeBlocks[i].descriptorCount;
    
    return available;
}

uint32_t ASOHCIATDescriptorPool::GetTotalDescriptors() const
{
    return fDescriptorCount;
}

// IsInitialized() provided inline in header
