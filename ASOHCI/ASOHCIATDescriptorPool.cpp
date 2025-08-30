//
// ASOHCIATDescriptorPool.cpp
// ASOHCI
//
// OHCI 1.1 AT Descriptor Pool Management Implementation
// Based on Linux OHCI implementation - dynamic buffer allocation instead of
// large pre-allocated pools Based on OHCI 1.1 Specification §7.1 (List
// management), §7.7 (Descriptor formats)
//

#include "ASOHCIATDescriptorPool.hpp"
#include "LogHelper.hpp"
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/OSCollections.h>

// Memory allocation constants (Linux OHCI reference + OHCI 1.1 spec compliance)
static constexpr size_t kPageSize =
    4096; // Standard page size (Linux compatible)
static constexpr size_t kMaxAllocation =
    16 * 1024 * 1024; // 16MB limit (Linux OHCI limit)

// OHCI 1.1 specification compliance constants
static constexpr size_t kOHCIDescriptorAlignment =
    16; // OHCI §7.1 - descriptors must be 16-byte aligned
static constexpr size_t kOHCIMaxDescriptorBlock =
    128; // OHCI §7.1 - descriptor block max size
static constexpr size_t kLinuxARBufferSize =
    32 * 1024; // Linux AR_BUFFER_SIZE (32KB)

ASOHCIATDescriptorPool::~ASOHCIATDescriptorPool() { Deallocate(); }

kern_return_t ASOHCIATDescriptorPool::Initialize(IOPCIDevice *pciDevice,
                                                 uint8_t barIndex) {
  kern_return_t result = kIOReturnError;

  os_log(ASLog(),
         "ASOHCIATDescriptorPool: Initialize called with dynamic allocation "
         "(Linux-style), barIndex=%u",
         barIndex);

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
    os_log(ASLog(),
           "ASOHCIATDescriptorPool: Failed to add initial buffer: 0x%x",
           result);
    goto cleanup;
  }

  fInitialized = true;

  os_log(
      ASLog(),
      "ASOHCIATDescriptorPool: SUCCESS - Initialized with dynamic allocation");

  return kIOReturnSuccess;

cleanup:
  Deallocate();
  return result;
}

