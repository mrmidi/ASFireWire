//
//  ASOHCI_Priv.hpp
//  ASOHCI
//
//  Created by Aleksandr Shabelnikov on 23.08.2025.
//

#ifndef ASOHCI_Priv_h
#define ASOHCI_Priv_h

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSData.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

// Forward declarations for types defined in ASOHCIIVars.h
enum class ASOHCIState : uint32_t;
struct ASOHCI_IVars;

// State machine query methods (REFACTOR.md §9) - implementations
ASOHCIState GetCurrentState(ASOHCI_IVars *ivars);
const char *GetCurrentStateString(ASOHCI_IVars *ivars);
bool IsInState(ASOHCI_IVars *ivars, ASOHCIState state);

#endif /* ASOHCI_Priv_h */