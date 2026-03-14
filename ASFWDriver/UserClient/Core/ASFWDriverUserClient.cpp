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
#include <memory>
#include <optional>

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

namespace {

using MethodDispatchResult = std::optional<kern_return_t>;

std::optional<uint32_t> GetFirstScalarInput(const IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount < 1) {
        return std::nullopt;
    }

    return static_cast<uint32_t>(arguments->scalarInput[0]);
}

MethodDispatchResult DispatchBusResetMethods(ASFW::UserClient::UserClientRuntimeState& runtimeState,
                                             IOUserClientMethodArguments* arguments,
                                             uint64_t selector) {
    switch (selector) {
    case kMethodGetBusResetCount:
        return runtimeState.BusReset().GetBusResetCount(arguments);
    case kMethodGetBusResetHistory:
        return runtimeState.BusReset().GetBusResetHistory(arguments);
    case kMethodClearHistory:
        return runtimeState.BusReset().ClearHistory(arguments);
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchTopologyMethods(ASFW::UserClient::UserClientRuntimeState& runtimeState,
                                             IOUserClientMethodArguments* arguments,
                                             uint64_t selector) {
    switch (selector) {
    case kMethodGetSelfIDCapture:
        return runtimeState.Topology().GetSelfIDCapture(arguments);
    case kMethodGetTopologySnapshot:
        return runtimeState.Topology().GetTopologySnapshot(arguments);
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchStatusMethods(ASFW::UserClient::UserClientRuntimeState& runtimeState,
                                           ASFWDriverUserClient& userClient,
                                           IOUserClientMethodArguments* arguments,
                                           uint64_t selector) {
    switch (selector) {
    case kMethodGetControllerStatus:
        return runtimeState.Status().GetControllerStatus(arguments);
    case kMethodGetMetricsSnapshot:
        return runtimeState.Status().GetMetricsSnapshot(arguments);
    case kMethodPing:
        return runtimeState.Status().Ping(arguments);
    case kMethodRegisterStatusListener:
        return runtimeState.Status().RegisterStatusListener(arguments, &userClient);
    case kMethodCopyStatusSnapshot:
        return runtimeState.Status().CopyStatusSnapshot(arguments);
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchTransactionMethods(
    ASFW::UserClient::UserClientRuntimeState& runtimeState, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments, uint64_t selector) {
    switch (selector) {
    case kMethodAsyncRead:
        return runtimeState.Transactions().AsyncRead(arguments, &userClient);
    case kMethodAsyncWrite:
        return runtimeState.Transactions().AsyncWrite(arguments, &userClient);
    case kMethodAsyncBlockRead:
        return runtimeState.Transactions().AsyncBlockRead(arguments, &userClient);
    case kMethodAsyncBlockWrite:
        return runtimeState.Transactions().AsyncBlockWrite(arguments, &userClient);
    case kMethodGetTransactionResult:
        return runtimeState.Transactions().GetTransactionResult(arguments);
    case kMethodRegisterTransactionListener:
        return runtimeState.Transactions().RegisterTransactionListener(arguments, &userClient);
    case kMethodAsyncCompareSwap:
        return runtimeState.Transactions().AsyncCompareSwap(arguments, &userClient);
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchConfigRomMethods(
    ASFW::UserClient::UserClientRuntimeState& runtimeState, IOUserClientMethodArguments* arguments,
    uint64_t selector) {
    switch (selector) {
    case kMethodExportConfigROM:
        return runtimeState.ConfigROM().ExportConfigROM(arguments);
    case kMethodTriggerROMRead:
        return runtimeState.ConfigROM().TriggerROMRead(arguments);
    case kMethodGetDiscoveredDevices:
        return runtimeState.DeviceDiscovery().GetDiscoveredDevices(arguments);
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchAVCMethods(ASFW::UserClient::UserClientRuntimeState& runtimeState,
                                        IOUserClientMethodArguments* arguments,
                                        uint64_t selector) {
    switch (selector) {
    case kMethodGetAVCUnits:
        return runtimeState.AVC().GetAVCUnits(arguments);
    case kMethodGetSubunitCapabilities:
        return runtimeState.AVC().GetSubunitCapabilities(arguments);
    case kMethodGetSubunitDescriptor:
        return runtimeState.AVC().GetSubunitDescriptor(arguments);
    case kMethodReScanAVCUnits:
        return runtimeState.AVC().ReScanAVCUnits(arguments);
    case kMethodSendRawFCPCommand:
        return runtimeState.AVC().SendRawFCPCommand(arguments);
    case kMethodGetRawFCPCommandResult:
        return runtimeState.AVC().GetRawFCPCommandResult(arguments);
    default:
        return std::nullopt;
    }
}

kern_return_t HandleGetDriverVersion(IOUserClientMethodArguments* arguments) {
    ASFW_LOG_V3(UserClient, "GetDriverVersion called");
    ASFW_LOG_V3(UserClient, "  structureOutput=%p", arguments->structureOutput);
    ASFW_LOG_V3(UserClient, "  structureOutputDescriptor=%p",
                arguments->structureOutputDescriptor);

    const ASFW::Shared::DriverVersionInfo versionInfo =
        ASFW::Shared::DriverVersionInfo::Create(ASFW::Version::kSemanticVersion,
                                                ASFW::Version::kGitCommitShort,
                                                ASFW::Version::kGitCommitFull,
                                                ASFW::Version::kGitBranch,
                                                ASFW::Version::kBuildTimestamp,
                                                ASFW::Version::kBuildHost,
                                                ASFW::Version::kGitDirty);

    ASFW_LOG_V3(UserClient, "  Creating OSData with %zu bytes", sizeof(versionInfo));

    OSData* data = OSData::withBytes(&versionInfo, sizeof(versionInfo));
    if (!data) {
        ASFW_LOG_V0(UserClient, "  OSData::withBytes failed!");
        return kIOReturnNoMemory;
    }

    arguments->structureOutput = data;
    ASFW_LOG_V3(UserClient, "GetDriverVersion: %{public}s", ASFW::Version::kFullVersionString);
    return kIOReturnSuccess;
}

MethodDispatchResult DispatchDriverScalarSetters(ASFWDriver& driver,
                                                 IOUserClientMethodArguments* arguments,
                                                 uint64_t selector) {
    const auto value = GetFirstScalarInput(arguments);
    switch (selector) {
    case kMethodSetAsyncVerbosity:
        return value ? MethodDispatchResult{driver.SetAsyncVerbosity(*value)}
                     : MethodDispatchResult{kIOReturnBadArgument};
    case kMethodSetIsochVerbosity:
        return value ? MethodDispatchResult{driver.SetIsochVerbosity(*value)}
                     : MethodDispatchResult{kIOReturnBadArgument};
    case kMethodSetHexDumps:
        return value ? MethodDispatchResult{driver.SetHexDumps(*value)}
                     : MethodDispatchResult{kIOReturnBadArgument};
    case kMethodSetIsochTxVerifier:
        return value ? MethodDispatchResult{driver.SetIsochTxVerifier(*value)}
                     : MethodDispatchResult{kIOReturnBadArgument};
    case kMethodSetAudioAutoStart:
        return value ? MethodDispatchResult{driver.SetAudioAutoStart(*value)}
                     : MethodDispatchResult{kIOReturnBadArgument};
    default:
        return std::nullopt;
    }
}

kern_return_t HandleGetAudioAutoStart(ASFWDriver& driver,
                                      IOUserClientMethodArguments* arguments) {
    if (!arguments->scalarOutput || arguments->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    uint32_t enabled = 0;
    const kern_return_t kr = driver.GetAudioAutoStart(&enabled);
    if (kr == kIOReturnSuccess) {
        arguments->scalarOutput[0] = enabled;
        arguments->scalarOutputCount = 1;
    }
    return kr;
}

kern_return_t HandleGetLogConfig(ASFWDriver& driver,
                                 IOUserClientMethodArguments* arguments) {
    if (!arguments->scalarOutput || arguments->scalarOutputCount < 2) {
        return kIOReturnBadArgument;
    }

    uint32_t asyncVerbosity = 0;
    uint32_t hexDumpsEnabled = 0;
    uint32_t isochVerbosity = 0;
    const kern_return_t kr =
        driver.GetLogConfig(&asyncVerbosity, &hexDumpsEnabled, &isochVerbosity);
    if (kr != kIOReturnSuccess) {
        return kr;
    }

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

    return kr;
}

MethodDispatchResult DispatchDriverControlMethods(ASFWDriver& driver,
                                                  IOUserClientMethodArguments* arguments,
                                                  uint64_t selector) {
    if (selector == kMethodGetDriverVersion) {
        return HandleGetDriverVersion(arguments);
    }

    if (const auto setterResult =
            DispatchDriverScalarSetters(driver, arguments, selector);
        setterResult.has_value()) {
        return setterResult;
    }

    switch (selector) {
    case kMethodGetAudioAutoStart:
        return HandleGetAudioAutoStart(driver, arguments);
    case kMethodGetLogConfig:
        return HandleGetLogConfig(driver, arguments);
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchIsochMethods(ASFW::UserClient::UserClientRuntimeState& runtimeState,
                                          IOUserClientMethodArguments* arguments,
                                          uint64_t selector) {
    switch (selector) {
    case kMethodTestIRMAllocation:
        return runtimeState.Isoch().TestIRMAllocation(arguments);
    case kMethodTestIRMRelease:
        return runtimeState.Isoch().TestIRMRelease(arguments);
    case kMethodTestCMPConnectOPCR:
        return runtimeState.Isoch().TestCMPConnectOPCR(arguments);
    case kMethodTestCMPDisconnectOPCR:
        return runtimeState.Isoch().TestCMPDisconnectOPCR(arguments);
    case kMethodTestCMPConnectIPCR:
        return runtimeState.Isoch().TestCMPConnectIPCR(arguments);
    case kMethodTestCMPDisconnectIPCR:
        return runtimeState.Isoch().TestCMPDisconnectIPCR(arguments);
    case kMethodStartIsochReceive:
        return runtimeState.Isoch().StartIsochReceive(arguments);
    case kMethodStopIsochReceive:
        return runtimeState.Isoch().StopIsochReceive(arguments);
    case kMethodGetIsochRxMetrics:
        return runtimeState.Isoch().GetIsochRxMetrics(arguments);
    case kMethodResetIsochRxMetrics:
        return runtimeState.Isoch().ResetIsochRxMetrics(arguments);
    case kMethodStartIsochTransmit:
        return runtimeState.Isoch().StartIsochTransmit(arguments);
    case kMethodStopIsochTransmit:
        return runtimeState.Isoch().StopIsochTransmit(arguments);
    default:
        return std::nullopt;
    }
}

} // namespace

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

    auto runtimeState = std::make_unique<ASFW::UserClient::UserClientRuntimeState>();
    if (!runtimeState || !runtimeState->IsValid()) {
        if (ivars->actionLock) {
            IOLockFree(ivars->actionLock);
            ivars->actionLock = nullptr;
        }
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
        return false;
    }
    ivars->runtimeState = runtimeState.release();

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
            auto runtimeState =
                std::unique_ptr<ASFW::UserClient::UserClientRuntimeState>(
                    static_cast<ASFW::UserClient::UserClientRuntimeState*>(ivars->runtimeState));
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

    if (auto result = DispatchBusResetMethods(*runtimeState, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchTopologyMethods(*runtimeState, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchStatusMethods(*runtimeState, *this, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchTransactionMethods(*runtimeState, *this, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchConfigRomMethods(*runtimeState, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchAVCMethods(*runtimeState, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchDriverControlMethods(*ivars->driver, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchIsochMethods(*runtimeState, arguments, selector)) {
        return *result;
    }

    return kIOReturnBadArgument;
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
                                               const uint8_t* payload, uint16_t* handle) {
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
                                                     const uint8_t* compareValue, const uint8_t* newValue, // NOLINT(bugprone-easily-swappable-parameters)
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
                                                         uint32_t* dataLength, uint8_t* data,
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
