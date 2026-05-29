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
#include <cstddef>

namespace ASFW::UserClient {

namespace {

// IOConnectCallStructMethod returns structure output inline, capped at 4 KB. The app
// requests at most this many bytes (see ASFWDiagnosticsClient.structOutputLimit); if the
// driver hands back a larger OSData the copy fails with kIOReturnNoSpace and the retry --
// now requesting >4 KB -- is rejected with kIOReturnBadArgument. So every payload we pack
// must be clamped to this limit, not just to ASFW_DIAG_MAX_*.
constexpr size_t kStructOutputLimit = 4096;

// Default: fixed-size structs are well under the limit; serialize the whole thing.
template <typename T>
size_t PrepareForWire(T& /*val*/) {
    return sizeof(T);
}

// Topology grows with node count. Clamp to whatever fits in the inline limit and reflect
// the truncation in nodeCount so the client never reads zero-filled phantom nodes.
template <>
inline size_t PrepareForWire<ASFWDiagTopology>(ASFWDiagTopology& val) {
    constexpr size_t kHeaderBytes = offsetof(ASFWDiagTopology, nodes);
    constexpr uint32_t kMaxFit =
        static_cast<uint32_t>((kStructOutputLimit - kHeaderBytes) / sizeof(ASFWDiagNode));
    uint32_t count = val.nodeCount;
    if (count > ASFW_DIAG_MAX_NODES) {
        count = ASFW_DIAG_MAX_NODES;
    }
    if (count > kMaxFit) {
        count = kMaxFit;
    }
    val.nodeCount = count;
    return kHeaderBytes + (count * sizeof(ASFWDiagNode));
}

// The async trace ring (up to ASFW_DIAG_MAX_ASYNC_EVENTS) far exceeds the inline limit when
// full. events[] is ordered oldest->newest, so when we must truncate we drop from the front
// to keep the most recent activity, then rewrite eventCount to the serialized count.
template <>
inline size_t PrepareForWire<ASFWDiagAsyncTrace>(ASFWDiagAsyncTrace& val) {
    constexpr size_t kHeaderBytes = offsetof(ASFWDiagAsyncTrace, events);
    constexpr uint32_t kMaxFit =
        static_cast<uint32_t>((kStructOutputLimit - kHeaderBytes) / sizeof(ASFWDiagAsyncEvent));
    uint32_t count = val.eventCount;
    if (count > ASFW_DIAG_MAX_ASYNC_EVENTS) {
        count = ASFW_DIAG_MAX_ASYNC_EVENTS;
    }
    if (count > kMaxFit) {
        const uint32_t drop = count - kMaxFit;
        for (uint32_t i = 0; i < kMaxFit; ++i) {
            val.events[i] = val.events[i + drop];
        }
        count = kMaxFit;
    }
    val.eventCount = count;
    return kHeaderBytes + (count * sizeof(ASFWDiagAsyncEvent));
}

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

    size_t sizeToCopy = PrepareForWire(val);

    // Allocate OSData with the populated bytes directly
    OSData* data = OSData::withBytes(&val, static_cast<uint32_t>(sizeToCopy));
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

kern_return_t DiagnosticsHandler::GetBusManager(IOUserClientMethodArguments* args) {
    return CollectAndPack<ASFWDiagBusManager>(service_, args, &Diagnostics::DiagnosticsService::CollectBusManager);
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