kern_return_t ASOHCIATDescriptorPool::AddBuffer() {
  kern_return_t result;
  DescriptorBuffer *newBuffer;

  // Check allocation limit (Linux has 16MB limit)
  if (fTotalAllocation >= kMaxAllocation) {
    os_log(ASLog(),
           "ASOHCIATDescriptorPool: Allocation limit reached (%zu bytes)",
           kMaxAllocation);
    return kIOReturnNoMemory;
  }

  // Allocate new buffer structure
  newBuffer = new DescriptorBuffer();
  if (!newBuffer) {
    os_log(ASLog(),
           "ASOHCIATDescriptorPool: Failed to allocate buffer structure");
    return kIOReturnNoMemory;
  }

  // Pre-allocation logging (Linux-inspired detailed context)
  os_log(ASLog(),
         "ASOHCIATDescriptorPool: Attempting IOBufferMemoryDescriptor::Create");
  os_log(ASLog(),
         "ASOHCIATDescriptorPool:   -> Direction: %d (kIOMemoryDirectionInOut)",
         kIOMemoryDirectionInOut);
  os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Size: %zu bytes (PAGE_SIZE)",
         kPageSize);
  os_log(ASLog(),
         "ASOHCIATDescriptorPool:   -> Alignment: %zu bytes (OHCI spec)",
         (size_t)ATDesc::kDescriptorAlignBytes);
  os_log(ASLog(),
         "ASOHCIATDescriptorPool:   -> Current pool usage: %zu/%zu bytes (%d%% "
         "used)",
         fTotalAllocation, kMaxAllocation,
         (int)((fTotalAllocation * 100) / kMaxAllocation));

  // Linux-inspired fallback allocation strategy: try smaller sizes if full
  // PAGE_SIZE fails
  static const size_t kAllocationSizes[] = {kPageSize, 2048, 1024, 512};
  size_t allocatedSize = 0;

  for (size_t size : kAllocationSizes) {
    os_log(ASLog(), "ASOHCIATDescriptorPool: Trying allocation size: %zu bytes",
           size);
    result = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionInOut, size,
        ATDesc::kDescriptorAlignBytes, // 16-byte alignment
        &newBuffer->memory);
    if (result == kIOReturnSuccess) {
      allocatedSize = size;
      if (size != kPageSize) {
        os_log(ASLog(),
               "ASOHCIATDescriptorPool: Fallback allocation succeeded with "
               "size=%zu (requested=%zu)",
               size, kPageSize);
      }
      break;
    } else {
      os_log(ASLog(),
             "ASOHCIATDescriptorPool: Allocation failed for size=%zu: 0x%x",
             size, result);
    }
  }

  if (result != kIOReturnSuccess) {
    // Enhanced failure logging with comprehensive context
    os_log(
        ASLog(),
        "ASOHCIATDescriptorPool: IOBufferMemoryDescriptor::Create FAILED: 0x%x",
        result);
    os_log(ASLog(), "ASOHCIATDescriptorPool: Allocation request details:");
    os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Requested size: %zu bytes",
           kPageSize);
    os_log(ASLog(),
           "ASOHCIATDescriptorPool:   -> Requested alignment: %zu bytes",
           (size_t)ATDesc::kDescriptorAlignBytes);
    os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Direction: %d",
           kIOMemoryDirectionInOut);
    os_log(ASLog(), "ASOHCIATDescriptorPool: Pool state at failure:");
    os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Allocated: %zu bytes",
           fTotalAllocation);
    os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Limit: %zu bytes",
           kMaxAllocation);
    os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Remaining: %zu bytes",
           kMaxAllocation - fTotalAllocation);
    if (result == kIOReturnNoMemory) {
      os_log(ASLog(), "ASOHCIATDescriptorPool: System DMA coherent memory "
                      "exhausted - check available memory");
      os_log(ASLog(), "ASOHCIATDescriptorPool: Consider reducing buffer sizes "
                      "or implementing fallback strategies");
    }
    delete newBuffer;
    return result;
  }

  // Success case logging with memory details
  os_log(ASLog(),
         "ASOHCIATDescriptorPool: IOBufferMemoryDescriptor::Create SUCCESS");
  os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Allocated size: %zu bytes",
         kPageSize);
  os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Alignment: %zu bytes",
         (size_t)ATDesc::kDescriptorAlignBytes);

  // Map memory for CPU access
  result = newBuffer->memory->CreateMapping(0, 0, 0, 0, 0, &newBuffer->map);
  if (result != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCIATDescriptorPool: Failed to map memory: 0x%x",
           result);
    newBuffer->memory->release();
    delete newBuffer;
    return result;
  }

  // Get virtual address
  newBuffer->virtualAddress =
      reinterpret_cast<void *>(newBuffer->map->GetAddress());
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
    os_log(ASLog(),
           "ASOHCIATDescriptorPool: Failed to get physical address: 0x%x",
           result);
    newBuffer->map->release();
    newBuffer->memory->release();
    delete newBuffer;
    return result;
  }

  newBuffer->physicalAddress = segment.address;
  newBuffer->bufferSize =
      allocatedSize; // Use actual allocated size, not hardcoded kPageSize

  // Post-allocation success details (Linux-inspired comprehensive reporting)
  os_log(
      ASLog(),
      "ASOHCIATDescriptorPool: Memory mapping SUCCESS - allocation details:");
  os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Virtual addr: %p",
         newBuffer->virtualAddress);
  os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Physical addr: 0x%llx",
         newBuffer->physicalAddress);
  os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Buffer size: %zu bytes",
         newBuffer->bufferSize);
  os_log(ASLog(), "ASOHCIATDescriptorPool:   -> DMA-coherent: YES "
                  "(IOBufferMemoryDescriptor)");
  newBuffer->used = 0;
  newBuffer->next = nullptr;

  // Validate 32-bit addressability (OHCI requirement)
  if (newBuffer->physicalAddress + kPageSize > 0x100000000ULL) {
    os_log(ASLog(),
           "ASOHCIATDescriptorPool: Memory not 32-bit addressable (PA=0x%llx)",
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
    DescriptorBuffer *last = fBufferList;
    while (last->next) {
      last = last->next;
    }
    last->next = newBuffer;
  }

  // Set as current buffer if we don't have one
  if (!fCurrentBuffer) {
    fCurrentBuffer = newBuffer;
  }

  // Update total allocation tracking
  fTotalAllocation += allocatedSize; // Track actual allocated size

  // Pool allocation summary (Linux-inspired resource tracking)
  uint32_t usagePercent = (uint32_t)((fTotalAllocation * 100) / kMaxAllocation);
  os_log(ASLog(), "ASOHCIATDescriptorPool: Buffer added successfully");
  os_log(ASLog(), "ASOHCIATDescriptorPool: Pool allocation summary:");
  os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Total allocated: %zu bytes",
         fTotalAllocation);
  os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Pool limit: %zu bytes",
         kMaxAllocation);
  os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Usage: %u%% (%zu/%zu)",
         usagePercent, fTotalAllocation, kMaxAllocation);
  os_log(ASLog(), "ASOHCIATDescriptorPool:   -> Remaining: %zu bytes",
         kMaxAllocation - fTotalAllocation);

  return kIOReturnSuccess;
}

