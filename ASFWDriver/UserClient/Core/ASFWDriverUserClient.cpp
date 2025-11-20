//
//  ASFWDriverUserClient.cpp
//  ASFWDriver
//
//  User client for GUI application communication
//  Refactored into handler-based architecture for maintainability
//

#include "ASFWDriverUserClient.h"
#include "ASFWDriver.h"
#include "../Handlers/BusResetHandler.hpp"
#include "../Handlers/TopologyHandler.hpp"
#include "../Handlers/StatusHandler.hpp"
#include "../Handlers/TransactionHandler.hpp"
#include "../Handlers/ConfigROMHandler.hpp"
#include "../Handlers/DeviceDiscoveryHandler.hpp"
#include "../Storage/TransactionStorage.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Shared/DriverVersionInfo.hpp"
#include "../../Version/DriverVersion.hpp"


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
    auto* storage = new ASFW::UserClient::TransactionStorage();
    if (!storage || !storage->IsValid()) {
        delete storage;
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

        // Delete handlers
        delete static_cast<ASFW::UserClient::BusResetHandler*>(ivars->busResetHandler);
        delete static_cast<ASFW::UserClient::TopologyHandler*>(ivars->topologyHandler);
        delete static_cast<ASFW::UserClient::StatusHandler*>(ivars->statusHandler);
        delete static_cast<ASFW::UserClient::TransactionHandler*>(ivars->transactionHandler);
        delete static_cast<ASFW::UserClient::ConfigROMHandler*>(ivars->configROMHandler);
        delete static_cast<ASFW::UserClient::DeviceDiscoveryHandler*>(ivars->deviceDiscoveryHandler);

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

    if (!ivars->busResetHandler || !ivars->topologyHandler ||
        !ivars->statusHandler || !ivars->transactionHandler ||
        !ivars->configROMHandler || !ivars->deviceDiscoveryHandler) {
        ASFW_LOG(UserClient, "Start() failed to create handlers");
        return kIOReturnNoMemory;
    }

    ASFW_LOG(UserClient, "Start() completed - handlers initialized");
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWDriverUserClient, Stop)
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

    // Verify handlers are initialized
    if (!ivars->busResetHandler || !ivars->topologyHandler ||
        !ivars->statusHandler || !ivars->transactionHandler ||
        !ivars->configROMHandler || !ivars->deviceDiscoveryHandler) {
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

        // TransactionHandler methods - CompareSwap (17)
        case kMethodAsyncCompareSwap:
            return static_cast<ASFW::UserClient::TransactionHandler*>(ivars->transactionHandler)->AsyncCompareSwap(arguments, this);

        // Version query (18)
        case kMethodGetDriverVersion: {
            if (!arguments->structureOutput) {
                return kIOReturnBadArgument;
            }

            // Create version info
            ASFW::Shared::DriverVersionInfo versionInfo{};
            std::strncpy(versionInfo.semanticVersion, ASFW::Version::kSemanticVersion, sizeof(versionInfo.semanticVersion) - 1);
            std::strncpy(versionInfo.gitCommitShort, ASFW::Version::kGitCommitShort, sizeof(versionInfo.gitCommitShort) - 1);
            std::strncpy(versionInfo.gitCommitFull, ASFW::Version::kGitCommitFull, sizeof(versionInfo.gitCommitFull) - 1);
            std::strncpy(versionInfo.gitBranch, ASFW::Version::kGitBranch, sizeof(versionInfo.gitBranch) - 1);
            std::strncpy(versionInfo.buildTimestamp, ASFW::Version::kBuildTimestamp, sizeof(versionInfo.buildTimestamp) - 1);
            std::strncpy(versionInfo.buildHost, ASFW::Version::kBuildHost, sizeof(versionInfo.buildHost) - 1);
            versionInfo.gitDirty = ASFW::Version::kGitDirty;

            // Use OSData pattern like other handlers
            OSData* data = OSData::withBytes(&versionInfo, sizeof(versionInfo));
            if (!data) {
                return kIOReturnNoMemory;
            }
            
            arguments->structureOutput = data;
            arguments->structureOutputDescriptor = nullptr;

            ASFW_LOG(UserClient, "GetDriverVersion: %s", ASFW::Version::kFullVersionString);
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
