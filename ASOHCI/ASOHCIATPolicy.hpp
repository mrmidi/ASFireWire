//#pragma once
//
// ASOHCIATPolicy.hpp
// Retry & fairness policy knobs exposed to contexts
//
// Spec refs: OHCI 1.1 §7.3 (ATRetries), §7.4 (FairnessControl), §7.6 (in-order vs pipelined)

#pragma once

#include <stdint.h>

struct ATRetryPolicy {
    // Encodes controller’s ATRetries register fields (§7.3). Interpretation is HW-specific.
    uint32_t raw = 0;          // caller fills sane defaults elsewhere
};

struct ATFairnessPolicy {
    bool enableFairness = true;   // §7.4 (request fairness)
};

struct ATPipelinePolicy {
    // If true, we allow multiple outstanding and accept out-of-order completions (§7.6)
    bool allowPipelining = true;
    // Software cap on in-flight descriptors in a context (prevents starvation)
    uint32_t maxOutstanding = 8;
};

