// Logging stubs for host tests
// Provides no-op implementations to satisfy linker

#include "Logging/Logging.hpp"
#include <cstdarg>
#include <cstdio>

namespace ASFW::Driver::Logging {

// Stub log handles - just use default log for host tests
static os_log_t stub_log = OS_LOG_DEFAULT;

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

} // namespace ASFW::Driver::Logging
