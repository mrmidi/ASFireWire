//
//  ASFWDriverUserClient.cpp
//  ASFWDriver
//
//  User client for GUI application communication
//  Refactored into handler-based architecture for maintainability
//

#include "ASFWDriverUserClient.h"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Shared/DriverVersionInfo.hpp"
#include "../../Version/DriverVersion.hpp"
#include "ASFWDriver.h"
#include "UserClientRuntimeState.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>

// Method selectors for ExternalMethod (matching .iig definitions)
enum {
    kMethodGetBusResetCount = 0,
    kMethodGetBusResetHistory = 1,
    kMethodGetControllerStatus = 2,
    kMethodGetMetricsSnapshot = 3,
    kMethodClearHistory = 4,
    kMethodGetSelfIDCapture = 5,
    kMethodGetTopologySnapshot = 6,
    kMethodPing = 7,
    kMethodAsyncRead = 8,
    kMethodAsyncWrite = 9,
    kMethodRegisterStatusListener = 10,
    kMethodCopyStatusSnapshot = 11,
    kMethodGetTransactionResult = 12,
    kMethodRegisterTransactionListener = 13,
    kMethodExportConfigROM = 14,
    kMethodTriggerROMRead = 15,
    kMethodGetDiscoveredDevices = 16,
    kMethodAsyncCompareSwap = 17,
    kMethodGetDriverVersion = 18,
    kMethodSetAsyncVerbosity = 19,
    kMethodSetHexDumps = 20,
    kMethodGetLogConfig = 21,
    kMethodGetAVCUnits = 22,
    kMethodGetSubunitCapabilities = 23,
    kMethodGetSubunitDescriptor = 24,
    kMethodReScanAVCUnits = 25,
    kMethodSendRawFCPCommand = 38,
    kMethodGetRawFCPCommandResult = 39,
    kMethodSetIsochVerbosity = 40,
    kMethodSetIsochTxVerifier = 41,
    kMethodSetAudioAutoStart = 42,
    kMethodGetAudioAutoStart = 43,
    kMethodAsyncBlockRead = 44,
    kMethodAsyncBlockWrite = 45,
    // SBP2 address space management
    kMethodAllocateAddressRange = 46,
    kMethodDeallocateAddressRange = 47,
    kMethodReadIncomingData = 48,
    kMethodWriteLocalData = 49,
    // TODO(ASFW-IRM): Remove temporary IRM test method after dedicated validation tooling exists.
    kMethodTestIRMAllocation = 26,
    kMethodTestIRMRelease = 27,
    // TODO(ASFW-CMP): Remove temporary CMP test methods after dedicated validation tooling exists.
    kMethodTestCMPConnectOPCR = 28,
    kMethodTestCMPDisconnectOPCR = 29,
    kMethodTestCMPConnectIPCR = 30,
    kMethodTestCMPDisconnectIPCR = 31,

    // Isoch Stream Control
    kMethodStartIsochReceive = 32,
    kMethodStopIsochReceive = 33,

    // Isoch Metrics
    kMethodGetIsochRxMetrics = 34,
    kMethodResetIsochRxMetrics = 35,

    // Isoch Transmit Control (IT DMA allocation only - no CMP)
    kMethodStartIsochTransmit = 36,
    kMethodStopIsochTransmit = 37,
};

bool ASFWDriverUserClient::init() {
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
    ivars->actionLock = IOLockAlloc();
    if (!ivars->actionLock) {
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
        return false;
    }
    ivars->stopping = false;

    auto* runtimeState = new ASFW::UserClient::UserClientRuntimeState();
    if (!runtimeState || !runtimeState->IsValid()) {
        delete runtimeState;
        if (ivars->actionLock) {
            IOLockFree(ivars->actionLock);
            ivars->actionLock = nullptr;
        }
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
        return false;
    }
    ivars->runtimeState = static_cast<void*>(runtimeState);

    return true;
}

