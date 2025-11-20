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

// Topology snapshot wire formats
struct __attribute__((packed)) TopologyNodeWire {
    uint8_t nodeId;
    uint8_t portCount;
    uint8_t gapCount;
    uint8_t powerClass;
    uint32_t maxSpeedMbps;
    uint8_t isIRMCandidate;
    uint8_t linkActive;
    uint8_t initiatedReset;
    uint8_t isRoot;
    uint8_t parentPort;      // 0xFF if no parent
    uint8_t portStateCount;  // Number of port states
    uint8_t _padding[2];
    // Followed by: port states array (uint8_t per port)
};

struct __attribute__((packed)) TopologySnapshotWire {
    uint32_t generation;
    uint64_t capturedAt;
    uint8_t nodeCount;
    uint8_t rootNodeId;      // 0xFF if none
    uint8_t irmNodeId;       // 0xFF if none
    uint8_t localNodeId;     // 0xFF if none
    uint8_t gapCount;
    uint8_t warningCount;
    uint16_t busBase16;      // Bus base (bus << 6), ready to OR with node ID
    // Followed by: nodes array, then warnings array (null-terminated strings)
};

} // namespace ASFW::UserClient::Wire

#endif // ASFW_USERCLIENT_TOPOLOGY_WIRE_FORMATS_HPP
