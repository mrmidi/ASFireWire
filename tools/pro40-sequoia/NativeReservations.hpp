// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <IOKit/firewire/IOFireWireLib.h>
#include <cstdint>
#include <cstdio>
#include <unistd.h>

// Shared generation-bound resource backend; extracted unchanged from ResourceProbe.
namespace Pro40Resources {
static void checkpoint(FILE *log) { std::fflush(log); fsync(fileno(log)); }

struct Resources { uint32_t bandwidth; uint64_t channels; };
static uint32_t be32(const unsigned char *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
static IOReturn snapshot(IOFireWireLibDeviceRef device, uint32_t generation, Resources &r) {
    UInt16 irm = 0;
    auto status = (*device)->GetIRMNodeID(device, generation, &irm);
    if (status) return status;
    FWAddress address{};
    address.nodeID = irm; address.addressHi = 0xffff; address.addressLo = kCSRBandwidthAvailable;
    unsigned char bytes[12]{};
    // Apple IOFWIsochChannel.cpp:735-745 limits IRM reads to quadlets.
    for (unsigned offset = 0; offset < sizeof(bytes); offset += 4) {
        address.addressLo = kCSRBandwidthAvailable + offset;
        UInt32 size = 4;
        status = (*device)->Read(device, 0, &address, bytes + offset, &size, true, generation);
        if (status || size != 4) return status ? status : kIOReturnUnderrun;
    }
    r = {be32(bytes), uint64_t(be32(bytes + 4)) << 32 | be32(bytes + 8)};
    return 0;
}

struct NativeReservations {
    IOFireWireLibDeviceRef device;
    FILE *log;
    IOReturn channel(bool acquire, uint8_t channel, uint32_t generation) {
        std::fprintf(log, "enter channel acquire=%u channel=%u generation=%u\n", acquire, channel, generation);
        checkpoint(log);
        const auto status = acquire
            ? (*device)->AllocateIRMChannelInGeneration(device, channel, generation)
            : (*device)->ReleaseIRMChannelInGeneration(device, channel, generation);
        std::fprintf(log, "leave channel status=0x%08x\n", unsigned(status)); checkpoint(log);
        return status;
    }
    IOReturn bandwidth(bool acquire, uint32_t units, uint32_t generation) {
        std::fprintf(log, "enter bandwidth acquire=%u units=%u generation=%u\n", acquire, units, generation);
        checkpoint(log);
        const auto status = acquire
            ? (*device)->AllocateIRMBandwidthInGeneration(device, units, generation)
            : (*device)->ReleaseIRMBandwidthInGeneration(device, units, generation);
        std::fprintf(log, "leave bandwidth status=0x%08x\n", unsigned(status)); checkpoint(log);
        return status;
    }
};

} // namespace Pro40Resources
