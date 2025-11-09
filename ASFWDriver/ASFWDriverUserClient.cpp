#include "ASFWDriverUserClient.h"
#include "ASFWDriver.h"
#include "Core/ControllerCore.hpp"
#include "Core/ControllerMetrics.hpp"
#include "Core/ControllerStateMachine.hpp"
#include "Core/TopologyManager.hpp"
#include "Core/MetricsSink.hpp"
#include "Logging/Logging.hpp"
#include "Core/ControllerTypes.hpp"
#include "Debug/BusResetPacketCapture.hpp"
#include "Async/AsyncSubsystem.hpp"
#include "Discovery/ConfigROMStore.hpp"
#include "Discovery/ROMScanner.hpp"

#include <cstdio>

#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>
#include <os/log.h>

namespace {
constexpr uint64_t kSharedStatusMemoryType = 0;

// Transaction result storage
struct TransactionResult {
    uint16_t handle{0};
    uint32_t status{0};  // AsyncStatus value
    uint32_t dataLength{0};
    uint8_t data[512]{};  // Max response data size
};

// Internal storage for transaction results (not visible to IIG)
struct TransactionStorage {
    static constexpr size_t kMaxCompletedTransactions = 16;
    TransactionResult completedTransactions[kMaxCompletedTransactions];
    size_t completedHead{0};  // Next slot to write
    size_t completedTail{0};  // Oldest unread result
    IOLock* completedLock{nullptr};

    TransactionStorage() {
        completedLock = IOLockAlloc();
    }

    ~TransactionStorage() {
        if (completedLock) {
            IOLockFree(completedLock);
            completedLock = nullptr;
        }
    }
};

// Static completion callback for async transactions
// This is called by AsyncSubsystem when a transaction completes
static void AsyncTransactionCompletionCallback(
    ASFW::Async::AsyncHandle handle,
    ASFW::Async::AsyncStatus status,
    void* context,
    const void* responsePayload,
    uint32_t responseLength)
{
    auto* userClient = static_cast<ASFWDriverUserClient*>(context);
    if (!userClient || !userClient->ivars || !userClient->ivars->transactionStorage) {
        return;
    }

    auto* storage = static_cast<TransactionStorage*>(userClient->ivars->transactionStorage);
    IOLockLock(storage->completedLock);

    // Calculate next head position
    size_t nextHead = (storage->completedHead + 1) % TransactionStorage::kMaxCompletedTransactions;

    // If buffer is full, drop oldest result
    if (nextHead == storage->completedTail) {
        storage->completedTail = (storage->completedTail + 1) % TransactionStorage::kMaxCompletedTransactions;
        ASFW_LOG(UserClient, "AsyncTransactionCompletion: Dropped oldest result (buffer full)");
    }

    // Store result
    TransactionResult& result = storage->completedTransactions[storage->completedHead];
    result.handle = handle.value;
    result.status = static_cast<uint32_t>(status);
    result.dataLength = (responseLength > 512) ? 512 : responseLength;

    if (responsePayload && responseLength > 0 && result.dataLength > 0) {
        std::memcpy(result.data, responsePayload, result.dataLength);
    }

    storage->completedHead = nextHead;

    IOLockUnlock(storage->completedLock);

    // Send async notification to GUI
    userClient->NotifyTransactionComplete(handle.value, static_cast<uint32_t>(status));

    ASFW_LOG(UserClient, "AsyncTransactionCompletion: handle=0x%04x status=%u len=%u stored",
             handle.value, static_cast<uint32_t>(status), responseLength);
}

}

// Wire format structures
constexpr uint32_t kControllerStatusWireVersion = 1;

struct ControllerStatusFlags {
    static constexpr uint32_t kIsIRM = 1u << 0;
    static constexpr uint32_t kIsCycleMaster = 1u << 1;
};

struct ControllerStatusAsyncDescriptorWire {
    uint64_t descriptorVirt{0};
    uint64_t descriptorIOVA{0};
    uint32_t descriptorCount{0};
    uint32_t descriptorStride{0};
    uint32_t commandPtr{0};
    uint32_t reserved{0};
};
static_assert(sizeof(ControllerStatusAsyncDescriptorWire) == 32, "Async descriptor wire size mismatch");

struct ControllerStatusAsyncBuffersWire {
    uint64_t bufferVirt{0};
    uint64_t bufferIOVA{0};
    uint32_t bufferCount{0};
    uint32_t bufferSize{0};
};
static_assert(sizeof(ControllerStatusAsyncBuffersWire) == 24, "Async buffer wire size mismatch");

struct ControllerStatusAsyncWire {
    ControllerStatusAsyncDescriptorWire atRequest{};
    ControllerStatusAsyncDescriptorWire atResponse{};
    ControllerStatusAsyncDescriptorWire arRequest{};
    ControllerStatusAsyncDescriptorWire arResponse{};
    ControllerStatusAsyncBuffersWire arRequestBuffers{};
    ControllerStatusAsyncBuffersWire arResponseBuffers{};
    uint64_t dmaSlabVirt{0};
    uint64_t dmaSlabIOVA{0};
    uint32_t dmaSlabSize{0};
    uint32_t reserved{0};
};
static_assert(sizeof(ControllerStatusAsyncWire) == 200, "Async status wire size mismatch");

struct ControllerStatusWire {
    uint32_t version{0};
    uint32_t flags{0};
    char stateName[32]{};
    uint32_t generation{0};
    uint32_t nodeCount{0};
    uint32_t localNodeID{0xFFFFFFFFu};
    uint32_t rootNodeID{0xFFFFFFFFu};
    uint32_t irmNodeID{0xFFFFFFFFu};
    uint64_t busResetCount{0};
    uint64_t lastBusResetTime{0};
    uint64_t uptimeNanoseconds{0};
    ControllerStatusAsyncWire async{};
};
static_assert(sizeof(ControllerStatusWire) == 288, "ControllerStatusWire size mismatch");

