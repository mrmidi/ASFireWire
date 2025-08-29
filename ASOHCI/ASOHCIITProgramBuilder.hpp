#pragma once
//
// ASOHCIITProgramBuilder.hpp
// Builds OUTPUT_MORE/OUTPUT_LAST* (and *_Immediate) chains for IT packets.
//
// Spec refs (OHCI 1.1): §9.1 (list building), §9.4 (appending), §9.6 (IT header/data format)

#include <stdint.h>
#include "ASOHCIATDescriptorPool.hpp"  // reuse the existing pool
#include "ASOHCIITDescriptor.hpp"
#include "ASOHCIITTypes.hpp"

class ASOHCIITProgramBuilder {
public:
    // Reserve up to 'maxDescriptors' (header/immediate + payload frags + last), max 8 (Z range 2..8) (§9.1)
    void Begin(ASOHCIATDescriptorPool& pool, uint32_t maxDescriptors = 7);

    // Build the IT immediate header (controller emits the wire header from these fields) (§9.6)
    // dataLength = payload bytes for this packet; controller pads to quadlet if needed.
    void AddHeaderImmediate(ITSpeed spd,
                            uint8_t tag,
                            uint8_t channel,
                            uint8_t sy,
                            uint32_t dataLength,
                            ATIntPolicy ip = ATIntPolicy::kErrorsOnly);

    // Append a payload fragment by physical address (§6.1)
    void AddPayloadFragment(uint32_t payloadPA, uint32_t payloadBytes);

    // Close the packet with OUTPUT_LAST*; returns a ready-to-enqueue program (§6.1)
    ITDesc::Program Finalize();

    // Abort build and return reserved descriptors to pool
    void Cancel();

private:
    ASOHCIATDescriptorPool* _pool = nullptr;
    ASOHCIATDescriptorPool::Block _blk{};
    uint32_t _descUsed = 0;
    ATIntPolicy _ip = ATIntPolicy::kErrorsOnly;
};

