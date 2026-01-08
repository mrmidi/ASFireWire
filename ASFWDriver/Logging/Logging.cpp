// DriverKit logging shim: os_log_create is not exposed to dexts, so every call
// resolves to OS_LOG_DEFAULT. We rely on the ASFW_LOG macro to add readable
// prefixes for filtering.
#include "Logging.hpp"
#include <os/log.h>

namespace {
inline os_log_t MakeCategory(const char *category) {
    (void)category; // os_log_create unavailable in DriverKit, categories visible via message prefix
    return OS_LOG_DEFAULT;
}
} // namespace

namespace ASFW::Driver::Logging {

os_log_t Controller() { static os_log_t log = MakeCategory("controller"); return log; }
os_log_t Hardware()   { static os_log_t log = MakeCategory("hardware");   return log; }
os_log_t BusReset()   { static os_log_t log = MakeCategory("busreset");   return log; }
os_log_t Topology()   { static os_log_t log = MakeCategory("topology");   return log; }
os_log_t Metrics()    { static os_log_t log = MakeCategory("metrics");    return log; }
os_log_t Async()      { static os_log_t log = MakeCategory("async");      return log; }
os_log_t UserClient() { static os_log_t log = MakeCategory("userclient"); return log; }
os_log_t Discovery()  { static os_log_t log = MakeCategory("discovery");  return log; }
os_log_t IRM()        { static os_log_t log = MakeCategory("irm");        return log; }
os_log_t BusManager() { static os_log_t log = MakeCategory("busmanager"); return log; }
os_log_t ConfigROM()  { static os_log_t log = MakeCategory("configrom");  return log; }
os_log_t MusicSubunit() { static os_log_t log = MakeCategory("musicsubunit"); return log; }
os_log_t FCP()        { static os_log_t log = MakeCategory("fcp");        return log; }
os_log_t CMP()        { static os_log_t log = MakeCategory("cmp");        return log; }
os_log_t AVC()        { static os_log_t log = MakeCategory("avc");        return log; }
os_log_t Isoch()      { static os_log_t log = MakeCategory("isoch");      return log; }
os_log_t Audio()      { static os_log_t log = MakeCategory("audio");      return log; }

} // namespace ASFW::Driver::Logging
