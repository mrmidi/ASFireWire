#pragma once
#include <DriverKit/IOLib.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include "OHCIConstants.hpp"
#include "LogHelper.hpp"

// Forward declaration
class ASOHCI;

// Encapsulates serialized access to OHCI PhyControl register using IORecursiveLock.
class ASOHCIPHYAccess {
private:
    IORecursiveLock * _lock {nullptr};
    ASOHCI          * _owner {nullptr};
    IOPCIDevice     * _pci   {nullptr};
    uint8_t           _bar0  {0};

    bool waitForWriteComplete(uint32_t timeoutIterations); // busy poll small bounded
    bool waitForReadComplete(uint32_t timeoutIterations);
public:
    ASOHCIPHYAccess() = default;
    ~ASOHCIPHYAccess();
    bool init(ASOHCI * owner, IOPCIDevice * pci, uint8_t bar0);

    // Lock helpers
    void acquire();
    void release();

    // Apple‑style naming: readPhyRegister / writePhyRegister
    kern_return_t readPhyRegister(uint8_t reg, uint8_t * value); // reg 0..31
    kern_return_t writePhyRegister(uint8_t reg, uint8_t value);
    kern_return_t updatePhyRegisterWithMask(uint8_t reg, uint8_t value, uint8_t mask);
};
