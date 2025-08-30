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

// IT interrupt policy for OUTPUT_LAST* descriptors 'i' field (OHCI §9.1.3, §9.1.4, Table 9-2, Table 9-3)
enum class ITIntPolicy : uint8_t {
    kNever  = 0,    // i=00: No interrupt on completion/skip
    kAlways = 3,    // i=11: Interrupt on completion or skipAddress taken
    kOnCompletion = 3,  // Alias for kAlways - interrupt on completion
};

struct ITQueueOptions {
    // Per-packet interrupt policy (IT-specific 'i' bits in OUTPUT_LAST*, OHCI Table 9-3)
    ITIntPolicy interruptPolicy = ITIntPolicy::kNever;

    // Cycle match controls (§9.2, IT ContextControl cycleMatch)
    bool     cycleMatchEnable = false;
    uint8_t  startOnCycle     = 0;     // 7-bit cycle number; honored only if cycleMatchEnable
    bool     allowAppendWhileActive = true; // enable program tail-patching (§9.4)
};

// High-level policy toggles for a context
struct ITPolicy {
    bool     cycleMatchEnable = false;      // §9.2
    bool     cycleMatchEnabled = false;     // Alias for cycleMatchEnable
    uint8_t  startOnCycle     = 0;          // §9.2
    bool     dropIfLate       = true;       // Software policy only: if packet missed its cycle, do not enqueue retroactively
    uint32_t underrunBudgetUs = 0;          // controller-specific: how soon to re-arm after underrun
    ITIntPolicy defaultInterruptPolicy = ITIntPolicy::kOnCompletion; // Default interrupt policy for packets
};

