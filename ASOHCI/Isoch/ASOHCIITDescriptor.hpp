#pragma once
//
// ASOHCIITDescriptor.hpp
// IT uses the same 16B OUTPUT_* descriptor format as AT for non-immediate
// forms. Immediate variants (key=0x2) carry the isoch header quadlets emitted
// on the bus.
//
// Spec refs (OHCI 1.1): §9.1 (program building), §9.6 (IT data/header), Chapter
// 6 (interrupt context)

#include "ASOHCIATDescriptor.hpp" // ATDesc::Descriptor + ATDesc::Program

namespace ITDesc {
using Descriptor =
    ATDesc::Descriptor; // identical layout (16B, cmd/key/i/b fields)
using Program =
    ATDesc::Program; // headPA/tailPA/Z/count (same CommandPtr rules)
} // namespace ITDesc
