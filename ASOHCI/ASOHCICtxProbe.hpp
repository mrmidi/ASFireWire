// ASOHCICtxProbe.hpp
#pragma once
#include <PCIDriverKit/IOPCIDevice.h>
#include <DriverKit/IOLib.h>
#include "ASOHCICtxRegMap.hpp"   // for IT base/stride and Compute()
#include "ASOHCITypes.hpp"

struct ITProbeResult {
    uint32_t count = 0;      // number of present IT contexts
    uint32_t presentMask = 0;// bit i set => ITi responds
};

inline bool Read32(IOPCIDevice* pci, uint8_t bar, uint32_t off, uint32_t& out) {
    if (!pci) return false;
    pci->MemoryRead32(bar, off, &out);   // DriverKit returns via out; no status
    return true;
}

inline bool LooksLikeMMIOHole(uint32_t v0, uint32_t v1) {
    // Treat 0xFFFFFFFF on both reads as "no device behind this address".
    return (v0 == 0xFFFFFFFFu) && (v1 == 0xFFFFFFFFu);
}

// Probe ITn at computed offsets. Returns true if the window responds.
inline bool ProbeSingleIT(IOPCIDevice* pci, uint8_t bar, uint32_t itIndex) {
    ASContextOffsets offs{};
    if (!ASOHCICtxRegMap::Compute(ASContextKind::kIT_Transmit, itIndex, &offs))
        return false;

    uint32_t b0a = 0, b0b = 0;
    uint32_t b4a = 0, b4b = 0;
    uint32_t cpa = 0, cpb = 0;

    // Two passes to avoid transient bus error artifacts.
    Read32(pci, bar, offs.contextBase,        b0a);
    Read32(pci, bar, offs.contextControlClear, b4a);
    Read32(pci, bar, offs.commandPtr,         cpa);

    Read32(pci, bar, offs.contextBase,        b0b);
    Read32(pci, bar, offs.contextControlClear, b4b);
    Read32(pci, bar, offs.commandPtr,         cpb);

    const bool hole0 = LooksLikeMMIOHole(b0a, b0b);
    const bool hole4 = LooksLikeMMIOHole(b4a, b4b);
    const bool holeC = LooksLikeMMIOHole(cpa, cpb);

    // Only classify "absent" if *all* probed registers look like a hole.
    const bool allHole = hole0 && hole4 && holeC;
    return !allHole;
}

// Returns the number of responding IT contexts (0..32) and a presence bitmask.
inline ITProbeResult ProbeITContextCount(IOPCIDevice* pci, uint8_t bar) {
    ITProbeResult r{};
    for (uint32_t i = 0; i < 32; ++i) {
        if (ProbeSingleIT(pci, bar, i)) {
            r.presentMask |= (1u << i);
            r.count = i + 1; // highest contiguous index + 1
        } else {
            // Stop on first hole for typical contiguous implementations.
            // If you want to allow sparse implementations, remove this break
            // and keep filling the presentMask.
            break;
        }
    }
    return r;
}