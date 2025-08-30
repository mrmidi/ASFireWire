//#pragma once
//
// ASOHCIATDescriptorPool.hpp
// Manages a pool of OUTPUT_* descriptors (DMA-coherent), hands out 16B-aligned blocks.
//
// Spec refs: OHCI 1.1 §7.1 (CommandPtr & Z nibble), §7.6 (prefetch/pipeline fetch boundaries)

#pragma once

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSCollections.h>
#include <stddef.h>
#include "ASOHCIATDescriptor.hpp"

class ASOHCIATDescriptorPool {
public:
    struct Block {
        uint32_t physicalAddress = 0;  // 32-bit IOVA to first descriptor (§7.1)
        void*    virtualAddress  = nullptr; // CPU mapping
        uint32_t descriptorCount = 0;  // number of 16B descriptors
        uint8_t  zValue          = 0;  // Z nibble for CommandPtr when used as a single program
        bool     valid           = false;
    };

    // Buffer structure to match Linux approach
    struct DescriptorBuffer {
        IOBufferMemoryDescriptor* memory = nullptr;
        IOMemoryMap*              map = nullptr;
        void*                     virtualAddress = nullptr;
        uint64_t                  physicalAddress = 0;
        size_t                    bufferSize = 0;
        size_t                    used = 0;
        DescriptorBuffer*         next = nullptr;
    };

    ASOHCIATDescriptorPool() = default;
    ~ASOHCIATDescriptorPool();

    // Initialize with dynamic buffer allocation (Linux-style)
    kern_return_t Initialize(IOPCIDevice* pciDevice, uint8_t barIndex);
    kern_return_t Deallocate();

    // Allocate a descriptor block (N descriptors * 16B). Returns contiguous chunk.
    Block        AllocateBlock(uint32_t descriptorCount);

    // Free previously allocated block
    kern_return_t FreeBlock(const Block& block);

    // Telemetry
    uint32_t GetAvailableDescriptors() const;
    uint32_t GetTotalDescriptors() const;
    bool     IsInitialized() const { return fInitialized; }

private:
    // Dynamic buffer management (Linux-style)
    kern_return_t AddBuffer();
    DescriptorBuffer* FindBufferForAllocation(size_t neededSize);
    
    static constexpr size_t kPageSize = 4096;  // PAGE_SIZE like Linux
    static constexpr size_t kMaxAllocation = 16 * 1024 * 1024;  // 16MB limit like Linux

    IOPCIDevice*                fPCIDevice = nullptr;
    uint8_t                     fBARIndex = 0;
    
    // Buffer list management
    DescriptorBuffer*           fBufferList = nullptr;  // Head of buffer list
    DescriptorBuffer*           fCurrentBuffer = nullptr;  // Current buffer for allocations
    size_t                      fTotalAllocation = 0;
    
    bool                        fInitialized = false;
};
