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

enum class BMPolicyVerdict : uint8_t {
    ObserveOnly = 0,
    RemoteRootAlreadyCycling = 1,
    RemoteCMSTRNeeded = 2,
    LocalRootCycleMaster = 3,
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

    // BM evidence pipeline fields (FW-14 Phase 2)
    bool rootCmcKnown{false};
    bool rootCmcCapable{false};
    bool cycleStartObserved{false};
    uint8_t cycleStartSourceNode{0x3F};
    bool remoteCmstrNeeded{false};
    bool remoteCmstrAllowed{false};
    bool remoteCmstrAlreadySatisfied{false};
    uint32_t lastRemoteCmstrGeneration{0};
    uint8_t lastRemoteCmstrTargetNode{0x3F};
    uint32_t lastRemoteCmstrResult{0};
    uint8_t bmPolicyVerdict{static_cast<uint8_t>(BMPolicyVerdict::ObserveOnly)};
    uint8_t fullBMActivityLevel{0};

    void ResetGenerationScopedPolicy() noexcept {
        rootCmcKnown = false;
        rootCmcCapable = false;
        cycleStartObserved = false;
        cycleStartSourceNode = 0x3F;
        remoteCmstrNeeded = false;
        remoteCmstrAllowed = false;
        remoteCmstrAlreadySatisfied = false;
        lastRemoteCmstrGeneration = 0;
        lastRemoteCmstrTargetNode = 0x3F;
        lastRemoteCmstrResult = 0;
        bmPolicyVerdict = static_cast<uint8_t>(BMPolicyVerdict::ObserveOnly);
    }
};

} // namespace ASFW::Bus
