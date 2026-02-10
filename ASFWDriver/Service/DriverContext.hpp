#pragma once

#include <atomic>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include "../Controller/ControllerConfig.hpp"
#include "../Controller/ControllerCore.hpp"
#include "../Diagnostics/StatusPublisher.hpp"
#include "../Hardware/InterruptDispatcher.hpp"
#include "../Isoch/IsochService.hpp"
#include "../Scheduling/WatchdogCoordinator.hpp"

class ASFWDriver;

struct ServiceContext {
    ASFW::Driver::ControllerCore::Dependencies deps;
    ASFW::Driver::ControllerConfig config{}; // placeholder config
    std::shared_ptr<ASFW::Driver::ControllerCore> controller;
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<OSAction> interruptAction;
    std::atomic<bool> stopping{false};
    ASFW::Driver::StatusPublisher statusPublisher;
    ASFW::Driver::WatchdogCoordinator watchdog;
    ASFW::Driver::IsochService isoch;
    ASFW::Driver::InterruptDispatcher interruptDispatcher;

    void Reset();
};

namespace ASFW::Driver {

class DriverWiring {
public:
    static void EnsureDeps(ASFWDriver* driver, ::ServiceContext& ctx);
    static kern_return_t PrepareQueue(ASFWDriver& service, ::ServiceContext& ctx);
    static kern_return_t PrepareInterrupts(ASFWDriver& service, IOService* provider, ::ServiceContext& ctx);
    static kern_return_t PrepareWatchdog(ASFWDriver& service, ::ServiceContext& ctx);
    static void CleanupStartFailure(::ServiceContext& ctx);
};

} // namespace ASFW::Driver