struct __attribute__((packed)) BusResetPacketWire {
    uint64_t captureTimestamp;
    uint32_t generation;
    uint8_t eventCode;
    uint8_t tCode;
    uint16_t cycleTime;
    uint32_t rawQuadlets[4];
    uint32_t wireQuadlets[4];
    char contextInfo[64];
};

// Self-ID and Topology wire formats
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
    uint8_t _padding[2];
    // Followed by: nodes array, then warnings array (null-terminated strings)
};

bool ASFWDriverUserClient::init()
{
    if (!super::init()) {
        return false;
    }

    ivars = IONewZero(ASFWDriverUserClient_IVars, 1);
    if (!ivars) {
        return false;
    }

    ivars->statusRegistered = false;
    ivars->statusAction = nullptr;
    ivars->transactionListenerRegistered = false;
    ivars->transactionAction = nullptr;

    // Allocate transaction storage
    auto* storage = new TransactionStorage();
    if (!storage || !storage->completedLock) {
        delete storage;
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
        return false;
    }
    ivars->transactionStorage = static_cast<void*>(storage);

    return true;
}

void ASFWDriverUserClient::free()
{
    if (ivars) {
        if (ivars->driver && ivars->statusRegistered) {
            ivars->driver->UnregisterStatusListener(this);
        }
        if (ivars->statusAction) {
            ivars->statusAction->release();
            ivars->statusAction = nullptr;
        }
        if (ivars->transactionAction) {
            ivars->transactionAction->release();
            ivars->transactionAction = nullptr;
        }
        if (ivars->transactionStorage) {
            delete static_cast<TransactionStorage*>(ivars->transactionStorage);
            ivars->transactionStorage = nullptr;
        }
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
    }
    super::free();
}

kern_return_t ASFWDriverUserClient::Start_Impl(IOService* provider)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // Store typed reference to driver
    ivars->driver = OSDynamicCast(ASFWDriver, provider);
    if (!ivars->driver) {
        return kIOReturnError;
    }

    ivars->statusRegistered = false;
    if (ivars->statusAction) {
        ivars->statusAction->release();
        ivars->statusAction = nullptr;
    }

    ASFW_LOG(UserClient, "Start() completed");
    return kIOReturnSuccess;
}

