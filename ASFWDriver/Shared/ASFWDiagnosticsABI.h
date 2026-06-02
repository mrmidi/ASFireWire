#ifndef ASFW_DIAGNOSTICS_ABI_H
#define ASFW_DIAGNOSTICS_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASFW_DIAG_ABI_VERSION 10u
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

// Mirrors the IEEE 1394 Self-ID 2-bit port status field 1:1 (see Linux
// phy-packet-definitions.h and Apple IOFireWireController.h, which agree):
//   00=not present, 01=not connected, 10=parent, 11=child.
// Values MUST match ASFW::Driver::PortState so the driver-side translation in
// DiagnosticsService stays a straight mapping (asserted there).
typedef enum ASFWDiagPortState : uint32_t {
    ASFWDiagPortStateNotPresent = 0,
    ASFWDiagPortStateNotConnected = 1,
    ASFWDiagPortStateParent = 2,
    ASFWDiagPortStateChild = 3
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
    uint32_t asfwInitiatedResetCount;
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
    // Index of the port whose reported state is Parent (toward root), or
    // 0xFFFFFFFF if this node has no parent (i.e. it is the root).
    uint32_t parentPort;
    // Reported 2-bit port state per port (ASFWDiagPortState). Only the first
    // portCount entries are meaningful; the remainder are NotPresent (0).
    uint32_t ports[ASFW_DIAG_MAX_PORTS];
    // Physical adjacency per port, parallel to ports[]: bits [15:8] = remote
    // node id, bits [7:0] = remote port. 0xFFFFFFFF means the port is not
    // connected. Lets the app draw the bus tree without inferring adjacency
    // from Self-ID ordering.
    uint32_t links[ASFW_DIAG_MAX_PORTS];
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
    // Observed bus gap_count (max of the Self-ID gap_count fields).
    uint32_t gapCount;
    // Bus base address component (bus << 6), ready to OR with a node id to form
    // the 16-bit node address. Carried so the app need not recompute it.
    uint32_t busBase16;
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

// Post-reset timing gates (IEEE 1394-2008 §8.x / Annex H), anchored to Self-ID
// completion. Generation-scoped: a newer bus reset invalidates the gates until
// the next Self-ID completion. Reporting only — the driver takes no bus action
// from these gates in this milestone.
typedef struct ASFWDiagPostResetTiming {
    ASFWDiagHeader header;
    uint32_t selfIdComplete; // 1 once Self-ID completion has armed the gates
    uint32_t generation;     // generation the gates are anchored to
    uint64_t selfIdCompleteNs; // monotonic ns of Self-ID completion (0 if none)
    uint64_t nowNs;            // monotonic ns when this snapshot was taken
    uint64_t ageSinceSelfIdNs; // nowNs - selfIdCompleteNs (0 if not armed)
    // Gate states. Values are TimingGateState: 0=Unknown 1=Closed 2=Open
    // 3=ExpiredGeneration 4=SuppressedByRolePolicy 5=SuppressedByTopology.
    uint32_t incumbentBMGate;
    uint32_t nonIncumbentBMGate;
    uint32_t irmFallbackGate;
    uint32_t newIsoAllocationGate;
    // Time until each still-closed gate opens, in ns (0 when already open).
    uint64_t nonIncumbentBMRemainingNs;
    uint64_t irmFallbackRemainingNs;
    uint64_t newIsoAllocationRemainingNs;
    // BMCandidateClass for the local node: 0=NotCandidate 1=Incumbent
    // 2=NonIncumbent. Display only; an Open BM gate with NotCandidate means the
    // local node will not contend (role policy suppresses it).
    uint32_t bmCandidateClass;
    // Counters incremented by future timer/action consumers (0 in this milestone).
    uint32_t staleTimerFirings;
    uint32_t suppressedByGeneration;
    uint32_t suppressedByRolePolicy;
} ASFWDiagPostResetTiming;

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
    // Bit i set => regs[i] was read successfully (OHCI rdDone, no regAccessFail/timeout).
    // Distinguishes a genuine 0xFF (e.g. isolated PHY: physical_id=63) from a failed read.
    uint32_t regValidMask;
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

typedef struct ASFWDiagBusManager {
    ASFWDiagHeader header;
    uint32_t roleMode;
    uint32_t advertisedBmc;
    uint32_t advertisedIrmc;
    uint32_t advertisedCmc;
    uint32_t advertisedIsc;
    
    // Election / Runtime State (C Types)
    uint32_t localIsIRM;
    uint32_t localIsBM;
    uint32_t localIsRoot;
    uint32_t bmOwnerSource;
    uint32_t lastBusManagerIdOldValue;
    uint32_t staleElectionAbortCount;
    uint32_t failedElectionCount;
    uint32_t unexpectedResourceCsrSoftwareCount;

    // Local IRM resource registers
    uint32_t localIrmBusManagerId;
    uint32_t localIrmBandwidthAvailable;
    uint32_t localIrmChannelsAvailableHi;
    uint32_t localIrmChannelsAvailableLo;

    // Topology Map Service status
    uint32_t topologyMapValid;
    uint32_t topologyMapCSRGeneration;
    uint32_t topologyMapSelfIdCount;
    uint32_t topologyMapCRC;
    uint32_t topologyMapDMAReady;

    // BM evidence pipeline fields
    uint32_t rootCmcKnown;
    uint32_t rootCmcCapable;
    uint32_t cycleStartObserved;
    uint32_t cycleStartSourceNode;
    uint32_t remoteCmstrNeeded;
    uint32_t remoteCmstrAllowed;
    uint32_t remoteCmstrAlreadySatisfied;
    uint32_t bmPolicyVerdict;

    // Local IRM resource controller status
    uint32_t localIrmResourceState;
    uint32_t localIrmReadbackValid;
    uint32_t csrControlLastStatus;
    uint32_t fullBMActivityLevel;
    uint32_t lastRemoteCmstrResult;
    uint32_t lastRemoteCmstrGeneration;
    uint32_t lastRemoteCmstrTargetNode;
    
    // Milestone 1 additions
    uint32_t broadcastChannelValue;
    uint32_t broadcastChannelValid;
    uint32_t initialBandwidthAvailable;
    uint32_t initialChannelsAvailableHi;
    uint32_t initialChannelsAvailableLo;

    // Milestone 3 additions: Bus Manager Election State
    uint32_t bmElectionState;
    uint32_t bmElectionResultKind;
    uint32_t bmElectionLocalFlag;
    uint32_t bmElectionAction;
    uint32_t bmElectionPath; // 0=none, 1=Local CSRControl, 2=Remote async lock
    uint32_t bmElectionCompareValue;
    uint32_t bmElectionSwapValue;
    uint32_t bmCandidateClass;
    uint32_t bmElectionAttemptedGen;
    uint32_t bmElectionAttemptsThisGen;

    // Milestone 4: IRM Fallback Planner
    uint32_t irmFallbackState;
    uint32_t irmFallbackPlannedAction;
    uint32_t irmFallbackProbeStatus;
    uint32_t irmFallbackRawBusManagerId;
    uint32_t irmFallbackAnnexHGateOpen;
    uint32_t irmFallbackRemainingMs;

    // Milestone 5: Cycle Master Policy
    uint32_t cyclePolicyDecision;
    uint32_t cyclePolicyAction;
    uint32_t cyclePolicyTargetNode;
    uint32_t cyclePolicyLocalLowLevelMasterBefore;
    uint32_t cyclePolicyLocalLowLevelMasterAfter;
    uint32_t cyclePolicyRemoteCmstrInFlight;
    uint32_t cyclePolicyRemoteCmstrStatus;
    uint32_t cyclePolicyLocalEnableCount;
    uint32_t cyclePolicyRemoteSubmitCount;

    // Milestone 6: Root Selection Policy
    uint32_t rootSelectionDecision;
    uint32_t rootSelectionAction;
    uint32_t rootSelectionSelectedRoot;
    uint32_t rootSelectionPreviousRoot;
    uint32_t rootSelectionAttemptsThisTopology;
    uint32_t rootSelectionTotalAttempts;
    uint32_t rootSelectionRetryLimitHit;
    uint32_t rootSelectionResetRequested;
    uint32_t rootSelectionCurrentGap;
    uint32_t rootSelectionRequestedGap;

    // Milestone 7: Gap Count Policy
    uint32_t gapPolicyDecision;
    uint32_t gapPolicyAction;
    uint32_t gapPolicyCurrentGap;
    uint32_t gapPolicyExpectedGap;
    uint32_t gapPolicyRequestedGap;
    uint32_t gapPolicyComputationSource;
    uint32_t gapPolicyMaxHops;
    uint32_t gapPolicyMaxHopsKnown;
    uint32_t gapPolicyGapConsistent;
    uint32_t gapPolicyBetaKnown;
    uint32_t gapPolicyBetaPresent;
    uint32_t gapPolicyResetRequested;
    uint32_t gapPolicyCombinedWithRootSelection;
    uint32_t gapPolicyTargetRoot;
    uint32_t gapPolicyAttemptsThisTopology;
    uint32_t gapPolicyTotalAttempts;
    uint32_t gapPolicyRetryLimitHit;

    // Milestone 8: Power / Link-On Policy
    uint32_t powerPolicyDecision;
    uint32_t powerPolicyAction;
    uint32_t powerBudgetStatus;
    uint32_t powerEligibleNodeCount;
    uint32_t powerTargetNodeCount;
    uint32_t powerTargetNodes[16];
    uint32_t linkOnSubmittedCount;
    uint32_t linkOnSuccessCount;
    uint32_t linkOnFailureCount;
    uint32_t linkOnAttemptsThisGeneration;
    uint32_t linkOnTotalAttempts;

    // Milestone 9: CSR Compliance / Maps
    uint32_t topologyMapPublishStatus;
    uint32_t topologyMapGeneration;
    uint32_t topologyMapLengthQuadlets;

    uint32_t speedMapStatus;
    uint32_t speedMapGeneration;
    uint32_t speedMapNodeCount;
    uint32_t speedMapEncodedQuadlets;
    uint32_t speedMapBetaKnown;
    uint32_t speedMapBetaPresent;

    uint32_t csrContractVerdict;
    uint32_t csrSoftwareAnsweredHardwareOwned;
    uint32_t csrHardwareOwnedSoftwareHits;
    uint32_t csrUnsupportedAccesses;

    // Former reserved[0]; keep at the tail to preserve existing field offsets.
    uint32_t cyclePolicyLocalClearCount;
} ASFWDiagBusManager;

#ifdef __cplusplus
}
#endif

#endif // ASFW_DIAGNOSTICS_ABI_H