void ASFWDriverUserClient::free() {
    if (ivars) {
        if (ivars->driver && ivars->statusRegistered) {
            ivars->driver->UnregisterStatusListener(this);
        }
        if (ivars->actionLock) {
            IOLockLock(ivars->actionLock);
            ivars->stopping = true;
            if (ivars->statusAction) {
                ivars->statusAction->release();
                ivars->statusAction = nullptr;
            }
            if (ivars->transactionAction) {
                ivars->transactionAction->release();
                ivars->transactionAction = nullptr;
            }
            IOLockUnlock(ivars->actionLock);
            IOLockFree(ivars->actionLock);
            ivars->actionLock = nullptr;
        }

        if (ivars->runtimeState) {
            auto* runtimeState =
                static_cast<ASFW::UserClient::UserClientRuntimeState*>(ivars->runtimeState);
            runtimeState->ReleaseOwner(this);
            delete runtimeState;
            ivars->runtimeState = nullptr;
        }
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(ASFWDriverUserClient, Start) {
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // Store typed reference to driver
    ivars->driver = OSDynamicCast(ASFWDriver, provider);
    if (!ivars->driver) {
        return kIOReturnError;
    }

    if (ivars->actionLock) {
        IOLockLock(ivars->actionLock);
        ivars->stopping = false;
        IOLockUnlock(ivars->actionLock);
    }

    ivars->statusRegistered = false;
    if (ivars->statusAction) {
        ivars->statusAction->release();
        ivars->statusAction = nullptr;
    }

    auto* runtimeState = ASFW::UserClient::GetRuntimeState(this);
    if (!runtimeState || !runtimeState->BindDriver(ivars->driver)) {
        ASFW_LOG(UserClient, "Start() failed to initialize runtime state");
        return kIOReturnNoMemory;
    }

    ASFW_LOG(UserClient, "Start() completed - runtime state initialized");
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWDriverUserClient, Stop) {
    if (ivars && ivars->actionLock) {
        IOLockLock(ivars->actionLock);
        ivars->stopping = true;
        ivars->statusRegistered = false;
        ivars->transactionListenerRegistered = false;
        if (ivars->statusAction) {
            ivars->statusAction->release();
            ivars->statusAction = nullptr;
        }
        if (ivars->transactionAction) {
            ivars->transactionAction->release();
            ivars->transactionAction = nullptr;
        }
        IOLockUnlock(ivars->actionLock);
    }

    if (ivars && ivars->driver) {
        ivars->driver->UnregisterStatusListener(this);
        ivars->driver = nullptr;
    }
    if (auto* runtimeState = ASFW::UserClient::GetRuntimeState(this); runtimeState != nullptr) {
        runtimeState->ReleaseOwner(this);
        runtimeState->ResetHandlers();
    }

    ASFW_LOG(UserClient, "Stop() completed");
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t ASFWDriverUserClient::ExternalMethod(uint64_t selector,
                                                   IOUserClientMethodArguments* arguments,
                                                   const IOUserClientMethodDispatch* dispatch,
                                                   OSObject* target, void* reference) {
    (void)dispatch;
    (void)target;
    (void)reference;

    ASFW_LOG_V3(UserClient, "ExternalMethod called: selector=%llu", selector);

    if (!ivars || !ivars->driver) {
        ASFW_LOG(UserClient, "ExternalMethod: Not ready (ivars=%p driver=%p)", ivars,
                 ivars ? ivars->driver : nullptr);
        return kIOReturnNotReady;
    }

    auto* runtimeState = ASFW::UserClient::GetRuntimeState(this);
    if (runtimeState == nullptr || !runtimeState->HandlersReady()) {
        return kIOReturnNotReady;
    }

    // Simple dispatcher to appropriate handler
    switch (selector) {
    // BusResetHandler methods (0, 1, 4)
    case kMethodGetBusResetCount:
        return runtimeState->BusReset().GetBusResetCount(arguments);

    case kMethodGetBusResetHistory:
        return runtimeState->BusReset().GetBusResetHistory(arguments);

    case kMethodClearHistory:
        return runtimeState->BusReset().ClearHistory(arguments);

    // TopologyHandler methods (5, 6)
    case kMethodGetSelfIDCapture:
        return runtimeState->Topology().GetSelfIDCapture(arguments);

    case kMethodGetTopologySnapshot:
        return runtimeState->Topology().GetTopologySnapshot(arguments);

    // StatusHandler methods (2, 3, 7, 10, 11)
    case kMethodGetControllerStatus:
        return runtimeState->Status().GetControllerStatus(arguments);

    case kMethodGetMetricsSnapshot:
        return runtimeState->Status().GetMetricsSnapshot(arguments);

    case kMethodPing:
        return runtimeState->Status().Ping(arguments);

    case kMethodRegisterStatusListener:
        return runtimeState->Status().RegisterStatusListener(arguments, this);

    case kMethodCopyStatusSnapshot:
        return runtimeState->Status().CopyStatusSnapshot(arguments);

    // TransactionHandler methods (8, 9, 12, 13)
    case kMethodAsyncRead:
        return runtimeState->Transactions().AsyncRead(arguments, this);

    case kMethodAsyncWrite:
        return runtimeState->Transactions().AsyncWrite(arguments, this);

    case kMethodAsyncBlockRead:
        return runtimeState->Transactions().AsyncBlockRead(arguments, this);

    case kMethodAsyncBlockWrite:
        return runtimeState->Transactions().AsyncBlockWrite(arguments, this);

    case kMethodGetTransactionResult:
        return runtimeState->Transactions().GetTransactionResult(arguments);

    case kMethodRegisterTransactionListener:
        return runtimeState->Transactions().RegisterTransactionListener(arguments, this);

    // ConfigROMHandler methods (14, 15)
    case kMethodExportConfigROM:
        return runtimeState->ConfigROM().ExportConfigROM(arguments);

    case kMethodTriggerROMRead:
        return runtimeState->ConfigROM().TriggerROMRead(arguments);

    // DeviceDiscoveryHandler methods (16)
    case kMethodGetDiscoveredDevices:
        return runtimeState->DeviceDiscovery().GetDiscoveredDevices(arguments);

    // AVCHandler methods (22, 23, 24)
    case kMethodGetAVCUnits:
        return runtimeState->AVC().GetAVCUnits(arguments);

    case kMethodGetSubunitCapabilities:
        return runtimeState->AVC().GetSubunitCapabilities(arguments);

    case kMethodGetSubunitDescriptor:
        return runtimeState->AVC().GetSubunitDescriptor(arguments);

    case kMethodReScanAVCUnits:
        return runtimeState->AVC().ReScanAVCUnits(arguments);

    case kMethodSendRawFCPCommand:
        return runtimeState->AVC().SendRawFCPCommand(arguments);

    case kMethodGetRawFCPCommandResult:
        return runtimeState->AVC().GetRawFCPCommandResult(arguments);

    // SBP2 address space management (46-49)
    case kMethodAllocateAddressRange:
        return runtimeState->SBP2().AllocateAddressRange(arguments, this);

    case kMethodDeallocateAddressRange:
        return runtimeState->SBP2().DeallocateAddressRange(arguments, this);

    case kMethodReadIncomingData:
        return runtimeState->SBP2().ReadIncomingData(arguments, this);

    case kMethodWriteLocalData:
        return runtimeState->SBP2().WriteLocalData(arguments, this);

    // TransactionHandler methods - CompareSwap (17)
    case kMethodAsyncCompareSwap:
        return runtimeState->Transactions().AsyncCompareSwap(arguments, this);

    // Version query (18)
    case kMethodGetDriverVersion: {
        ASFW_LOG_V3(UserClient, "GetDriverVersion called");
        ASFW_LOG_V3(UserClient, "  structureOutput=%p", arguments->structureOutput);
        ASFW_LOG_V3(UserClient, "  structureOutputDescriptor=%p",
                    arguments->structureOutputDescriptor);

        // Create version info
        ASFW::Shared::DriverVersionInfo versionInfo{};
        std::strncpy(versionInfo.semanticVersion, ASFW::Version::kSemanticVersion,
                     sizeof(versionInfo.semanticVersion) - 1);
        std::strncpy(versionInfo.gitCommitShort, ASFW::Version::kGitCommitShort,
                     sizeof(versionInfo.gitCommitShort) - 1);
        std::strncpy(versionInfo.gitCommitFull, ASFW::Version::kGitCommitFull,
                     sizeof(versionInfo.gitCommitFull) - 1);
        std::strncpy(versionInfo.gitBranch, ASFW::Version::kGitBranch,
                     sizeof(versionInfo.gitBranch) - 1);
        std::strncpy(versionInfo.buildTimestamp, ASFW::Version::kBuildTimestamp,
                     sizeof(versionInfo.buildTimestamp) - 1);
        std::strncpy(versionInfo.buildHost, ASFW::Version::kBuildHost,
                     sizeof(versionInfo.buildHost) - 1);
        versionInfo.gitDirty = ASFW::Version::kGitDirty;

        ASFW_LOG_V3(UserClient, "  Creating OSData with %zu bytes", sizeof(versionInfo));

        // Create OSData to return structure output
        // Note: structureOutput is initially NULL. We must create and assign the OSData object.
        // The kernel will copy the data from this object to the user's buffer.
        OSData* data = OSData::withBytes(&versionInfo, sizeof(versionInfo));
        if (!data) {
            ASFW_LOG_V0(UserClient, "  OSData::withBytes failed!");
            return kIOReturnNoMemory;
        }

        ASFW_LOG_V3(UserClient, "  OSData created successfully, assigning to structureOutput");
        arguments->structureOutput = data;

        ASFW_LOG_V3(UserClient, "GetDriverVersion: %{public}s", ASFW::Version::kFullVersionString);
        return kIOReturnSuccess;
    }

    // Logging configuration (19, 20, 21, 40)
    case kMethodSetAsyncVerbosity: {
        if (!arguments->scalarInput || arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        uint32_t level = static_cast<uint32_t>(arguments->scalarInput[0]);
        return ivars->driver->SetAsyncVerbosity(level);
    }

    case kMethodSetIsochVerbosity: {
        if (!arguments->scalarInput || arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        uint32_t level = static_cast<uint32_t>(arguments->scalarInput[0]);
        return ivars->driver->SetIsochVerbosity(level);
    }

    case kMethodSetHexDumps: {
        if (!arguments->scalarInput || arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        uint32_t enabled = static_cast<uint32_t>(arguments->scalarInput[0]);
        return ivars->driver->SetHexDumps(enabled);
    }

    case kMethodSetIsochTxVerifier: {
        if (!arguments->scalarInput || arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        uint32_t enabled = static_cast<uint32_t>(arguments->scalarInput[0]);
        return ivars->driver->SetIsochTxVerifier(enabled);
    }

    case kMethodSetAudioAutoStart: {
        if (!arguments->scalarInput || arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        uint32_t enabled = static_cast<uint32_t>(arguments->scalarInput[0]);
        return ivars->driver->SetAudioAutoStart(enabled);
    }

    case kMethodGetAudioAutoStart: {
        if (!arguments->scalarOutput || arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        uint32_t enabled = 0;
        const kern_return_t kr = ivars->driver->GetAudioAutoStart(&enabled);
        if (kr == kIOReturnSuccess) {
            arguments->scalarOutput[0] = enabled;
            arguments->scalarOutputCount = 1;
        }
        return kr;
    }

    case kMethodGetLogConfig: {
        if (!arguments->scalarOutput || arguments->scalarOutputCount < 2) {
            return kIOReturnBadArgument;
        }
        uint32_t asyncVerbosity = 0;
        uint32_t hexDumpsEnabled = 0;
        uint32_t isochVerbosity = 0;
        kern_return_t kr =
            ivars->driver->GetLogConfig(&asyncVerbosity, &hexDumpsEnabled, &isochVerbosity);
        if (kr == kIOReturnSuccess) {
            arguments->scalarOutput[0] = asyncVerbosity;
            arguments->scalarOutput[1] = hexDumpsEnabled;
            if (arguments->scalarOutputCount >= 4) {
                arguments->scalarOutput[2] = isochVerbosity;
                arguments->scalarOutput[3] =
                    ASFW::LogConfig::Shared().IsIsochTxVerifierEnabled() ? 1 : 0;
                arguments->scalarOutputCount = 4;
            } else if (arguments->scalarOutputCount >= 3) {
                arguments->scalarOutput[2] = isochVerbosity;
                arguments->scalarOutputCount = 3;
            } else {
                arguments->scalarOutputCount = 2;
            }
        }
        return kr;
    }

    case kMethodTestIRMAllocation:
        return runtimeState->Isoch().TestIRMAllocation(arguments);

    case kMethodTestIRMRelease:
        return runtimeState->Isoch().TestIRMRelease(arguments);

    case kMethodTestCMPConnectOPCR:
        return runtimeState->Isoch().TestCMPConnectOPCR(arguments);

    case kMethodTestCMPDisconnectOPCR:
        return runtimeState->Isoch().TestCMPDisconnectOPCR(arguments);

    case kMethodTestCMPConnectIPCR:
        return runtimeState->Isoch().TestCMPConnectIPCR(arguments);

    case kMethodTestCMPDisconnectIPCR:
        return runtimeState->Isoch().TestCMPDisconnectIPCR(arguments);

    case kMethodStartIsochReceive:
        return runtimeState->Isoch().StartIsochReceive(arguments);

    case kMethodStopIsochReceive:
        return runtimeState->Isoch().StopIsochReceive(arguments);

    case kMethodGetIsochRxMetrics:
        return runtimeState->Isoch().GetIsochRxMetrics(arguments);

    case kMethodResetIsochRxMetrics:
        return runtimeState->Isoch().ResetIsochRxMetrics(arguments);

    // IT DMA Allocation (no CMP - just allocates memory)
    case kMethodStartIsochTransmit:
        return runtimeState->Isoch().StartIsochTransmit(arguments);

    case kMethodStopIsochTransmit:
        return runtimeState->Isoch().StopIsochTransmit(arguments);

    default:
        return kIOReturnBadArgument;
    }
}

// LOCALONLY user-client ABI entry point.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t ASFWDriverUserClient::AsyncRead(uint16_t destinationID, uint16_t addressHi,
                                              uint32_t addressLo, uint32_t length,
                                              uint16_t* handle) {
    // LOCALONLY method - implementation is in TransactionHandler via ExternalMethod case 8
    // This should never be called directly
    if (handle) {
        *handle = 0;
    }
    return kIOReturnUnsupported;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t ASFWDriverUserClient::AsyncWrite(uint16_t destinationID, uint16_t addressHi,
                                               uint32_t addressLo, uint32_t length,
                                               const void* payload, uint16_t* handle) {
    // LOCALONLY method - implementation is in TransactionHandler via ExternalMethod case 9
    // This should never be called directly
    if (handle) {
        *handle = 0;
    }
    return kIOReturnUnsupported;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t ASFWDriverUserClient::AsyncCompareSwap(uint16_t destinationID, uint16_t addressHi,
                                                     uint32_t addressLo, uint8_t size,
                                                     const void* compareValue, const void* newValue, // NOLINT(bugprone-easily-swappable-parameters)
                                                     uint16_t* handle, uint8_t* locked) {
    // LOCALONLY method - implementation is in TransactionHandler via ExternalMethod case 17
    // This should never be called directly
    if (handle) {
        *handle = 0;
    }
    if (locked) {
        *locked = 0;
    }
    return kIOReturnUnsupported;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ASFWDriverUserClient::NotifyStatus(uint64_t sequence, uint32_t reason) {
    if (!ivars || !ivars->actionLock) {
        return;
    }

    OSAction* action = nullptr;
    IOLockLock(ivars->actionLock);
    if (!ivars->stopping && ivars->statusRegistered && ivars->statusAction) {
        action = ivars->statusAction;
        action->retain();
    }
    IOLockUnlock(ivars->actionLock);

    if (!action) {
        return;
    }

    IOUserClientAsyncArgumentsArray data{};
    data[0] = sequence;
    data[1] = reason;
    AsyncCompletion(action, kIOReturnSuccess, data, 2);
    action->release();
}

void ASFWDriverUserClient::NotifyTransactionComplete(uint16_t handle, uint32_t status) {
    if (!ivars || !ivars->actionLock) {
        return;
    }

    ASFW_LOG(UserClient, "NotifyTransactionComplete: handle=0x%04x status=0x%08x", handle, status);

    OSAction* action = nullptr;
    IOLockLock(ivars->actionLock);
    if (!ivars->stopping && ivars->transactionListenerRegistered && ivars->transactionAction) {
        action = ivars->transactionAction;
        action->retain();
    }
    IOLockUnlock(ivars->actionLock);

    if (!action) {
        return;
    }

    IOUserClientAsyncArgumentsArray data{};
    data[0] = handle;
    data[1] = status;
    AsyncCompletion(action, kIOReturnSuccess, data, 2);
    action->release();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t ASFWDriverUserClient::GetTransactionResult(uint16_t handle, uint32_t* status,
                                                         uint32_t* dataLength, void* data,
                                                         uint32_t maxDataLength) {
    // LOCALONLY method - implementation is in TransactionHandler via ExternalMethod case 12
    // This should never be called directly
    if (status)
        *status = 0;
    if (dataLength)
        *dataLength = 0;
    return kIOReturnUnsupported;
}

kern_return_t IMPL(ASFWDriverUserClient, CopyClientMemoryForType) {
    if (!memory) {
        return kIOReturnBadArgument;
    }

    if (!ivars || !ivars->driver) {
        return kIOReturnNotReady;
    }

    // Only support kSharedStatusMemoryType = 0
    if (type != 0) {
        return kIOReturnUnsupported;
    }

    return ivars->driver->CopySharedStatusMemory(options, memory);
}

// Note: GetDiscoveredDevices is handled in ExternalMethod (selector 16), no stub needed
