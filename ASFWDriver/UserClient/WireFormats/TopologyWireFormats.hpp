//
//  TopologyWireFormats.hpp
//  ASFWDriver
//
//  Wire format structures for Self-ID and topology snapshots
//

#ifndef ASFW_USERCLIENT_TOPOLOGY_WIRE_FORMATS_HPP
#define ASFW_USERCLIENT_TOPOLOGY_WIRE_FORMATS_HPP

#include "WireFormatsCommon.hpp"

namespace ASFW::UserClient::Wire {

// Self-ID capture wire formats
struct __attribute__((packed)) SelfIDMetricsWire {
    uint32_t generation;
    uint64_t captureTimestamp;
    uint32_t quadletCount;        // Number of quadlets in buffer
    uint32_t sequenceCount;       // Number of sequences
    uint8_t valid;
    uint8_t timedOut;
    uint8_t crcError;
    uint8_t _padding;
    char errorReason[64];
    // Followed by: quadlets array, then sequences array
};

struct __attribute__((packed)) SelfIDSequenceWire {
    uint32_t startIndex;
    uint32_t quadletCount;
};

// NOTE: The topology snapshot wire formats (TopologyNodeWire /
// TopologySnapshotWire) were retired. Topology is now served through the
// diagnostics ABI (ASFWDiagTopology / ASFWDiagNode in Shared/ASFWDiagnosticsABI.h),
// which is versioned and layout-shared with the Swift app via the bridging header.

} // namespace ASFW::UserClient::Wire

#endif // ASFW_USERCLIENT_TOPOLOGY_WIRE_FORMATS_HPP
