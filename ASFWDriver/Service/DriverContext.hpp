#pragma once

#include <atomic>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/IOServiceNotificationDispatchSource.h>
#endif

#include "../Controller/ControllerConfig.hpp"
#include "../Controller/ControllerCore.hpp"
#include "../Diagnostics/StatusPublisher.hpp"
#include "../Hardware/InterruptDispatcher.hpp"
#include "../Isoch/IsochService.hpp"
#include "../Scheduling/WatchdogCoordinator.hpp"

class ASFWDriver;

namespace ASFW::Audio {
class AudioCoordinator;
}

struct ServiceContext {
    ASFW::Driver::ControllerCore::Dependencies deps;
    ASFW::Driver::ControllerConfig config{}; // immutable identity/static config
    // Initial (wiring-time) role policy. The runtime-mutable copy is owned by
    // ControllerCore; this is the seed passed at construction and read by
    // wiring that runs before ControllerCore exists (e.g. the election driver).
    ASFW::Driver::RolePolicy rolePolicy{};
    std::shared_ptr<ASFW::Driver::ControllerCore> controller;
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<OSAction> interruptAction;
#ifndef ASFW_HOST_TEST
    OSSharedPtr<IOServiceNotificationDispatchSource> providerNotifications;
    OSSharedPtr<OSAction> providerNotificationAction;
#endif
    std::atomic<bool> stopping{false};
    ASFW::Driver::StatusPublisher statusPublisher;
    ASFW::Driver::WatchdogCoordinator watchdog;
    ASFW::Driver::IsochService isoch;
    ASFW::Driver::InterruptDispatcher interruptDispatcher;
    std::shared_ptr<ASFW::Audio::AudioCoordinator> audioCoordinator;

    void DisarmProviderNotifications();
    void Reset();
};

namespace ASFW::Driver {

class DriverWiring {
public:
    static void EnsureDeps(ASFWDriver* driver, ::ServiceContext& ctx);
    static void EnsureSbp2Deps(::ServiceContext& ctx);
    static kern_return_t PrepareQueue(ASFWDriver& service, ::ServiceContext& ctx);
    static kern_return_t PrepareInterrupts(ASFWDriver& service, IOService* provider, ::ServiceContext& ctx);
    static kern_return_t PrepareWatchdog(ASFWDriver& service, ::ServiceContext& ctx);
    static void CleanupStartFailure(::ServiceContext& ctx);
};

} // namespace ASFW::Driver
