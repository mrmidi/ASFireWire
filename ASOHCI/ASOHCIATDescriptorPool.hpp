//#pragma once
//
// ASOHCIATDescriptorPool.hpp
// Manages a pool of OUTPUT_* descriptors (DMA-coherent), hands out 16B-aligned blocks.
//
// Spec refs: OHCI 1.1 §7.1 (CommandPtr & Z nibble), §7.6 (prefetch/pipeline fetch boundaries)

#pragma once

#include <stdint.h>
#include <DriverKit/DriverKit.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
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

    ASOHCIATDescriptorPool() = default;
    ~ASOHCIATDescriptorPool();

    // Initialize a backing buffer and map it for both CPU & 32-bit DMA
    kern_return_t Initialize(IOPCIDevice* pciDevice, uint8_t barIndex, uint32_t poolSizeBytes);
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
    struct FreeSpan { uint32_t offset; uint32_t descriptorCount; };

    IOPCIDevice*                fPCIDevice = nullptr;
    uint8_t                     fBARIndex = 0;
    IOBufferMemoryDescriptor*   fPoolMemory = nullptr;
    IOMemoryMap*                fPoolMap = nullptr;
    void*                       fPoolVirtualAddress = nullptr;
    uint64_t                    fPoolPhysicalAddress = 0;
    uint32_t                    fPoolSizeBytes = 0;
    uint32_t                    fDescriptorCount = 0;
    static constexpr uint32_t   kMaxFreeSpans = 32;
    FreeSpan                    fFreeBlocks[kMaxFreeSpans] = {};
    uint32_t                    fFreeSpanCount = 0;
    bool                        fInitialized = false;
};
