//#pragma once
//
// ASOHCIATDescriptor.hpp
// Descriptor formats used by AT programs (OUTPUT_MORE/OUTPUT_LAST + IMMEDIATE)
//
// Spec refs: OHCI 1.1 §7.7 (Data formats), §7.1 (Program/list basics), §7.6 (pipelining notes)

#pragma once

#include <stdint.h>

namespace ATDesc {

// Hardware alignment requires 16-byte aligned descriptors; “Z” nibble must match (§7.1)
static constexpr uint32_t kDescriptorAlignBytes = 16;

// Minimal tagged union view for OUTPUT_* descriptors (opaque to clients)
struct alignas(16) Descriptor {
    uint32_t quad[4]; // layout defined by §7.7; builder fills fields
};

// A built descriptor chain (one packet program)
struct Program {
    // 32-bit DMA addresses; OHCI requires 32-bit IOVA (§7.1)
    uint32_t headPA = 0;  // first descriptor physical address
    uint32_t tailPA = 0;  // last descriptor physical address (OUTPUT_LAST*)
    uint8_t  zHead  = 0;  // Z nibble for CommandPtr (§7.1)
    uint32_t descCount = 0;
    // CPU virtual addresses (for tail patching / completion readback)
    void*    headVA = nullptr;
    void*    tailVA = nullptr;
};

} // namespace ATDesc

