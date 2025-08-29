#pragma once
//
// ASOHCIITTypes.hpp
// IT (Isochronous Transmit) enums and simple option structs
//
// Spec refs (OHCI 1.1): §9.1 (program/list basics), §9.2 (context registers),
//   §9.4 (appending), §9.5 (interrupt semantics), §9.6 (data/header formats).
//   Chapter 6 for global IntEvent / IsoXmitIntEvent bit demux.

#include <stdint.h>
#include "ASOHCIATTypes.hpp" // reuse ATIntPolicy definition (same 'i' field semantics)

enum class ITSpeed : uint8_t {
    S100 = 0, S200 = 1, S400 = 2, S800 = 3, // extend if your silicon supports more
};

struct ITQueueOptions {
    // Per-packet interrupt policy (reuses AT's policy: same 'i' bits in OUTPUT_LAST*)
    ATIntPolicy interruptPolicy = ATIntPolicy::kErrorsOnly;

    // Cycle match controls (§6.2, IT ContextControl cycleMatch)
    bool     cycleMatchEnable = false;
    uint8_t  startOnCycle     = 0;     // 7-bit cycle number; honored only if cycleMatchEnable
    bool     allowAppendWhileActive = true; // enable program tail-patching (§6.1, appending)
};

// High-level policy toggles for a context
struct ITPolicy {
    bool     cycleMatchEnable = false;  // §6.2
    uint8_t  startOnCycle     = 0;      // §6.2
    bool     dropIfLate       = true;   // Software policy only: if packet missed its cycle, do not enqueue retroactively
    uint32_t underrunBudgetUs = 0;      // controller-specific: how soon to re-arm after underrun
};