kern_return_t ASFWDriverUserClient::Stop_Impl(IOService* provider)
{
    if (ivars && ivars->driver && ivars->statusRegistered) {
        ivars->driver->UnregisterStatusListener(this);
        ivars->statusRegistered = false;
    }

    if (ivars && ivars->statusAction) {
        ivars->statusAction->release();
        ivars->statusAction = nullptr;
    }

    ivars->driver = nullptr;

    ASFW_LOG(UserClient, "Stop() completed");
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t ASFWDriverUserClient::ExternalMethod(
    uint64_t selector,
    IOUserClientMethodArguments* arguments,
    const IOUserClientMethodDispatch* dispatch,
    OSObject* target,
    void* reference)
{
    (void)dispatch;
    (void)target;
    (void)reference;

    if (!ivars || !ivars->driver) {
        return kIOReturnNotReady;
    }
    auto* driver = ivars->driver;

    switch (selector) {
        case 0: { // kMethodGetBusResetCount
            // Return bus reset count, generation, and timestamp
            // Output: 3 scalar uint64_t values
            if (!arguments || arguments->scalarOutputCount < 3) {
                return kIOReturnBadArgument;
            }

            // Get real metrics from ControllerCore
            using namespace ASFW::Driver;
            auto* controller = static_cast<ControllerCore*>(driver->GetControllerCore());
            if (!controller) {
                // Driver not fully initialized yet
                arguments->scalarOutput[0] = 0;
                arguments->scalarOutput[1] = 0;
                arguments->scalarOutput[2] = 0;
                arguments->scalarOutputCount = 3;
                return kIOReturnSuccess;
            }

            auto& metrics = controller->Metrics().BusReset();
            uint32_t generation = 0;
            if (auto topo = controller->LatestTopology()) {
                generation = topo->generation;
            }

            arguments->scalarOutput[0] = metrics.resetCount;
            arguments->scalarOutput[1] = generation;
            arguments->scalarOutput[2] = metrics.lastResetCompletion;
            arguments->scalarOutputCount = 3;

            return kIOReturnSuccess;
        }

        case 1: { // kMethodGetBusResetHistory
            // Return array of bus reset packet snapshots
            // Input: startIndex, count
            // Output: OSData with BusResetPacketWire array
            if (!arguments || arguments->scalarInputCount < 2) {
                return kIOReturnBadArgument;
            }

            const uint64_t startIndex = arguments->scalarInput[0];
            const uint64_t requestCount = arguments->scalarInput[1];

            if (requestCount == 0 || requestCount > 32) {
                return kIOReturnBadArgument;
            }

            using namespace ASFW::Async;
            using namespace ASFW::Debug;

            // Get capture from driver's async subsystem
            auto* asyncSys = static_cast<AsyncSubsystem*>(driver->GetAsyncSubsystem());
            if (!asyncSys) {
                // Return empty if not available
                OSData* data = OSData::withCapacity(0);
                if (!data) return kIOReturnNoMemory;
                arguments->structureOutput = data;
                arguments->structureOutputDescriptor = nullptr;
                return kIOReturnSuccess;
            }

            auto* capture = asyncSys->GetBusResetCapture();
            if (!capture) {
                // Return empty if not available
                OSData* data = OSData::withCapacity(0);
                if (!data) return kIOReturnNoMemory;
                arguments->structureOutput = data;
                arguments->structureOutputDescriptor = nullptr;
                return kIOReturnSuccess;
            }

            // Determine how many packets to return
            size_t totalCount = capture->GetCount();
            if (startIndex >= totalCount) {
                // startIndex out of range, return empty
                OSData* data = OSData::withCapacity(0);
                if (!data) return kIOReturnNoMemory;
                arguments->structureOutput = data;
                arguments->structureOutputDescriptor = nullptr;
                return kIOReturnSuccess;
            }

            size_t availableCount = totalCount - startIndex;
            size_t returnCount = std::min(availableCount, static_cast<size_t>(requestCount));

            // Allocate buffer for wire format packets
            size_t dataSize = returnCount * sizeof(BusResetPacketWire);
            OSData* data = OSData::withCapacity(static_cast<uint32_t>(dataSize));
            if (!data) {
                return kIOReturnNoMemory;
            }

            // Copy packets from capture to wire format
            for (size_t i = 0; i < returnCount; i++) {
                auto snapshot = capture->GetSnapshot(startIndex + i);
                if (!snapshot) break;  // Shouldn't happen, but be safe

                BusResetPacketWire wire{};
                wire.captureTimestamp = snapshot->captureTimestamp;
                wire.generation = snapshot->generation;
                wire.eventCode = snapshot->eventCode;
                wire.tCode = snapshot->tCode;
                wire.cycleTime = snapshot->cycleTime;

                // Copy quadlets
                for (int q = 0; q < 4; q++) {
                    wire.rawQuadlets[q] = snapshot->rawQuadlets[q];
                    wire.wireQuadlets[q] = snapshot->wireQuadlets[q];
                }

                // Copy context info
                std::strncpy(wire.contextInfo, snapshot->contextInfo, sizeof(wire.contextInfo) - 1);
                wire.contextInfo[sizeof(wire.contextInfo) - 1] = '\0';

                // Append to OSData
                if (!data->appendBytes(&wire, sizeof(wire))) {
                    data->release();
                    return kIOReturnNoMemory;
                }
            }

            arguments->structureOutput = data;
            arguments->structureOutputDescriptor = nullptr;

            return kIOReturnSuccess;
        }

        case 2: { // kMethodGetControllerStatus
            // Return comprehensive controller status
            // Output: ControllerStatusWire structure
            if (!arguments) {
                return kIOReturnBadArgument;
            }

            using namespace ASFW::Driver;
            ControllerStatusWire status{};
            status.version = kControllerStatusWireVersion;
            status.flags = 0;
            std::strncpy(status.stateName, "NotReady", sizeof(status.stateName));
            status.stateName[sizeof(status.stateName) - 1] = '\0';
            status.generation = 0;
            status.nodeCount = 0;
            status.localNodeID = 0xFFFFFFFFu;
            status.rootNodeID = 0xFFFFFFFFu;
            status.irmNodeID = 0xFFFFFFFFu;
            status.busResetCount = 0;
            status.lastBusResetTime = 0;
            status.uptimeNanoseconds = 0;

            auto* controller = static_cast<ControllerCore*>(driver->GetControllerCore());
            if (controller) {
                auto stateStr = std::string(ToString(controller->StateMachine().CurrentState()));
                std::strncpy(status.stateName, stateStr.c_str(), sizeof(status.stateName) - 1);
                status.stateName[sizeof(status.stateName) - 1] = '\0';

                const auto& busResetMetrics = controller->Metrics().BusReset();
                status.busResetCount = busResetMetrics.resetCount;
                status.lastBusResetTime = busResetMetrics.lastResetCompletion;
                if (busResetMetrics.lastResetCompletion >= busResetMetrics.lastResetStart) {
                    status.uptimeNanoseconds = busResetMetrics.lastResetCompletion - busResetMetrics.lastResetStart;
                } else {
                    status.uptimeNanoseconds = busResetMetrics.lastResetCompletion;
                }

                if (auto topo = controller->LatestTopology()) {
                    status.generation = topo->generation;
                    status.nodeCount = topo->nodeCount;
                    status.localNodeID = topo->localNodeId.has_value()
                        ? static_cast<uint32_t>(*topo->localNodeId)
                        : 0xFFFFFFFFu;
                    status.rootNodeID = topo->rootNodeId.has_value()
                        ? static_cast<uint32_t>(*topo->rootNodeId)
                        : 0xFFFFFFFFu;
                    status.irmNodeID = topo->irmNodeId.has_value()
                        ? static_cast<uint32_t>(*topo->irmNodeId)
                        : 0xFFFFFFFFu;

                    if (topo->irmNodeId.has_value() && topo->localNodeId.has_value() &&
                        topo->irmNodeId == topo->localNodeId) {
                        status.flags |= ControllerStatusFlags::kIsIRM;
                    }
                    // TODO: Determine cycle-master role from hardware registers/topology
                }
            }

            if (auto* asyncSys = static_cast<ASFW::Async::AsyncSubsystem*>(driver->GetAsyncSubsystem())) {
                if (auto snapshotOpt = asyncSys->GetStatusSnapshot()) {
                    const auto& snapshot = *snapshotOpt;

                    status.async.atRequest.descriptorVirt = snapshot.atRequest.descriptorVirt;
                    status.async.atRequest.descriptorIOVA = snapshot.atRequest.descriptorIOVA;
                    status.async.atRequest.descriptorCount = snapshot.atRequest.descriptorCount;
                    status.async.atRequest.descriptorStride = snapshot.atRequest.descriptorStride;
                    status.async.atRequest.commandPtr = snapshot.atRequest.commandPtr;

                    status.async.atResponse = {
                        snapshot.atResponse.descriptorVirt,
                        snapshot.atResponse.descriptorIOVA,
                        snapshot.atResponse.descriptorCount,
                        snapshot.atResponse.descriptorStride,
                        snapshot.atResponse.commandPtr,
                        0
                    };

                    status.async.arRequest = {
                        snapshot.arRequest.descriptorVirt,
                        snapshot.arRequest.descriptorIOVA,
                        snapshot.arRequest.descriptorCount,
                        snapshot.arRequest.descriptorStride,
                        snapshot.arRequest.commandPtr,
                        0
                    };

                    status.async.arResponse = {
                        snapshot.arResponse.descriptorVirt,
                        snapshot.arResponse.descriptorIOVA,
                        snapshot.arResponse.descriptorCount,
                        snapshot.arResponse.descriptorStride,
                        snapshot.arResponse.commandPtr,
                        0
                    };

                    status.async.arRequestBuffers.bufferVirt = snapshot.arRequestBuffers.bufferVirt;
                    status.async.arRequestBuffers.bufferIOVA = snapshot.arRequestBuffers.bufferIOVA;
                    status.async.arRequestBuffers.bufferCount = snapshot.arRequestBuffers.bufferCount;
                    status.async.arRequestBuffers.bufferSize = snapshot.arRequestBuffers.bufferSize;

                    status.async.arResponseBuffers.bufferVirt = snapshot.arResponseBuffers.bufferVirt;
                    status.async.arResponseBuffers.bufferIOVA = snapshot.arResponseBuffers.bufferIOVA;
                    status.async.arResponseBuffers.bufferCount = snapshot.arResponseBuffers.bufferCount;
                    status.async.arResponseBuffers.bufferSize = snapshot.arResponseBuffers.bufferSize;

                    status.async.dmaSlabVirt = snapshot.dmaSlabVirt;
                    status.async.dmaSlabIOVA = snapshot.dmaSlabIOVA;
                    status.async.dmaSlabSize = snapshot.dmaSlabSize;
                }
            }

            OSData* data = OSData::withBytes(&status, sizeof(status));
            if (!data) {
                return kIOReturnNoMemory;
            }

            arguments->structureOutput = data;
            arguments->structureOutputDescriptor = nullptr;

            return kIOReturnSuccess;
        }

        case 3: { // kMethodGetMetricsSnapshot
            // Future: Return IOReporter data
            return kIOReturnUnsupported;
        }

        case 4: { // kMethodClearHistory
            // Clear bus reset packet history
            using namespace ASFW::Async;

            auto* asyncSys = static_cast<AsyncSubsystem*>(driver->GetAsyncSubsystem());
            if (!asyncSys) {
                return kIOReturnSuccess;  // Nothing to clear
            }

            auto* capture = asyncSys->GetBusResetCapture();
            if (capture) {
                capture->Clear();
            }

            return kIOReturnSuccess;
        }

        case 5: { // kMethodGetSelfIDCapture
            // Return Self-ID capture with raw quadlets and sequences
            // Input: generation (optional, 0 = latest)
            // Output: OSData with SelfIDMetricsWire + quadlets + sequences
            
            ASFW_LOG(UserClient, "kMethodGetSelfIDCapture called: arguments=%p", arguments);
            
            if (!arguments) {
                ASFW_LOG(UserClient, "kMethodGetSelfIDCapture - arguments is NULL, returning BadArgument");
                return kIOReturnBadArgument;
            }

            ASFW_LOG(UserClient, "kMethodGetSelfIDCapture - structureInput=%p structureOutput=%p maxSize=%llu",
                     arguments->structureInput,
                     arguments->structureOutput,
                     arguments->structureOutputMaximumSize);

            using namespace ASFW::Driver;

            auto* controller = static_cast<ControllerCore*>(driver->GetControllerCore());
            if (!controller) {
                ASFW_LOG(UserClient, "kMethodGetSelfIDCapture - controller is NULL");
                return kIOReturnNotReady;
            }

            auto topo = controller->LatestTopology();
            if (!topo || !topo->selfIDData.valid) {
                // No valid Self-ID data available
                ASFW_LOG(UserClient, "kMethodGetSelfIDCapture - no valid Self-ID data (topo=%d valid=%d)",
                         topo.has_value() ? 1 : 0,
                         topo.has_value() ? (topo->selfIDData.valid ? 1 : 0) : 0);
                OSData* data = OSData::withCapacity(0);
                if (!data) return kIOReturnNoMemory;
                arguments->structureOutput = data;
                arguments->structureOutputDescriptor = nullptr;
                ASFW_LOG(UserClient, "kMethodGetSelfIDCapture EXIT: setting structureOutput len=0 (no data yet)");
                return kIOReturnSuccess;
            }

            const auto& selfID = topo->selfIDData;
            
            // Calculate total size
            size_t headerSize = sizeof(SelfIDMetricsWire);
            size_t quadletsSize = selfID.rawQuadlets.size() * sizeof(uint32_t);
            size_t sequencesSize = selfID.sequences.size() * sizeof(SelfIDSequenceWire);
            size_t totalSize = headerSize + quadletsSize + sequencesSize;

            OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
            if (!data) return kIOReturnNoMemory;

            // Write header
            SelfIDMetricsWire wire{};
            wire.generation = selfID.generation;
            wire.captureTimestamp = selfID.captureTimestamp;
            wire.quadletCount = static_cast<uint32_t>(selfID.rawQuadlets.size());
            wire.sequenceCount = static_cast<uint32_t>(selfID.sequences.size());
            wire.valid = selfID.valid ? 1 : 0;
            wire.timedOut = selfID.timedOut ? 1 : 0;
            wire.crcError = selfID.crcError ? 1 : 0;
            
            if (selfID.errorReason.has_value()) {
                std::strncpy(wire.errorReason, selfID.errorReason->c_str(), sizeof(wire.errorReason) - 1);
                wire.errorReason[sizeof(wire.errorReason) - 1] = '\0';
            } else {
                wire.errorReason[0] = '\0';
            }

            if (!data->appendBytes(&wire, sizeof(wire))) {
                data->release();
                return kIOReturnNoMemory;
            }

            // Write quadlets
            if (!selfID.rawQuadlets.empty()) {
                if (!data->appendBytes(selfID.rawQuadlets.data(), quadletsSize)) {
                    data->release();
                    return kIOReturnNoMemory;
                }
            }

            // Write sequences
            for (const auto& seq : selfID.sequences) {
                SelfIDSequenceWire seqWire{};
                seqWire.startIndex = static_cast<uint32_t>(seq.first);
                seqWire.quadletCount = seq.second;
                if (!data->appendBytes(&seqWire, sizeof(seqWire))) {
                    data->release();
                    return kIOReturnNoMemory;
                }
            }

            arguments->structureOutput = data;
            arguments->structureOutputDescriptor = nullptr;
            ASFW_LOG(UserClient, "kMethodGetSelfIDCapture EXIT: setting structureOutput len=%zu (gen=%u quads=%u seqs=%u)",
                     data ? data->getLength() : 0, wire.generation, wire.quadletCount, wire.sequenceCount);
            return kIOReturnSuccess;
        }

        case 6: { // kMethodGetTopologySnapshot
            // Return complete topology snapshot with nodes and port states
            // Output: OSData with TopologySnapshotWire + nodes + port states + warnings
            
            ASFW_LOG(UserClient, "kMethodGetTopologySnapshot called: arguments=%p", arguments);
            
            if (!arguments) {
                ASFW_LOG(UserClient, "kMethodGetTopologySnapshot - arguments is NULL, returning BadArgument");
                return kIOReturnBadArgument;
            }

            ASFW_LOG(UserClient, "kMethodGetTopologySnapshot - structureInput=%p structureOutput=%p maxSize=%llu",
                     arguments->structureInput,
                     arguments->structureOutput,
                     arguments->structureOutputMaximumSize);

            using namespace ASFW::Driver;
            // disabled noisy logging for now
            // ASFW_LOG(UserClient, "kMethodGetTopologySnapshot called");

            auto* controller = static_cast<ControllerCore*>(driver->GetControllerCore());
            if (!controller) {
                ASFW_LOG(UserClient, "kMethodGetTopologySnapshot - controller is NULL");
                return kIOReturnNotReady;
            }

            auto topo = controller->LatestTopology();
            if (!topo) {
                // No topology available
                ASFW_LOG(UserClient, "kMethodGetTopologySnapshot - no topology available");
                OSData* data = OSData::withCapacity(0);
                if (!data) return kIOReturnNoMemory;
                arguments->structureOutput = data;
                arguments->structureOutputDescriptor = nullptr;
                ASFW_LOG(UserClient, "kMethodGetTopologySnapshot EXIT: setting structureOutput len=0 (no data yet)");
                return kIOReturnSuccess;
            }

            // ASFW_LOG(UserClient, "kMethodGetTopologySnapshot - returning topology gen=%u nodes=%u",
                    //  topo->generation, topo->nodeCount);

            // Calculate size for variable-length data
            size_t headerSize = sizeof(TopologySnapshotWire);
            size_t nodesBaseSize = topo->nodes.size() * sizeof(TopologyNodeWire);
            
            // Calculate port states size
            size_t portStatesSize = 0;
            for (const auto& node : topo->nodes) {
                portStatesSize += node.portStates.size();
            }
            
            // Calculate warnings size (null-terminated strings)
            size_t warningsSize = 0;
            for (const auto& warning : topo->warnings) {
                warningsSize += warning.length() + 1; // +1 for null terminator
            }
            
            size_t totalSize = headerSize + nodesBaseSize + portStatesSize + warningsSize;

            OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
            if (!data) return kIOReturnNoMemory;

            // Write snapshot header
            TopologySnapshotWire snapWire{};
            snapWire.generation = topo->generation;
            snapWire.capturedAt = topo->capturedAt;
            snapWire.nodeCount = topo->nodeCount;
            snapWire.rootNodeId = topo->rootNodeId.value_or(0xFF);
            snapWire.irmNodeId = topo->irmNodeId.value_or(0xFF);
            snapWire.localNodeId = topo->localNodeId.value_or(0xFF);
            snapWire.gapCount = topo->gapCount;
            snapWire.warningCount = static_cast<uint8_t>(topo->warnings.size());

            if (!data->appendBytes(&snapWire, sizeof(snapWire))) {
                data->release();
                return kIOReturnNoMemory;
            }

            // Write nodes
            for (const auto& node : topo->nodes) {
                TopologyNodeWire nodeWire{};
                nodeWire.nodeId = node.nodeId;
                nodeWire.portCount = node.portCount;
                nodeWire.gapCount = node.gapCount;
                nodeWire.powerClass = node.powerClass;
                nodeWire.maxSpeedMbps = node.maxSpeedMbps;
                nodeWire.isIRMCandidate = node.isIRMCandidate ? 1 : 0;
                nodeWire.linkActive = node.linkActive ? 1 : 0;
                nodeWire.initiatedReset = node.initiatedReset ? 1 : 0;
                nodeWire.isRoot = node.isRoot ? 1 : 0;
                nodeWire.parentPort = node.parentPort.value_or(0xFF);
                nodeWire.portStateCount = static_cast<uint8_t>(node.portStates.size());

                if (!data->appendBytes(&nodeWire, sizeof(nodeWire))) {
                    data->release();
                    return kIOReturnNoMemory;
                }

                // Write port states for this node
                for (auto portState : node.portStates) {
                    uint8_t state = static_cast<uint8_t>(portState);
                    if (!data->appendBytes(&state, sizeof(state))) {
                        data->release();
                        return kIOReturnNoMemory;
                    }
                }
            }

            // Write warnings as null-terminated strings
            for (const auto& warning : topo->warnings) {
                const char* str = warning.c_str();
                size_t len = warning.length() + 1; // Include null terminator
                if (!data->appendBytes(str, len)) {
                    data->release();
                    return kIOReturnNoMemory;
                }
            }

            arguments->structureOutput = data;
            arguments->structureOutputDescriptor = nullptr;
            ASFW_LOG(UserClient, "kMethodGetTopologySnapshot EXIT: setting structureOutput len=%zu (gen=%u nodes=%u root=%u)",
                     data ? data->getLength() : 0, snapWire.generation, snapWire.nodeCount, snapWire.rootNodeId);
            return kIOReturnSuccess;
        }

        case 8: { // kMethodAsyncRead
            // Input: destinationID[16], addressHi[16], addressLo[32], length[32]
            // Output: handle[16]
            if (!arguments || arguments->scalarInputCount < 4 || arguments->scalarOutputCount < 1) {
                return kIOReturnBadArgument;
            }

            const uint16_t destinationID = static_cast<uint16_t>(arguments->scalarInput[0] & 0xFFFF);
            const uint16_t addressHi = static_cast<uint16_t>(arguments->scalarInput[1] & 0xFFFF);
            const uint32_t addressLo = static_cast<uint32_t>(arguments->scalarInput[2] & 0xFFFFFFFFu);
            const uint32_t length = static_cast<uint32_t>(arguments->scalarInput[3] & 0xFFFFFFFFu);

            ASFW_LOG(UserClient, "AsyncRead: dest=0x%04x addr=0x%04x:%08x len=%u",
                     destinationID, addressHi, addressLo, length);

            using namespace ASFW::Async;
            auto* asyncSys = static_cast<AsyncSubsystem*>(driver->GetAsyncSubsystem());
            if (!asyncSys) {
                ASFW_LOG(UserClient, "AsyncRead: AsyncSubsystem not available");
                return kIOReturnNotReady;
            }

            // Build ReadParams
            ReadParams params{};
            params.destinationID = destinationID;
            params.addressHigh = addressHi;
            params.addressLow = addressLo;
            params.length = length;

            // Initiate async read with completion callback
            AsyncHandle handle = asyncSys->Read(params, [this](AsyncHandle handle, AsyncStatus status, std::span<const uint8_t> responsePayload) {
                AsyncTransactionCompletionCallback(handle, status, this, responsePayload.data(), static_cast<uint32_t>(responsePayload.size()));
            });
            if (!handle) {
                ASFW_LOG(UserClient, "AsyncRead: Failed to initiate transaction");
                return kIOReturnError;
            }

            arguments->scalarOutput[0] = handle.value;
            arguments->scalarOutputCount = 1;

            ASFW_LOG(UserClient, "AsyncRead: Initiated with handle=0x%04x (with completion callback)", handle.value);
            return kIOReturnSuccess;
        }

        case 9: { // kMethodAsyncWrite
            // Input: destinationID[16], addressHi[16], addressLo[32], length[32]
            // structureInput: payload data
            // Output: handle[16]
            if (!arguments || arguments->scalarInputCount < 4 || arguments->scalarOutputCount < 1) {
                return kIOReturnBadArgument;
            }

            if (!arguments->structureInput) {
                ASFW_LOG(UserClient, "AsyncWrite: No payload data provided");
                return kIOReturnBadArgument;
            }

            // Get payload from structureInput (OSData) early to validate
            OSData* payloadData = OSDynamicCast(OSData, arguments->structureInput);
            if (!payloadData) {
                ASFW_LOG(UserClient, "AsyncWrite: structureInput is not OSData");
                return kIOReturnBadArgument;
            }

            const uint32_t actualPayloadSize = static_cast<uint32_t>(payloadData->getLength());
            if (actualPayloadSize == 0) {
                ASFW_LOG(UserClient, "AsyncWrite: Empty payload");
                return kIOReturnBadArgument;
            }

            const uint16_t destinationID = static_cast<uint16_t>(arguments->scalarInput[0] & 0xFFFF);
            const uint16_t addressHi = static_cast<uint16_t>(arguments->scalarInput[1] & 0xFFFF);
            const uint32_t addressLo = static_cast<uint32_t>(arguments->scalarInput[2] & 0xFFFFFFFFu);
            const uint32_t length = static_cast<uint32_t>(arguments->scalarInput[3] & 0xFFFFFFFFu);

            if (length != actualPayloadSize) {
                ASFW_LOG(UserClient, "AsyncWrite: Length mismatch (specified=%u actual=%u)",
                         length, actualPayloadSize);
                return kIOReturnBadArgument;
            }

            ASFW_LOG(UserClient, "AsyncWrite: dest=0x%04x addr=0x%04x:%08x len=%u",
                     destinationID, addressHi, addressLo, length);

            using namespace ASFW::Async;
            auto* asyncSys = static_cast<AsyncSubsystem*>(driver->GetAsyncSubsystem());
            if (!asyncSys) {
                ASFW_LOG(UserClient, "AsyncWrite: AsyncSubsystem not available");
                return kIOReturnNotReady;
            }

            // Get payload bytes (payloadData already validated above)
            const void* payload = payloadData->getBytesNoCopy();
            if (!payload) {
                ASFW_LOG(UserClient, "AsyncWrite: Failed to get payload bytes");
                return kIOReturnBadArgument;
            }

            // Build WriteParams
            WriteParams params{};
            params.destinationID = destinationID;
            params.addressHigh = addressHi;
            params.addressLow = addressLo;
            params.payload = payload;
            params.length = length;

            // Initiate async write with completion callback
            AsyncHandle handle = asyncSys->Write(params, [this](AsyncHandle handle, AsyncStatus status, std::span<const uint8_t> responsePayload) {
                AsyncTransactionCompletionCallback(handle, status, this, responsePayload.data(), static_cast<uint32_t>(responsePayload.size()));
            });
            if (!handle) {
                ASFW_LOG(UserClient, "AsyncWrite: Failed to initiate transaction");
                return kIOReturnError;
            }

            arguments->scalarOutput[0] = handle.value;
            arguments->scalarOutputCount = 1;

            ASFW_LOG(UserClient, "AsyncWrite: Initiated with handle=0x%04x (with completion callback)", handle.value);
            return kIOReturnSuccess;
        }

        case 7: { // kMethodPing
            if (!arguments) {
                return kIOReturnBadArgument;
            }

            using namespace ASFW::Driver;

            auto* controller = static_cast<ControllerCore*>(driver->GetControllerCore());
            if (!controller) {
                return kIOReturnNotReady;
            }

            // Touch metrics subsystem to ensure readiness
            const auto& busMetrics = controller->Metrics().BusReset();

            char message[64];
            int written = std::snprintf(message, sizeof(message), "pong (resets=%u)", busMetrics.resetCount);
            if (written < 0) {
                return kIOReturnError;
            }

            const size_t payloadSize = static_cast<size_t>(written) + 1; // include null terminator
            OSData* data = OSData::withBytes(message, static_cast<uint32_t>(payloadSize));
            if (!data) {
                return kIOReturnNoMemory;
            }

            arguments->structureOutput = data;
            arguments->structureOutputDescriptor = nullptr;
            return kIOReturnSuccess;
        }

        case 10: { // kMethodRegisterStatusListener
            if (!arguments || !arguments->completion) {
                return kIOReturnBadArgument;
            }

            if (!ivars || !ivars->driver) {
                return kIOReturnNotReady;
            }

            if (ivars->statusAction) {
                ivars->statusAction->release();
                ivars->statusAction = nullptr;
            }

            arguments->completion->retain();
            ivars->statusAction = arguments->completion;
            ivars->statusRegistered = true;
            ivars->driver->RegisterStatusListener(this);
            return kIOReturnSuccess;
        }

        case 11: { // kMethodCopyStatusSnapshot
            if (!arguments) {
                return kIOReturnBadArgument;
            }

            if (!ivars || !ivars->driver) {
                return kIOReturnNotReady;
            }

            OSDictionary* statusDict = nullptr;
            uint64_t sequence = 0;
            uint64_t timestamp = 0;

            auto kr = ivars->driver->CopyControllerSnapshot(&statusDict, &sequence, &timestamp);
            if (kr != kIOReturnSuccess) {
                return kr;
            }

            if (arguments->scalarOutput && arguments->scalarOutputCount >= 2) {
                arguments->scalarOutput[0] = sequence;
                arguments->scalarOutput[1] = timestamp;
                arguments->scalarOutputCount = 2;
            }

            if (statusDict) {
                statusDict->release();
            }

            return kIOReturnSuccess;
        }

        case 12: { // kMethodGetTransactionResult
            // Input: handle[16]
            // Output: status[32], dataLength[32], data[buffer]
            if (!arguments || arguments->scalarInputCount < 1) {
                return kIOReturnBadArgument;
            }

            if (!ivars->transactionStorage) {
                return kIOReturnNotReady;
            }

            const uint16_t handle = static_cast<uint16_t>(arguments->scalarInput[0] & 0xFFFF);

            auto* storage = static_cast<TransactionStorage*>(ivars->transactionStorage);
            IOLockLock(storage->completedLock);

            // Search for result with matching handle
            TransactionResult* foundResult = nullptr;
            size_t index = storage->completedTail;
            while (index != storage->completedHead) {
                if (storage->completedTransactions[index].handle == handle) {
                    foundResult = &storage->completedTransactions[index];
                    break;
                }
                index = (index + 1) % TransactionStorage::kMaxCompletedTransactions;
            }

            if (!foundResult) {
                IOLockUnlock(storage->completedLock);
                ASFW_LOG(UserClient, "GetTransactionResult: handle=0x%04x not found", handle);
                return kIOReturnNotFound;
            }

            // Copy result to output
            if (arguments->scalarOutput && arguments->scalarOutputCount >= 2) {
                arguments->scalarOutput[0] = foundResult->status;
                arguments->scalarOutput[1] = foundResult->dataLength;
                arguments->scalarOutputCount = 2;
            }

            if (arguments->structureOutput && foundResult->dataLength > 0) {
                OSData* resultData = OSData::withBytes(foundResult->data, foundResult->dataLength);
                if (resultData) {
                    arguments->structureOutput = resultData;
                    arguments->structureOutputDescriptor = nullptr;
                } else {
                    IOLockUnlock(storage->completedLock);
                    return kIOReturnNoMemory;
                }
            }

            ASFW_LOG(UserClient, "GetTransactionResult: handle=0x%04x status=%u len=%u",
                     handle, foundResult->status, foundResult->dataLength);

            // Remove this result from the buffer
            if (index == storage->completedTail) {
                storage->completedTail = (storage->completedTail + 1) % TransactionStorage::kMaxCompletedTransactions;
            }

            IOLockUnlock(storage->completedLock);
            return kIOReturnSuccess;
        }

        case 13: { // kMethodRegisterTransactionListener
            // Register async callback for transaction completion notifications
            if (!arguments || !arguments->completion) {
                return kIOReturnBadArgument;
            }

            if (!ivars || !ivars->driver) {
                return kIOReturnNotReady;
            }

            if (ivars->transactionAction) {
                ivars->transactionAction->release();
                ivars->transactionAction = nullptr;
            }

            arguments->completion->retain();
            ivars->transactionAction = arguments->completion;
            ivars->transactionListenerRegistered = true;

            ASFW_LOG(UserClient, "RegisterTransactionListener: callback registered");
            return kIOReturnSuccess;
        }

        case 14: { // kMethodExportConfigROM
            // Export Config ROM for a given nodeId and generation
            // Input: nodeId[8], generation[16]
            // Output: OSData with ROM quadlets (host byte order)
            if (!arguments || arguments->scalarInputCount < 2) {
                return kIOReturnBadArgument;
            }

            const uint8_t nodeId = static_cast<uint8_t>(arguments->scalarInput[0] & 0xFF);
            const uint16_t generation = static_cast<uint16_t>(arguments->scalarInput[1] & 0xFFFF);

            ASFW_LOG(UserClient, "ExportConfigROM: nodeId=%u gen=%u", nodeId, generation);

            using namespace ASFW::Driver;
            auto* controller = static_cast<ControllerCore*>(driver->GetControllerCore());
            if (!controller) {
                ASFW_LOG(UserClient, "ExportConfigROM: controller is NULL");
                return kIOReturnNotReady;
            }

            // Access ConfigROMStore from ControllerCore
            auto* romStore = controller->GetConfigROMStore();
            if (!romStore) {
                ASFW_LOG(UserClient, "ExportConfigROM: romStore is NULL");
                return kIOReturnNotReady;
            }

            // Lookup ROM by nodeId and generation
            const auto* rom = romStore->FindByNode(generation, nodeId);
            if (!rom) {
                ASFW_LOG(UserClient, "ExportConfigROM: ROM not found for node=%u gen=%u", nodeId, generation);
                // Return empty data to indicate "not cached"
                OSData* data = OSData::withCapacity(0);
                if (!data) return kIOReturnNoMemory;
                arguments->structureOutput = data;
                arguments->structureOutputDescriptor = nullptr;
                return kIOReturnSuccess;
            }

            // Export raw quadlets (already in host byte order in ConfigROM)
            if (rom->rawQuadlets.empty()) {
                ASFW_LOG(UserClient, "ExportConfigROM: ROM found but rawQuadlets empty");
                OSData* data = OSData::withCapacity(0);
                if (!data) return kIOReturnNoMemory;
                arguments->structureOutput = data;
                arguments->structureOutputDescriptor = nullptr;
                return kIOReturnSuccess;
            }

            size_t dataSize = rom->rawQuadlets.size() * sizeof(uint32_t);
            OSData* data = OSData::withBytes(rom->rawQuadlets.data(), static_cast<uint32_t>(dataSize));
            if (!data) {
                return kIOReturnNoMemory;
            }

            ASFW_LOG(UserClient, "ExportConfigROM: returning %zu quadlets (%zu bytes)",
                     rom->rawQuadlets.size(), dataSize);

            arguments->structureOutput = data;
            arguments->structureOutputDescriptor = nullptr;
            return kIOReturnSuccess;
        }

        case 15: { // kMethodTriggerROMRead
            // Manually trigger ROM read for a specific nodeId
            // Input: nodeId[8]
            // Output: status[32] (0=initiated, 1=already_in_progress, 2=failed)
            if (!arguments || arguments->scalarInputCount < 1 || arguments->scalarOutputCount < 1) {
                return kIOReturnBadArgument;
            }

            const uint8_t nodeId = static_cast<uint8_t>(arguments->scalarInput[0] & 0xFF);

            ASFW_LOG(UserClient, "TriggerROMRead: nodeId=%u", nodeId);

            using namespace ASFW::Driver;
            auto* controller = static_cast<ControllerCore*>(driver->GetControllerCore());
            if (!controller) {
                ASFW_LOG(UserClient, "TriggerROMRead: controller is NULL");
                arguments->scalarOutput[0] = 2; // failed
                arguments->scalarOutputCount = 1;
                return kIOReturnNotReady;
            }

            // Get current topology to validate nodeId
            auto topo = controller->LatestTopology();
            if (!topo) {
                ASFW_LOG(UserClient, "TriggerROMRead: no topology available");
                arguments->scalarOutput[0] = 2; // failed
                arguments->scalarOutputCount = 1;
                return kIOReturnError;
            }

            // Validate nodeId exists in topology
            bool nodeExists = false;
            for (const auto& node : topo->nodes) {
                if (node.nodeId == nodeId) {
                    nodeExists = true;
                    break;
                }
            }

            if (!nodeExists) {
                ASFW_LOG(UserClient, "TriggerROMRead: nodeId=%u not in topology", nodeId);
                arguments->scalarOutput[0] = 2; // failed
                arguments->scalarOutputCount = 1;
                return kIOReturnBadArgument;
            }

            // Trigger ROM read via ROMScanner
            auto* romScanner = controller->GetROMScanner();
            if (!romScanner) {
                ASFW_LOG(UserClient, "TriggerROMRead: romScanner is NULL");
                arguments->scalarOutput[0] = 2; // failed
                arguments->scalarOutputCount = 1;
                return kIOReturnError;
            }

            // Request manual ROM read for this node
            bool initiated = romScanner->TriggerManualRead(nodeId, topo->generation, *topo);

            arguments->scalarOutput[0] = initiated ? 0 : 1; // 0=initiated, 1=already_in_progress
            arguments->scalarOutputCount = 1;

            ASFW_LOG(UserClient, "TriggerROMRead: nodeId=%u %{public}s",
                     nodeId, initiated ? "initiated" : "already in progress");

            return kIOReturnSuccess;
        }

        default:
            return kIOReturnBadArgument;
    }
}

kern_return_t ASFWDriverUserClient::AsyncRead(
    uint16_t destinationID,
    uint16_t addressHi,
    uint32_t addressLo,
    uint32_t length,
    uint16_t* handle)
{
    // LOCALONLY method - implementation is in ExternalMethod case 8
    // This should never be called directly
    if (handle) {
        *handle = 0;
    }
    return kIOReturnUnsupported;
}

kern_return_t ASFWDriverUserClient::AsyncWrite(
    uint16_t destinationID,
    uint16_t addressHi,
    uint32_t addressLo,
    uint32_t length,
    const void* payload,
    uint16_t* handle)
{
    // LOCALONLY method - implementation is in ExternalMethod case 9
    // This should never be called directly
    if (handle) {
        *handle = 0;
    }
    return kIOReturnUnsupported;
}

void ASFWDriverUserClient::NotifyStatus(uint64_t sequence,
                                        uint32_t reason)
{
    if (!ivars || !ivars->statusRegistered || !ivars->statusAction) {
        return;
    }

    IOUserClientAsyncArgumentsArray data{};
    data[0] = sequence;
    data[1] = reason;
    AsyncCompletion(ivars->statusAction, kIOReturnSuccess, data, 2);
}

void ASFWDriverUserClient::NotifyTransactionComplete(uint16_t handle,
                                                     uint32_t status)
{
    if (!ivars || !ivars->transactionListenerRegistered || !ivars->transactionAction) {
        return;
    }

    ASFW_LOG(UserClient, "NotifyTransactionComplete: handle=0x%04x status=0x%08x", handle, status);

    IOUserClientAsyncArgumentsArray data{};
    data[0] = handle;
    data[1] = status;
    AsyncCompletion(ivars->transactionAction, kIOReturnSuccess, data, 2);
}

kern_return_t ASFWDriverUserClient::GetTransactionResult(
    uint16_t handle,
    uint32_t* status,
    uint32_t* dataLength,
    void* data,
    uint32_t maxDataLength)
{
    // LOCALONLY method - implementation is in ExternalMethod case 12
    // This should never be called directly
    if (status) *status = 0;
    if (dataLength) *dataLength = 0;
    return kIOReturnUnsupported;
}

kern_return_t ASFWDriverUserClient::CopyClientMemoryForType_Impl(
    uint64_t type,
    uint64_t* options,
    IOMemoryDescriptor** memory)
{
    if (!memory) {
        return kIOReturnBadArgument;
    }

    if (!ivars || !ivars->driver) {
        return kIOReturnNotReady;
    }

    if (type != kSharedStatusMemoryType) {
        return kIOReturnUnsupported;
    }

    return ivars->driver->CopySharedStatusMemory(options, memory);
}
