// LogHelper.hpp
// Shared logging helper for DriverKit and user-space builds

#pragma once

#include <os/log.h>
#include <TargetConditionals.h>

static inline os_log_t ASLog() {
#if defined(TARGET_OS_DRIVERKIT) && TARGET_OS_DRIVERKIT
    return OS_LOG_DEFAULT;
#else
    static os_log_t log = os_log_create("net.mrmidi.ASFireWire", "ASOHCI");
    return log;
#endif
}

