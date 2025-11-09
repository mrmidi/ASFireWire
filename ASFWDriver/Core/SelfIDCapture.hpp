#pragma once

#include <DriverKit/IOReturn.h>
#include <cstdint>
#include <optional>
#include <vector>
#include <utility>

#ifdef ASFW_HOST_TEST
#include "HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSSharedPtr.h>
#endif

namespace ASFW::Driver {

class HardwareInterface;

class SelfIDCapture {
public:
    struct Result {
        std::vector<uint32_t> quads;
        // sequences: pairs of (start_index, quadlet_count) into `quads`
        std::vector<std::pair<size_t, unsigned int>> sequences;
        uint32_t generation{0};
        bool valid{false};
        bool timedOut{false};
        bool crcError{false};
    };

    SelfIDCapture();
    ~SelfIDCapture();

    kern_return_t PrepareBuffers(size_t quadCapacity, HardwareInterface& hw);
    void ReleaseBuffers();

    kern_return_t Arm(HardwareInterface& hw);
    void Disarm(HardwareInterface& hw);

    // Decode the captured Self-ID buffer using the given SelfIDCount register value.
    // Performs double-read generation validation per OHCI §11.3 to detect racing bus resets.
    // Returns nullopt if buffer not ready.
    std::optional<Result> Decode(uint32_t selfIDCountReg, HardwareInterface& hw) const;

private:
    OSSharedPtr<IOBufferMemoryDescriptor> buffer_;
    OSSharedPtr<IODMACommand> dmaCommand_;
    OSSharedPtr<IOMemoryMap> map_;
    IOAddressSegment segment_{};
    bool segmentValid_{false};
    size_t bufferBytes_{0};
    size_t quadCapacity_{0};
    bool armed_{false};
};

} // namespace ASFW::Driver
