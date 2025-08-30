//#pragma once
//
// ASOHCIATPolicy.hpp
// Retry & fairness policy knobs exposed to contexts
//
// Spec refs: OHCI 1.1 §7.3 (ATRetries), §7.4 (FairnessControl), §7.6 (in-order vs pipelined)

#pragma once

#include <stdint.h>

struct ATRetryPolicy {
    // OHCI 1.1 §7.3 ATRetries register fields
    
    // Time-limit fields (dual-phase retry window) - optional, may be read-only 0 if not implemented
    uint32_t secondLimit = 0;        // 3 bits: seconds counter (0-7s)
    uint32_t cycleLimit = 0;         // 13 bits: 1394 cycle counter (0-7999, 8000 cycles = 1s)
    
    // Per-unit max-retry counters (always meaningful)
    uint32_t maxRetryA = 4;          // maxATReqRetries: AT Request Unit retry cap
    uint32_t maxRetryB = 4;          // maxATRespRetries: AT Response Unit retry cap  
    uint32_t maxPhyResp = 0;         // maxPhysRespRetries: Physical Response Unit retry cap
    
    // Legacy raw field for direct register access (computed from above fields)
    uint32_t raw = 0;                // caller fills sane defaults elsewhere
};

struct ATFairnessPolicy {
    bool enableFairness = true;       // §7.4 (request fairness)
    uint32_t fairnessControl = 0x3F;  // OHCI fairness control value (default 0x3F)
};

struct ATPipelinePolicy {
    // If true, we allow multiple outstanding and accept out-of-order completions (§7.6)
    bool allowPipelining = true;
    // Software cap on in-flight descriptors in a context (prevents starvation)
    uint32_t maxOutstanding = 8;
};

