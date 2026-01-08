//
//  ASFWDriverUserClient.cpp
//  ASFWDriver
//
//  User client for GUI application communication
//  Refactored into handler-based architecture for maintainability
//

#include "ASFWDriverUserClient.h"
#include "ASFWDriver.h"
#include "../../Controller/ControllerCore.hpp"
#include "../Handlers/BusResetHandler.hpp"
#include "../Handlers/TopologyHandler.hpp"
#include "../Handlers/StatusHandler.hpp"
#include "../Handlers/TransactionHandler.hpp"
#include "../Handlers/ConfigROMHandler.hpp"
#include "../Handlers/DeviceDiscoveryHandler.hpp"
#include "../Handlers/AVCHandler.hpp"
#include "../Handlers/IsochHandler.hpp"
#include "../Storage/TransactionStorage.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Shared/DriverVersionInfo.hpp"
#include "../../Version/DriverVersion.hpp"
#include "../../IRM/IRMClient.hpp"
#include "../../Protocols/AVC/CMP/CMPClient.hpp"


#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>

// Method selectors for ExternalMethod (matching .iig definitions)
enum {
    kMethodGetBusResetCount        = 0,
    kMethodGetBusResetHistory      = 1,
    kMethodGetControllerStatus     = 2,
    kMethodGetMetricsSnapshot      = 3,
    kMethodClearHistory            = 4,
    kMethodGetSelfIDCapture        = 5,
    kMethodGetTopologySnapshot     = 6,
    kMethodPing                    = 7,
    kMethodAsyncRead               = 8,
    kMethodAsyncWrite              = 9,
    kMethodRegisterStatusListener  = 10,
    kMethodCopyStatusSnapshot      = 11,
    kMethodGetTransactionResult    = 12,
    kMethodRegisterTransactionListener = 13,
    kMethodExportConfigROM         = 14,
    kMethodTriggerROMRead          = 15,
    kMethodGetDiscoveredDevices    = 16,
    kMethodAsyncCompareSwap        = 17,
    kMethodGetDriverVersion        = 18,
    kMethodSetAsyncVerbosity       = 19,
    kMethodSetHexDumps             = 20,
    kMethodGetLogConfig            = 21,
    kMethodGetAVCUnits             = 22,
    kMethodGetSubunitCapabilities  = 23,
    kMethodGetSubunitDescriptor    = 24,
    kMethodReScanAVCUnits          = 25,
    // TODO: IRM test method - temporary location for Phase 0.5 testing
    kMethodTestIRMAllocation       = 26,
    kMethodTestIRMRelease          = 27,
    // TODO: CMP test methods - temporary location for Phase 0.5 testing
    kMethodTestCMPConnectOPCR      = 28,
    kMethodTestCMPDisconnectOPCR   = 29,
    kMethodTestCMPConnectIPCR      = 30,
    kMethodTestCMPDisconnectIPCR   = 31,
    
    // Isoch Stream Control
    kMethodStartIsochReceive       = 32,
    kMethodStopIsochReceive        = 33,
    
    // Isoch Metrics
    kMethodGetIsochRxMetrics       = 34,
    kMethodResetIsochRxMetrics     = 35,
    
    // Isoch Transmit Control (IT DMA allocation only - no CMP)
    kMethodStartIsochTransmit      = 36,
    kMethodStopIsochTransmit       = 37,
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
    ivars->actionLock = IOLockAlloc();
    if (!ivars->actionLock) {
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
        return false;
    }
    ivars->stopping = false;

    // Allocate transaction storage
    auto* storage = new ASFW::UserClient::TransactionStorage();
    if (!storage || !storage->IsValid()) {
        delete storage;
        if (ivars->actionLock) {
            IOLockFree(ivars->actionLock);
            ivars->actionLock = nullptr;
        }
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
        return false;
    }
    ivars->transactionStorage = static_cast<void*>(storage);

    // Handlers will be created in Start_Impl once we have the driver reference
    ivars->busResetHandler = nullptr;
    ivars->topologyHandler = nullptr;
    ivars->statusHandler = nullptr;
    ivars->transactionHandler = nullptr;
    ivars->configROMHandler = nullptr;
    ivars->deviceDiscoveryHandler = nullptr;
    ivars->avcHandler = nullptr;
    ivars->isochHandler = nullptr;
    
    return true;
}

