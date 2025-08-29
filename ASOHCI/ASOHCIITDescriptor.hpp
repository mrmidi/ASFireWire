#pragma once
//
// ASOHCIITDescriptor.hpp
// IT uses the same 16B OUTPUT_* descriptor format as AT.
//
// Spec refs: OHCI 1.1 IT §6.1 (program), §6.4 (OUTPUT_* formats)

#include "ASOHCIATDescriptor.hpp" // ATDesc::Descriptor + ATDesc::Program

namespace ITDesc {
    using Descriptor = ATDesc::Descriptor; // identical layout (16B, cmd/key/i/b fields)
    using Program    = ATDesc::Program;    // headPA/tailPA/Z/count (same CommandPtr rules)
}

