#include "DiagnosticsHandler.hpp"
#include "../../Controller/ControllerCore.hpp"
#include "../../Diagnostics/DiagnosticsService.hpp"
#include "../../Async/AsyncSubsystem.hpp"
#include "../../Async/Interfaces/IAsyncSubsystemPort.hpp"
#include "../../Debug/AsyncTraceCapture.hpp"
#include "../../Logging/Logging.hpp"
#include "ASFWDriver.h"
#include "ControllerCoreAccess.hpp"

#include <DriverKit/OSData.h>

namespace ASFW::UserClient {

namespace {

template <typename StructType, typename CollectFn>
kern_return_t CollectAndPack(Diagnostics::DiagnosticsService* service, IOUserClientMethodArguments* args, CollectFn&& collectFn) {
    if (!service) {
        return kIOReturnNotReady;
    }
    if (!args) {
        return kIOReturnBadArgument;
    }

    StructType val{};
    ASFWDiagStatus status = (service->*collectFn)(&val);
    (void)status; // Handled via header status field
    
    // Allocate OSData with the bytes directly
    OSData* data = OSData::withBytes(&val, sizeof(StructType));
    if (!data) {
        return kIOReturnNoMemory;
    }

    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

} // namespace

DiagnosticsHandler::DiagnosticsHandler(ASFWDriver* driver) noexcept
    : driver_(driver) {
    auto* controller = GetControllerCorePtr(driver_);
    if (controller) {
        service_ = new Diagnostics::DiagnosticsService(controller);
    }
}

DiagnosticsHandler::~DiagnosticsHandler() {
    delete service_;
    service_ = nullptr;
}

kern_return_t DiagnosticsHandler::GetBusContract(IOUserClientMethodArguments* args) {
    return CollectAndPack<ASFWDiagBusContract>(service_, args, &Diagnostics::DiagnosticsService::CollectBusContract);
}

kern_return_t DiagnosticsHandler::GetTopology(IOUserClientMethodArguments* args) {
    return CollectAndPack<ASFWDiagTopology>(service_, args, &Diagnostics::DiagnosticsService::CollectTopology);
}

kern_return_t DiagnosticsHandler::GetRoleCoordinator(IOUserClientMethodArguments* args) {
    return CollectAndPack<ASFWDiagRoleCoordinator>(service_, args, &Diagnostics::DiagnosticsService::CollectRoleCoordinator);
}

kern_return_t DiagnosticsHandler::GetOHCI(IOUserClientMethodArguments* args) {
    return CollectAndPack<ASFWDiagOHCI>(service_, args, &Diagnostics::DiagnosticsService::CollectOHCI);
}

kern_return_t DiagnosticsHandler::GetPHY(IOUserClientMethodArguments* args) {
    return CollectAndPack<ASFWDiagPHY>(service_, args, &Diagnostics::DiagnosticsService::CollectPHY);
}

kern_return_t DiagnosticsHandler::GetCSRContract(IOUserClientMethodArguments* args) {
    return CollectAndPack<ASFWDiagCSRContract>(service_, args, &Diagnostics::DiagnosticsService::CollectCSRContract);
}

kern_return_t DiagnosticsHandler::GetAsyncTrace(IOUserClientMethodArguments* args) {
    return CollectAndPack<ASFWDiagAsyncTrace>(service_, args, &Diagnostics::DiagnosticsService::CollectAsyncTrace);
}

kern_return_t DiagnosticsHandler::GetInboundCSRStats(IOUserClientMethodArguments* args) {
    return CollectAndPack<ASFWDiagInboundCSRStats>(service_, args, &Diagnostics::DiagnosticsService::CollectInboundCSRStats);
}

kern_return_t DiagnosticsHandler::ClearAsyncTrace(IOUserClientMethodArguments* args) {
    if (!driver_) {
        return kIOReturnNotReady;
    }
    
    auto* controller = GetControllerCorePtr(driver_);
    if (!controller) {
        return kIOReturnNotReady;
    }
    
    auto* trace = controller->AsyncSubsystem().GetAsyncTraceCapture();
    if (trace) {
        trace->Clear();
    }
    
    // Return empty status payload
    OSData* data = OSData::withCapacity(0);
    if (!data) {
        return kIOReturnNoMemory;
    }
    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