void ASFWDriverUserClient::free()
{
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

        // Delete handlers
        delete static_cast<ASFW::UserClient::BusResetHandler*>(ivars->busResetHandler);
        delete static_cast<ASFW::UserClient::TopologyHandler*>(ivars->topologyHandler);
        delete static_cast<ASFW::UserClient::StatusHandler*>(ivars->statusHandler);
        delete static_cast<ASFW::UserClient::TransactionHandler*>(ivars->transactionHandler);
        delete static_cast<ASFW::UserClient::ConfigROMHandler*>(ivars->configROMHandler);
        delete static_cast<ASFW::UserClient::DeviceDiscoveryHandler*>(ivars->deviceDiscoveryHandler);
        delete static_cast<ASFW::UserClient::AVCHandler*>(ivars->avcHandler);
        delete static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler);
        
        if (ivars->transactionStorage) {
            delete static_cast<ASFW::UserClient::TransactionStorage*>(ivars->transactionStorage);
            ivars->transactionStorage = nullptr;
        }
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(ASFWDriverUserClient, Start)
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

    // Create handlers now that we have driver reference
    using namespace ASFW::UserClient;
    ivars->busResetHandler = static_cast<void*>(new BusResetHandler(ivars->driver));
    ivars->topologyHandler = static_cast<void*>(new TopologyHandler(ivars->driver));
    ivars->statusHandler = static_cast<void*>(new StatusHandler(ivars->driver));
    ivars->transactionHandler = static_cast<void*>(new TransactionHandler(
        ivars->driver,
        static_cast<TransactionStorage*>(ivars->transactionStorage)));
    ivars->configROMHandler = static_cast<void*>(new ConfigROMHandler(ivars->driver));
    ivars->deviceDiscoveryHandler = static_cast<void*>(new DeviceDiscoveryHandler(ivars->driver));

    // Get AVCDiscovery for AVCHandler
    auto* controllerCore = static_cast<ASFW::Driver::ControllerCore*>(ivars->driver->GetControllerCore());
    auto* avcDiscovery = controllerCore ? controllerCore->GetAVCDiscovery() : nullptr;
    ivars->avcHandler = static_cast<void*>(new AVCHandler(avcDiscovery));
    ivars->isochHandler = static_cast<void*>(new IsochHandler(ivars->driver));

    if (!ivars->busResetHandler || !ivars->topologyHandler ||
        !ivars->statusHandler || !ivars->transactionHandler ||
        !ivars->configROMHandler || !ivars->deviceDiscoveryHandler ||
        !ivars->avcHandler || !ivars->isochHandler) {
        ASFW_LOG(UserClient, "Start() failed to create handlers");
        return kIOReturnNoMemory;
    }

    ASFW_LOG(UserClient, "Start() completed - handlers initialized");
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWDriverUserClient, Stop)
{
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

    ASFW_LOG_V3(UserClient, "ExternalMethod called: selector=%llu", selector);

    if (!ivars || !ivars->driver) {
        ASFW_LOG(UserClient, "ExternalMethod: Not ready (ivars=%p driver=%p)", ivars, ivars ? ivars->driver : nullptr);
        return kIOReturnNotReady;
    }

    // Verify handlers are initialized
    if (!ivars->busResetHandler || !ivars->topologyHandler ||
        !ivars->statusHandler || !ivars->transactionHandler ||
        !ivars->configROMHandler || !ivars->deviceDiscoveryHandler ||
        !ivars->avcHandler) {
        return kIOReturnNotReady;
    }

    // Simple dispatcher to appropriate handler
    switch (selector) {
        // BusResetHandler methods (0, 1, 4)
        case kMethodGetBusResetCount:
            return static_cast<ASFW::UserClient::BusResetHandler*>(ivars->busResetHandler)->GetBusResetCount(arguments);

        case kMethodGetBusResetHistory:
            return static_cast<ASFW::UserClient::BusResetHandler*>(ivars->busResetHandler)->GetBusResetHistory(arguments);

        case kMethodClearHistory:
            return static_cast<ASFW::UserClient::BusResetHandler*>(ivars->busResetHandler)->ClearHistory(arguments);

        // TopologyHandler methods (5, 6)
        case kMethodGetSelfIDCapture:
            return static_cast<ASFW::UserClient::TopologyHandler*>(ivars->topologyHandler)->GetSelfIDCapture(arguments);

        case kMethodGetTopologySnapshot:
            return static_cast<ASFW::UserClient::TopologyHandler*>(ivars->topologyHandler)->GetTopologySnapshot(arguments);

        // StatusHandler methods (2, 3, 7, 10, 11)
        case kMethodGetControllerStatus:
            return static_cast<ASFW::UserClient::StatusHandler*>(ivars->statusHandler)->GetControllerStatus(arguments);

        case kMethodGetMetricsSnapshot:
            return static_cast<ASFW::UserClient::StatusHandler*>(ivars->statusHandler)->GetMetricsSnapshot(arguments);

        case kMethodPing:
            return static_cast<ASFW::UserClient::StatusHandler*>(ivars->statusHandler)->Ping(arguments);

        case kMethodRegisterStatusListener:
            return static_cast<ASFW::UserClient::StatusHandler*>(ivars->statusHandler)->RegisterStatusListener(arguments, this);

        case kMethodCopyStatusSnapshot:
            return static_cast<ASFW::UserClient::StatusHandler*>(ivars->statusHandler)->CopyStatusSnapshot(arguments);

        // TransactionHandler methods (8, 9, 12, 13)
        case kMethodAsyncRead:
            return static_cast<ASFW::UserClient::TransactionHandler*>(ivars->transactionHandler)->AsyncRead(arguments, this);

        case kMethodAsyncWrite:
            return static_cast<ASFW::UserClient::TransactionHandler*>(ivars->transactionHandler)->AsyncWrite(arguments, this);

        case kMethodGetTransactionResult:
            return static_cast<ASFW::UserClient::TransactionHandler*>(ivars->transactionHandler)->GetTransactionResult(arguments);

        case kMethodRegisterTransactionListener:
            return static_cast<ASFW::UserClient::TransactionHandler*>(ivars->transactionHandler)->RegisterTransactionListener(arguments, this);

        // ConfigROMHandler methods (14, 15)
        case kMethodExportConfigROM:
            return static_cast<ASFW::UserClient::ConfigROMHandler*>(ivars->configROMHandler)->ExportConfigROM(arguments);

        case kMethodTriggerROMRead:
            return static_cast<ASFW::UserClient::ConfigROMHandler*>(ivars->configROMHandler)->TriggerROMRead(arguments);

        // DeviceDiscoveryHandler methods (16)
        case kMethodGetDiscoveredDevices:
            return static_cast<ASFW::UserClient::DeviceDiscoveryHandler*>(ivars->deviceDiscoveryHandler)->GetDiscoveredDevices(arguments);

        // AVCHandler methods (22, 23, 24)
        case kMethodGetAVCUnits:
            return static_cast<ASFW::UserClient::AVCHandler*>(ivars->avcHandler)->GetAVCUnits(arguments);

        case kMethodGetSubunitCapabilities:
            return static_cast<ASFW::UserClient::AVCHandler*>(ivars->avcHandler)->GetSubunitCapabilities(arguments);

        case kMethodGetSubunitDescriptor:
            return static_cast<ASFW::UserClient::AVCHandler*>(ivars->avcHandler)->GetSubunitDescriptor(arguments);

        case kMethodReScanAVCUnits:
            return static_cast<ASFW::UserClient::AVCHandler*>(ivars->avcHandler)->ReScanAVCUnits(arguments);

        // TransactionHandler methods - CompareSwap (17)
        case kMethodAsyncCompareSwap:
            return static_cast<ASFW::UserClient::TransactionHandler*>(ivars->transactionHandler)->AsyncCompareSwap(arguments, this);

        // Version query (18)
        case kMethodGetDriverVersion: {
            ASFW_LOG_V3(UserClient, "GetDriverVersion called");
            ASFW_LOG_V3(UserClient, "  structureOutput=%p", arguments->structureOutput);
            ASFW_LOG_V3(UserClient, "  structureOutputDescriptor=%p", arguments->structureOutputDescriptor);
            
            // Create version info
            ASFW::Shared::DriverVersionInfo versionInfo{};
            std::strncpy(versionInfo.semanticVersion, ASFW::Version::kSemanticVersion, sizeof(versionInfo.semanticVersion) - 1);
            std::strncpy(versionInfo.gitCommitShort, ASFW::Version::kGitCommitShort, sizeof(versionInfo.gitCommitShort) - 1);
            std::strncpy(versionInfo.gitCommitFull, ASFW::Version::kGitCommitFull, sizeof(versionInfo.gitCommitFull) - 1);
            std::strncpy(versionInfo.gitBranch, ASFW::Version::kGitBranch, sizeof(versionInfo.gitBranch) - 1);
            std::strncpy(versionInfo.buildTimestamp, ASFW::Version::kBuildTimestamp, sizeof(versionInfo.buildTimestamp) - 1);
            std::strncpy(versionInfo.buildHost, ASFW::Version::kBuildHost, sizeof(versionInfo.buildHost) - 1);
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

        // Logging configuration (19, 20, 21)
        case kMethodSetAsyncVerbosity: {
            if (!arguments->scalarInput || arguments->scalarInputCount < 1) {
                return kIOReturnBadArgument;
            }
            uint32_t level = static_cast<uint32_t>(arguments->scalarInput[0]);
            return ivars->driver->SetAsyncVerbosity(level);
        }

        case kMethodSetHexDumps: {
            if (!arguments->scalarInput || arguments->scalarInputCount < 1) {
                return kIOReturnBadArgument;
            }
            uint32_t enabled = static_cast<uint32_t>(arguments->scalarInput[0]);
            return ivars->driver->SetHexDumps(enabled);
        }

        case kMethodGetLogConfig: {
            if (!arguments->scalarOutput) {
                return kIOReturnBadArgument;
            }
            uint32_t asyncVerbosity = 0;
            uint32_t hexDumpsEnabled = 0;
            kern_return_t kr = ivars->driver->GetLogConfig(&asyncVerbosity, &hexDumpsEnabled);
            if (kr == kIOReturnSuccess) {
                arguments->scalarOutput[0] = asyncVerbosity;
                arguments->scalarOutput[1] = hexDumpsEnabled;
                arguments->scalarOutputCount = 2;
            }
            return kr;
        }

        case kMethodTestIRMAllocation:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->TestIRMAllocation(arguments);
            
        case kMethodTestIRMRelease:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->TestIRMRelease(arguments);
            
        case kMethodTestCMPConnectOPCR:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->TestCMPConnectOPCR(arguments);
            
        case kMethodTestCMPDisconnectOPCR:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->TestCMPDisconnectOPCR(arguments);
            
        case kMethodTestCMPConnectIPCR:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->TestCMPConnectIPCR(arguments);
            
        case kMethodTestCMPDisconnectIPCR:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->TestCMPDisconnectIPCR(arguments);
            
        case kMethodStartIsochReceive:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->StartIsochReceive(arguments);
            
        case kMethodStopIsochReceive:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->StopIsochReceive(arguments);
            
        case kMethodGetIsochRxMetrics:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->GetIsochRxMetrics(arguments);
            
        case kMethodResetIsochRxMetrics:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->ResetIsochRxMetrics(arguments);
        
        // IT DMA Allocation (no CMP - just allocates memory)
        case kMethodStartIsochTransmit:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->StartIsochTransmit(arguments);
            
        case kMethodStopIsochTransmit:
            return static_cast<ASFW::UserClient::IsochHandler*>(ivars->isochHandler)->StopIsochTransmit(arguments);
        
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
    // LOCALONLY method - implementation is in TransactionHandler via ExternalMethod case 8
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
    // LOCALONLY method - implementation is in TransactionHandler via ExternalMethod case 9
    // This should never be called directly
    if (handle) {
        *handle = 0;
    }
    return kIOReturnUnsupported;
}

kern_return_t ASFWDriverUserClient::AsyncCompareSwap(
    uint16_t destinationID,
    uint16_t addressHi,
    uint32_t addressLo,
    uint8_t size,
    const void* compareValue,
    const void* newValue,
    uint16_t* handle,
    uint8_t* locked)
{
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

void ASFWDriverUserClient::NotifyStatus(uint64_t sequence,
                                        uint32_t reason)
{
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

void ASFWDriverUserClient::NotifyTransactionComplete(uint16_t handle,
                                                     uint32_t status)
{
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

kern_return_t ASFWDriverUserClient::GetTransactionResult(
    uint16_t handle,
    uint32_t* status,
    uint32_t* dataLength,
    void* data,
    uint32_t maxDataLength)
{
    // LOCALONLY method - implementation is in TransactionHandler via ExternalMethod case 12
    // This should never be called directly
    if (status) *status = 0;
    if (dataLength) *dataLength = 0;
    return kIOReturnUnsupported;
}

kern_return_t IMPL(ASFWDriverUserClient, CopyClientMemoryForType)
{
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
