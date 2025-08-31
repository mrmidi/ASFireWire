//
//  ASOHCIDriverTypes.hpp
//  ASOHCI
//
//  Created by Aleksandr Shabelnikov on 31.08.2025.
//

#ifndef ASOHCIDriverTypes_hpp
#define ASOHCIDriverTypes_hpp

#include <stdint.h>

// State machine for ASOHCI driver lifecycle (REFACTOR.md §9)
// This enum is used by both IIG boundary and implementation
enum class ASOHCIState : uint32_t {
  Stopped = 0,   // Initial state, no resources allocated
  Starting = 1,  // In the process of starting up
  Running = 2,   // Fully operational, accepting requests
  Quiescing = 3, // In the process of shutting down
  Dead = 4       // Terminal state, cleanup complete
};

#endif /* ASOHCIDriverTypes_hpp */