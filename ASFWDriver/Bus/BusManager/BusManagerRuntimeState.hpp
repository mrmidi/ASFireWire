#pragma once
#include <cstdint>

namespace ASFW::Bus {

enum class BMOwnerSource : uint8_t {
    Unknown = 0,
    Inferred = 1,
    BusManagerIdRead = 2,
    ElectionResult = 3,
    LocalWonElection = 4,
    RemoteWonElection = 5,
};

struct BusManagerRuntimeState {
    uint32_t generation{0};
    bool localIsIRM{false};
    bool localIsBM{false};
    bool localIsRoot{false};
    uint8_t localNodeId{0x3F};
    uint8_t rootNodeId{0x3F};
    uint8_t irmNodeId{0x3F};
    uint8_t bmNodeId{0x3F};
    BMOwnerSource bmOwnerSource{BMOwnerSource::Unknown};
    uint32_t lastBusManagerIdOldValue{0x3F};
    uint32_t staleElectionAbortCount{0};
    uint32_t failedElectionCount{0};
    uint32_t unexpectedResourceCsrSoftwareCount{0};
};

} // namespace ASFW::Bus
