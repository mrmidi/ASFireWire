//
// LogConfig.cpp
// ASFWDriver
//
// Runtime logging configuration implementation
//

#include "LogConfig.hpp"
#include "Logging.hpp"
#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSDictionary.h>

namespace ASFW {

// ============================================================================
// Singleton Access
// ============================================================================

LogConfig& LogConfig::Shared() {
    static LogConfig instance;
    return instance;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

LogConfig::LogConfig()
{
    // Initialize atomics (explicit for clarity)
    asyncVerbosity_.store(1);           // Default: Compact
    controllerVerbosity_.store(1);
    hardwareVerbosity_.store(1);
    enableHexDumps_.store(false);       // Default: No hex dumps
    logStatistics_.store(true);         // Default: Show statistics
    initialized_.store(false);
}

LogConfig::~LogConfig() {
    // std::atomic handles cleanup automatically
}

// ============================================================================
// Initialization
// ============================================================================

void LogConfig::Initialize(IOService* service) {
    if (!service) {
        ASFW_LOG_ERROR(Controller, "LogConfig::Initialize called with null service");
        return;
    }

    // Check if already initialized (atomic read)
    if (initialized_.load()) {
        ASFW_LOG(Controller, "LogConfig already initialized, skipping");
        return;
    }

    // Read properties from Info.plist (atomic stores)
    asyncVerbosity_.store(ReadUInt8Property(service, "ASFWAsyncVerbosity", 1));
    controllerVerbosity_.store(ReadUInt8Property(service, "ASFWControllerVerbosity", 1));
    hardwareVerbosity_.store(ReadUInt8Property(service, "ASFWHardwareVerbosity", 1));
    enableHexDumps_.store(ReadBoolProperty(service, "ASFWEnableHexDumps", false));
    logStatistics_.store(ReadBoolProperty(service, "ASFWLogStatistics", true));

    initialized_.store(true);

    // Log configuration (always visible at INFO level)
    ASFW_LOG_INFO(Controller,
                  "LogConfig initialized: Async=%u Controller=%u Hardware=%u HexDumps=%d Stats=%d",
                  asyncVerbosity_.load(), controllerVerbosity_.load(), hardwareVerbosity_.load(),
                  enableHexDumps_.load(), logStatistics_.load());
}

// ============================================================================
// Getters (Thread-Safe)
// ============================================================================

uint8_t LogConfig::GetAsyncVerbosity() const {
    return asyncVerbosity_.load(std::memory_order_relaxed);
}

uint8_t LogConfig::GetControllerVerbosity() const {
    return controllerVerbosity_.load(std::memory_order_relaxed);
}

uint8_t LogConfig::GetHardwareVerbosity() const {
    return hardwareVerbosity_.load(std::memory_order_relaxed);
}

bool LogConfig::IsHexDumpsEnabled() const {
    return enableHexDumps_.load(std::memory_order_relaxed);
}

bool LogConfig::IsStatisticsEnabled() const {
    return logStatistics_.load(std::memory_order_relaxed);
}

// ============================================================================
// Runtime Setters (Thread-Safe)
// ============================================================================

void LogConfig::SetAsyncVerbosity(uint8_t level) {
    level = ClampLevel(level);
    asyncVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "Async verbosity changed to %u", level);
}

void LogConfig::SetControllerVerbosity(uint8_t level) {
    level = ClampLevel(level);
    controllerVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "Controller verbosity changed to %u", level);
}

void LogConfig::SetHardwareVerbosity(uint8_t level) {
    level = ClampLevel(level);
    hardwareVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "Hardware verbosity changed to %u", level);
}

void LogConfig::SetHexDumps(bool enable) {
    enableHexDumps_.store(enable, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "Hex dumps %{public}s", enable ? "enabled" : "disabled");
}

void LogConfig::SetStatistics(bool enable) {
    logStatistics_.store(enable, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "Statistics logging %{public}s", enable ? "enabled" : "disabled");
}

// ============================================================================
// Private Helpers
// ============================================================================

uint8_t LogConfig::ReadUInt8Property(IOService* service, const char* key, uint8_t defaultValue) {
    // Get service properties dictionary (DriverKit pattern from ASFWDriver)
    OSDictionary* serviceProperties = nullptr;
    kern_return_t kr = service->CopyProperties(&serviceProperties);

    if (kr != kIOReturnSuccess || !serviceProperties) {
        ASFW_LOG_INFO(Controller, "Property '%{public}s' = %u (default, CopyProperties failed)", key, defaultValue);
        return defaultValue;
    }

    uint8_t value = defaultValue;
    bool found = false;

    if (auto property = serviceProperties->getObject(key)) {
        if (auto numberProp = OSDynamicCast(OSNumber, property)) {
            uint32_t val = numberProp->unsigned32BitValue();
            value = ClampLevel(static_cast<uint8_t>(val));
            found = true;
        }
    }

    if (found) {
        ASFW_LOG_INFO(Controller, "Property '%{public}s' = %u (from Info.plist)", key, value);
    } else {
        ASFW_LOG_INFO(Controller, "Property '%{public}s' = %u (default, not in Info.plist)", key, value);
    }

    OSSafeReleaseNULL(serviceProperties);
    return value;
}

bool LogConfig::ReadBoolProperty(IOService* service, const char* key, bool defaultValue) {
    // Get service properties dictionary (DriverKit pattern from ASFWDriver)
    OSDictionary* serviceProperties = nullptr;
    kern_return_t kr = service->CopyProperties(&serviceProperties);

    if (kr != kIOReturnSuccess || !serviceProperties) {
        ASFW_LOG_INFO(Controller, "Property '%{public}s' = %{public}s (default, CopyProperties failed)",
                      key, defaultValue ? "true" : "false");
        return defaultValue;
    }

    bool value = defaultValue;
    bool found = false;

    if (auto property = serviceProperties->getObject(key)) {
        if (auto booleanProp = OSDynamicCast(OSBoolean, property)) {
            value = (booleanProp == kOSBooleanTrue);
            found = true;
        } else if (auto numberProp = OSDynamicCast(OSNumber, property)) {
            value = numberProp->unsigned32BitValue() != 0;
            found = true;
        }
    }

    if (found) {
        ASFW_LOG_INFO(Controller, "Property '%{public}s' = %{public}s (from Info.plist)",
                      key, value ? "true" : "false");
    } else {
        ASFW_LOG_INFO(Controller, "Property '%{public}s' = %{public}s (default, not in Info.plist)",
                      key, value ? "true" : "false");
    }

    OSSafeReleaseNULL(serviceProperties);
    return value;
}

uint8_t LogConfig::ClampLevel(uint8_t level) {
    if (level > 4) {
        return 4;
    }
    return level;
}

} // namespace ASFW
