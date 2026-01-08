// Logging stubs for host tests
// Provides no-op implementations to satisfy linker

#include "Logging/Logging.hpp"
#include <cstdarg>
#include <cstdio>

#include "Logging/LogConfig.hpp"

namespace ASFW::Driver::Logging {

// Stub log handles - just use default log for host tests
static os_log_t stub_log = OS_LOG_DEFAULT;

// DriverKit Stubs
extern "C" {
    typedef struct IOLock IOLock;
    
    IOLock* IOLockAlloc() { return (IOLock*)1; }
    void IOLockFree(IOLock* lock) {}
    void IOLockLock(IOLock* lock) {}
    void IOLockUnlock(IOLock* lock) {}
    
    void IOSleep(uint64_t milliseconds) {}
    
    void* IOMallocZero(size_t size) { return calloc(1, size); }
    void IOFree(void* ptr, size_t size) { free(ptr); }
}

os_log_t Core() {
    return stub_log;
}

os_log_t BusReset() {
    return stub_log;
}

os_log_t Topology() {
    return stub_log;
}

os_log_t ConfigROM() {
    return stub_log;
}

os_log_t Transaction() {
    return stub_log;
}

os_log_t Interrupt() {
    return stub_log;
}

os_log_t Controller() {
    return stub_log;
}

os_log_t Hardware() {
    return stub_log;
}

os_log_t Metrics() {
    return stub_log;
}

os_log_t Async() {
    return stub_log;
}

os_log_t UserClient() {
    return stub_log;
}

os_log_t Discovery() {
    return stub_log;
}

os_log_t IRM() {
    return stub_log;
}

os_log_t BusManager() {
    return stub_log;
}

} // namespace ASFW::Driver::Logging

namespace ASFW {

// Minimal LogConfig stub for host tests (no plist, fixed defaults)
LogConfig& LogConfig::Shared() {
    static LogConfig instance;
    return instance;
}

// Constructors are defined inline for the stub; atomics default to zero/false.
LogConfig::LogConfig() = default;
LogConfig::~LogConfig() = default;

void LogConfig::Initialize(IOService*) {}

uint8_t LogConfig::GetAsyncVerbosity() const { return 0; }
uint8_t LogConfig::GetControllerVerbosity() const { return 0; }
uint8_t LogConfig::GetHardwareVerbosity() const { return 0; }
uint8_t LogConfig::GetDiscoveryVerbosity() const { return 0; }
uint8_t LogConfig::GetAVCVerbosity() const { return 0; }
uint8_t LogConfig::GetFCPVerbosity() const { return 0; }
uint8_t LogConfig::GetMusicSubunitVerbosity() const { return 0; }
bool LogConfig::IsHexDumpsEnabled() const { return false; }
bool LogConfig::IsStatisticsEnabled() const { return false; }

void LogConfig::SetAsyncVerbosity(uint8_t) {}
void LogConfig::SetControllerVerbosity(uint8_t) {}
void LogConfig::SetHardwareVerbosity(uint8_t) {}
void LogConfig::SetDiscoveryVerbosity(uint8_t) {}
void LogConfig::SetAVCVerbosity(uint8_t) {}
void LogConfig::SetFCPVerbosity(uint8_t) {}
void LogConfig::SetMusicSubunitVerbosity(uint8_t) {}
void LogConfig::SetHexDumps(bool) {}
void LogConfig::SetStatistics(bool) {}

uint8_t LogConfig::ReadUInt8Property(IOService*, const char*, uint8_t defaultValue) { return defaultValue; }
bool LogConfig::ReadBoolProperty(IOService*, const char*, bool defaultValue) { return defaultValue; }
uint8_t LogConfig::ClampLevel(uint8_t level) { return level > 4 ? 4 : level; }

} // namespace ASFW
