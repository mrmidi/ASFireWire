//
//  DiagnosticsWireFormats.hpp
//  ASFWDriver
//
//  Wire format structures for bus state diagnostics
//

#ifndef ASFW_USERCLIENT_DIAGNOSTICS_WIRE_FORMATS_HPP
#define ASFW_USERCLIENT_DIAGNOSTICS_WIRE_FORMATS_HPP

#include "WireFormatsCommon.hpp"

namespace ASFW::UserClient::Wire {

// Bus state diagnostics snapshot (64 bytes)
struct __attribute__((packed)) BusStateWire {
    uint32_t hcControl;       // OHCI HCControl register
    uint32_t linkControl;     // OHCI LinkControl register
    uint32_t nodeId;          // OHCI NodeID register
    uint32_t cycleTime;       // OHCI CycleTimer register
    uint32_t generation;      // Bus generation
    uint32_t busResetCount;   // Bus reset count
    // 24 bytes above
    uint8_t busResetFsmState; // BusResetCoordinator::State enum value
    uint8_t localNodeId;      // Local node ID from topology (0xFF if none)
    uint8_t rootNodeId;       // Root node ID from topology (0xFF if none)
    uint8_t irmNodeId;        // IRM node ID from topology (0xFF if none)
    uint8_t gapCount;         // Gap count from topology
    uint8_t rootPolicy;       // BusManager::Config::RootPolicy enum
    uint8_t delegateCm;       // delegateCycleMaster bool (0/1)
    uint8_t phyReg1;          // PHY register 1 value
    uint8_t phyReg4;          // PHY register 4 value
    // 9 bytes above (24 + 9 = 33)
    uint8_t pad[31];          // Padding to 64 bytes total
};
static_assert(sizeof(BusStateWire) == 64, "BusStateWire must be 64 bytes");

} // namespace ASFW::UserClient::Wire

#endif // ASFW_USERCLIENT_DIAGNOSTICS_WIRE_FORMATS_HPP