ASOHCIATDescriptorPool::DescriptorBuffer *
ASOHCIATDescriptorPool::FindBufferForAllocation(size_t neededSize) {
  DescriptorBuffer *buffer = fCurrentBuffer;

  // Check current buffer first
  if (buffer && (buffer->bufferSize - buffer->used) >= neededSize) {
    return buffer;
  }

  // Search through all buffers
  buffer = fBufferList;
  while (buffer) {
    if ((buffer->bufferSize - buffer->used) >= neededSize) {
      fCurrentBuffer = buffer; // Update current buffer
      return buffer;
    }
    buffer = buffer->next;
  }

  return nullptr;
}

kern_return_t ASOHCIATDescriptorPool::Deallocate() {
  DescriptorBuffer *buffer = fBufferList;
  DescriptorBuffer *next;

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

ASOHCIATDescriptorPool::Block
ASOHCIATDescriptorPool::AllocateBlock(uint32_t descriptorCount) {
  Block block = {};
  size_t neededSize = descriptorCount * sizeof(ATDesc::Descriptor);
  DescriptorBuffer *buffer;

  if (!fInitialized || descriptorCount == 0) {
    return block;
  }

  // Find a buffer with enough space
  buffer = FindBufferForAllocation(neededSize);
  if (!buffer) {
    // Try to add a new buffer
    kern_return_t result = AddBuffer();
    if (result != kIOReturnSuccess) {
      os_log(ASLog(),
             "ASOHCIATDescriptorPool: Failed to add buffer for allocation");
      return block;
    }
    buffer = fCurrentBuffer;
  }

  // Check if buffer has enough space (should be true after
  // FindBufferForAllocation/AddBuffer)
  if ((buffer->bufferSize - buffer->used) < neededSize) {
    os_log(
        ASLog(),
        "ASOHCIATDescriptorPool: Buffer allocation error - insufficient space");
    return block;
  }

  // Allocate from buffer
  uint32_t allocOffset = static_cast<uint32_t>(buffer->used);
  uint32_t allocPhysAddr =
      static_cast<uint32_t>(buffer->physicalAddress) + allocOffset;
  void *allocVirtAddr =
      static_cast<uint8_t *>(buffer->virtualAddress) + allocOffset;

  // Calculate Z nibble (OHCI §7.1: descriptor count encoding)
  uint8_t zValue;
  if (descriptorCount == 0) {
    zValue = 0; // End of list
  } else if (descriptorCount >= 2 && descriptorCount <= 8) {
    zValue = static_cast<uint8_t>(descriptorCount); // Valid descriptor block
  } else {
    os_log(ASLog(),
           "ASOHCIATDescriptorPool: Invalid descriptor count %u for Z nibble",
           descriptorCount);
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

  os_log(ASLog(),
         "ASOHCIATDescriptorPool: Allocated block with %u descriptors "
         "(PA=0x%x, Z=%u) from buffer at 0x%llx",
         descriptorCount, allocPhysAddr, zValue, buffer->physicalAddress);

  return block;
}

kern_return_t ASOHCIATDescriptorPool::FreeBlock(const Block &block) {
  if (!fInitialized || !block.valid) {
    return kIOReturnBadArgument;
  }

  // In Linux-style dynamic allocation, we don't actually free individual blocks
  // within a buffer. The buffer remains allocated until the pool is destroyed.
  // This is acceptable since OHCI descriptors are typically used for the
  // lifetime of the context.

  os_log(ASLog(), "ASOHCIATDescriptorPool: FreeBlock called - block remains "
                  "allocated (Linux-style)");

  return kIOReturnSuccess;
}

uint32_t ASOHCIATDescriptorPool::GetAvailableDescriptors() const {
  if (!fInitialized) {
    return 0;
  }

  uint32_t available = 0;
  DescriptorBuffer *buffer = fBufferList;

  while (buffer) {
    size_t remaining = buffer->bufferSize - buffer->used;
    available += remaining / sizeof(ATDesc::Descriptor);
    buffer = buffer->next;
  }

  return available;
}

uint32_t ASOHCIATDescriptorPool::GetTotalDescriptors() const {
  if (!fInitialized) {
    return 0;
  }

  return fTotalAllocation / sizeof(ATDesc::Descriptor);
}
