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
    discoveryVerbosity_.store(2);       // Default: Transitions (AVC discovery needs more detail)
    configROMVerbosity_.store(1);       // Default: Compact
    userClientVerbosity_.store(1);      // Default: Compact
    musicSubunitVerbosity_.store(1);    // Default: Compact
    fcpVerbosity_.store(1);             // Default: Compact
    cmpVerbosity_.store(1);             // Default: Compact
    irmVerbosity_.store(1);             // Default: Compact
    avcVerbosity_.store(1);             // Default: Compact
    isochVerbosity_.store(1);           // Default: Compact
    enableHexDumps_.store(false);       // Default: No hex dumps
    isochTxVerifierEnabled_.store(false); // Default: disabled (dev-only, expensive)
    audioAutoStartEnabled_.store(true); // Default: enabled
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
    discoveryVerbosity_.store(ReadUInt8Property(service, "ASFWDiscoveryVerbosity", 2));
    configROMVerbosity_.store(ReadUInt8Property(service, "ASFWConfigROMVerbosity", 1));
    userClientVerbosity_.store(ReadUInt8Property(service, "ASFWUserClientVerbosity", 1));
    musicSubunitVerbosity_.store(ReadUInt8Property(service, "ASFWMusicSubunitVerbosity", 1));
    fcpVerbosity_.store(ReadUInt8Property(service, "ASFWFCPVerbosity", 1));
    cmpVerbosity_.store(ReadUInt8Property(service, "ASFWCMPVerbosity", 1));
    irmVerbosity_.store(ReadUInt8Property(service, "ASFWIRMVerbosity", 1));
    avcVerbosity_.store(ReadUInt8Property(service, "ASFWAVCVerbosity", 1));
    isochVerbosity_.store(ReadUInt8Property(service, "ASFWIsochVerbosity", 1));
    enableHexDumps_.store(ReadBoolProperty(service, "ASFWEnableHexDumps", false));
    isochTxVerifierEnabled_.store(ReadBoolProperty(service, "ASFWEnableIsochTxVerifier", false));
    audioAutoStartEnabled_.store(ReadBoolProperty(service, "ASFWAutoStartAudioStreams", true));
    logStatistics_.store(ReadBoolProperty(service, "ASFWLogStatistics", true));

    initialized_.store(true);

    // Log configuration (always visible at INFO level)
    ASFW_LOG_INFO(Controller,
                  "LogConfig initialized: Async=%u Controller=%u Hardware=%u Discovery=%u ConfigROM=%u UserClient=%u Music=%u FCP=%u CMP=%u IRM=%u AVC=%u Isoch=%u HexDumps=%d TxVerify=%d AutoStart=%d Stats=%d",
                  asyncVerbosity_.load(), controllerVerbosity_.load(), hardwareVerbosity_.load(),
                  discoveryVerbosity_.load(), configROMVerbosity_.load(), userClientVerbosity_.load(),
                  musicSubunitVerbosity_.load(), fcpVerbosity_.load(), cmpVerbosity_.load(), irmVerbosity_.load(), avcVerbosity_.load(),
                  isochVerbosity_.load(),
                  enableHexDumps_.load(), isochTxVerifierEnabled_.load(),
                  audioAutoStartEnabled_.load(),
                  logStatistics_.load());
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

uint8_t LogConfig::GetDiscoveryVerbosity() const {
    return discoveryVerbosity_.load(std::memory_order_relaxed);
}

uint8_t LogConfig::GetConfigROMVerbosity() const {
    return configROMVerbosity_.load(std::memory_order_relaxed);
}

uint8_t LogConfig::GetUserClientVerbosity() const {
    return userClientVerbosity_.load(std::memory_order_relaxed);
}

uint8_t LogConfig::GetMusicSubunitVerbosity() const {
    return musicSubunitVerbosity_.load(std::memory_order_relaxed);
}

uint8_t LogConfig::GetFCPVerbosity() const {
    return fcpVerbosity_.load(std::memory_order_relaxed);
}

uint8_t LogConfig::GetCMPVerbosity() const {
    return cmpVerbosity_.load(std::memory_order_relaxed);
}

uint8_t LogConfig::GetIRMVerbosity() const {
    return irmVerbosity_.load(std::memory_order_relaxed);
}

uint8_t LogConfig::GetAVCVerbosity() const {
    return avcVerbosity_.load(std::memory_order_relaxed);
}

uint8_t LogConfig::GetIsochVerbosity() const {
    return isochVerbosity_.load(std::memory_order_relaxed);
}

bool LogConfig::IsHexDumpsEnabled() const {
    return enableHexDumps_.load(std::memory_order_relaxed);
}

bool LogConfig::IsStatisticsEnabled() const {
    return logStatistics_.load(std::memory_order_relaxed);
}

bool LogConfig::IsIsochTxVerifierEnabled() const {
    return isochTxVerifierEnabled_.load(std::memory_order_relaxed);
}

bool LogConfig::IsAudioAutoStartEnabled() const {
    return audioAutoStartEnabled_.load(std::memory_order_relaxed);
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

void LogConfig::SetDiscoveryVerbosity(uint8_t level) {
    level = ClampLevel(level);
    discoveryVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "Discovery verbosity changed to %u", level);
}

void LogConfig::SetConfigROMVerbosity(uint8_t level) {
    level = ClampLevel(level);
    configROMVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "ConfigROM verbosity changed to %u", level);
}

void LogConfig::SetUserClientVerbosity(uint8_t level) {
    level = ClampLevel(level);
    userClientVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "UserClient verbosity changed to %u", level);
}

void LogConfig::SetMusicSubunitVerbosity(uint8_t level) {
    level = ClampLevel(level);
    musicSubunitVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "MusicSubunit verbosity changed to %u", level);
}

void LogConfig::SetFCPVerbosity(uint8_t level) {
    level = ClampLevel(level);
    fcpVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "FCP verbosity changed to %u", level);
}

void LogConfig::SetCMPVerbosity(uint8_t level) {
    level = ClampLevel(level);
    cmpVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "CMP verbosity changed to %u", level);
}

void LogConfig::SetIRMVerbosity(uint8_t level) {
    level = ClampLevel(level);
    irmVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "IRM verbosity changed to %u", level);
}

void LogConfig::SetAVCVerbosity(uint8_t level) {
    level = ClampLevel(level);
    avcVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "AVC verbosity changed to %u", level);
}

void LogConfig::SetIsochVerbosity(uint8_t level) {
    level = ClampLevel(level);
    isochVerbosity_.store(level, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "Isoch verbosity changed to %u", level);
}

void LogConfig::SetHexDumps(bool enable) {
    enableHexDumps_.store(enable, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "Hex dumps %{public}s", enable ? "enabled" : "disabled");
}

void LogConfig::SetIsochTxVerifierEnabled(bool enable) {
    isochTxVerifierEnabled_.store(enable, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "Isoch TX verifier %{public}s", enable ? "enabled" : "disabled");
}

void LogConfig::SetAudioAutoStartEnabled(bool enable) {
    audioAutoStartEnabled_.store(enable, std::memory_order_relaxed);
    ASFW_LOG_INFO(Controller, "Audio auto-start %{public}s", enable ? "enabled" : "disabled");
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

    if (found) { // NOSONAR(cpp:S3923): branches log different diagnostic messages
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

    if (found) { // NOSONAR(cpp:S3923): branches log different diagnostic messages
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
