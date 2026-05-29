#ifndef ASFW_DIAGNOSTICS_ABI_H
#define ASFW_DIAGNOSTICS_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASFW_DIAG_ABI_VERSION 1u
#define ASFW_DIAG_MAX_NODES 64u
#define ASFW_DIAG_MAX_PORTS 27u
#define ASFW_DIAG_MAX_SELF_ID_QUADS 256u
#define ASFW_DIAG_MAX_ASYNC_EVENTS 128u // Bounded size to keep memory footprint sane
#define ASFW_DIAG_MAX_CSR_ENTRIES 32u
#define ASFW_DIAG_MAX_PHY_REGS 16u

typedef enum ASFWDiagStatus : uint32_t {
    ASFWDiagStatusOK = 0,
    ASFWDiagStatusUnavailable = 1,
    ASFWDiagStatusStaleGeneration = 2,
    ASFWDiagStatusBufferTooSmall = 3,
    ASFWDiagStatusUnsupported = 4,
    ASFWDiagStatusBusy = 5,
    ASFWDiagStatusFailed = 6
} ASFWDiagStatus;

typedef enum ASFWDiagPortState : uint32_t {
    ASFWDiagPortStateInactive = 0,
    ASFWDiagPortStateChild = 1,
    ASFWDiagPortStateParent = 2,
    ASFWDiagPortStateUnknown = 3
} ASFWDiagPortState;

typedef enum ASFWDiagSpeed : uint32_t {
    ASFWDiagSpeedUnknown = 0,
    ASFWDiagSpeedS100 = 1,
    ASFWDiagSpeedS200 = 2,
    ASFWDiagSpeedS400 = 4,
    ASFWDiagSpeedS800 = 8,
    ASFWDiagSpeedS1600 = 16,
    ASFWDiagSpeedS3200 = 32
} ASFWDiagSpeed;

typedef enum ASFWDiagCSROwner : uint32_t {
    ASFWDiagCSROwnerUnknown = 0,
    ASFWDiagCSROwnerOHCIHardware = 1,
    ASFWDiagCSROwnerASFWSoftware = 2,
    ASFWDiagCSROwnerOmittedAddressError = 3,
    ASFWDiagCSROwnerPlanned = 4
} ASFWDiagCSROwner;

typedef struct ASFWDiagHeader {
    uint32_t abiVersion;
    uint32_t structSize;
    uint32_t status;
    uint32_t reserved0;
    uint64_t timestampNs;
    uint32_t generation;
    uint32_t snapshotSeq;
} ASFWDiagHeader;

typedef struct ASFWDiagBusContract {
    ASFWDiagHeader header;
    uint32_t busId;
    uint32_t localNode;
    uint32_t rootNode;
    uint32_t irmNode;
    uint32_t bmNode;
    uint32_t nodeCount;
    uint32_t gapCount;
    uint32_t maxHops;
    uint32_t cycleStartObserved;
    uint32_t cycleStartSourceNode;
    uint32_t localCycleMasterEnabled;
    uint32_t localCycleTimerEnabled;
    uint32_t lastBusResetCount;
    uint32_t rolePolicyMode;
    uint32_t roleVerdict;
    uint32_t reserved1;
} ASFWDiagBusContract;

typedef struct ASFWDiagNode {
    uint32_t nodeId;
    uint32_t rawSelfId;
    uint32_t linkActive;
    uint32_t contender;
    uint32_t speed;
    uint32_t powerClass;
    uint32_t gapCount;
    uint32_t portCount;
    uint32_t ports[ASFW_DIAG_MAX_PORTS];
    uint32_t isLocal;
    uint32_t isRoot;
    uint32_t isIRM;
    uint32_t initiatedReset;
    uint32_t scannable;
    uint32_t reserved0;
} ASFWDiagNode;

typedef struct ASFWDiagTopology {
    ASFWDiagHeader header;
    uint32_t valid;
    uint32_t localNode;
    uint32_t rootNode;
    uint32_t irmNode;
    uint32_t nodeCount;
    uint32_t rawSelfIdCount;
    uint32_t selfIdSequenceCount;
    uint32_t enumeratorError;
    uint32_t rawSelfIds[ASFW_DIAG_MAX_SELF_ID_QUADS];
    ASFWDiagNode nodes[ASFW_DIAG_MAX_NODES];
} ASFWDiagTopology;

typedef struct ASFWDiagRoleCoordinator {
    ASFWDiagHeader header;
    uint32_t policyMode;
    uint32_t lastDecision;
    uint32_t lastAction;
    uint32_t lastActionResult;
    uint32_t localCycleMasterAllowed;
    uint32_t localCycleMasterEnabled;
    uint32_t remoteCMSTRTargetNode;
    uint32_t remoteCMSTRResult;
    uint64_t remoteCMSTRAddress;
    uint32_t remoteCMSTRPayload;
    uint32_t remoteCMSTRRCode;
    uint32_t cycleStartObserved;
    uint32_t cycleStartSourceNode;
    uint32_t resetGuardActive;
    uint32_t bmRetryCount;
    uint32_t gapMismatchDetected;
} ASFWDiagRoleCoordinator;

typedef struct ASFWDiagOHCI {
    ASFWDiagHeader header;
    uint32_t version;
    uint32_t guidROM;
    uint32_t atRetries;
    uint32_t csrData;
    uint32_t csrCompareData;
    uint32_t csrControl;
    uint32_t configROMHeader;
    uint32_t busIdRegister;
    uint32_t busOptions;
    uint32_t guidHi;
    uint32_t guidLo;
    uint32_t configROMMap;
    uint32_t postedWriteAddressLo;
    uint32_t postedWriteAddressHi;
    uint32_t vendorId;
    uint32_t hcControlSet;
    uint32_t hcControlClear;
    uint32_t selfIdBuffer;
    uint32_t selfIdCount;
    uint32_t intEventSet;
    uint32_t intMaskSet;
    uint32_t linkControlSet;
    uint32_t linkControlClear;
    uint32_t nodeId;
    uint32_t phyControl;
    uint32_t isochronousCycleTimer;
} ASFWDiagOHCI;

typedef struct ASFWDiagPHY {
    ASFWDiagHeader header;
    uint32_t regCount;
    uint32_t regs[ASFW_DIAG_MAX_PHY_REGS];
    uint32_t gapCount;
    uint32_t linkOn;
    uint32_t contender;
    uint32_t lastPhyConfigRootId;
    uint32_t lastPhyConfigGapCount;
    uint32_t lastPhyResetReason;
} ASFWDiagPHY;

typedef struct ASFWDiagCSREntry {
    uint64_t address;
    uint32_t offset;
    uint32_t owner;
    uint32_t implemented;
    uint32_t readCount;
    uint32_t writeCount;
    uint32_t lockCount;
    uint32_t lastRCode;
    char name[48];
} ASFWDiagCSREntry;

typedef struct ASFWDiagCSRContract {
    ASFWDiagHeader header;
    uint32_t entryCount;
    uint32_t reserved0;
    ASFWDiagCSREntry entries[ASFW_DIAG_MAX_CSR_ENTRIES];
} ASFWDiagCSRContract;

typedef struct ASFWDiagAsyncEvent {
    uint64_t timestampNs;
    uint32_t generation;
    uint32_t direction;
    uint32_t context;
    uint32_t tLabel;
    uint32_t tCode;
    uint32_t sourceId;
    uint32_t destinationId;
    uint64_t address;
    uint32_t quadletData;
    uint32_t payloadBytes;
    uint32_t ackCode;
    uint32_t rCode;
    uint32_t speed;
    uint32_t matchedTransaction;
    uint32_t dropReason;
} ASFWDiagAsyncEvent;

typedef struct ASFWDiagAsyncTrace {
    ASFWDiagHeader header;
    uint32_t eventCount;
    uint32_t droppedCount;
    ASFWDiagAsyncEvent events[ASFW_DIAG_MAX_ASYNC_EVENTS];
} ASFWDiagAsyncTrace;

typedef struct ASFWDiagInboundCSRStats {
    ASFWDiagHeader header;
    uint32_t inboundConfigROMReads;
    uint32_t inboundStateSetWrites;
    uint32_t inboundStateClearWrites;
    uint32_t inboundBusManagerIdReads;
    uint32_t inboundBusManagerIdLocks;
    uint32_t inboundBandwidthReads;
    uint32_t inboundBandwidthLocks;
    uint32_t inboundChannelReads;
    uint32_t inboundChannelLocks;
    uint32_t inboundBroadcastChannelReads;
    uint32_t inboundBroadcastChannelWrites;
    uint32_t inboundTopologyMapReads;
    uint32_t inboundSpeedMapReads;
    uint32_t unsupportedCSRRequests;
    uint32_t droppedCSRRequests;
    uint32_t reserved0;
} ASFWDiagInboundCSRStats;

#ifdef __cplusplus
}
#endif

#endif // ASFW_DIAGNOSTICS_ABI_H
