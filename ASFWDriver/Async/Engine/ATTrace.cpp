#include "ATTrace.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Async::Engine {

namespace {
const char* EventName(ATEvent ev) noexcept {
    switch (ev) {
        case ATEvent::P1_ARM: return "P1_ARM";
        case ATEvent::P2_LNK: return "P2_LNK";
        case ATEvent::P2_WAKE: return "P2_WAKE";
        case ATEvent::P2_FALLBACK: return "P2_FALLBACK";
        case ATEvent::STOP_IMM: return "STOP_IMM";
        case ATEvent::STOP_DRAIN: return "STOP_DRAIN";
        case ATEvent::RESET: return "RESET";
        case ATEvent::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
} // anonymous namespace

void ATTraceRing::dump() const noexcept {
    const uint32_t currentIdx = idx_.load(std::memory_order_relaxed);
    const uint32_t startIdx = (currentIdx >= 256) ? (currentIdx & 0xFF) : 0;
    const uint32_t count = (currentIdx >= 256) ? 256 : currentIdx;

    ASFW_LOG_ERROR(Async, "=== AT Trace Ring Dump (last %u events, index=%u) ===", count, currentIdx);

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t idx = (startIdx + i) & 0xFF;
        const ATTrace& e = buf_[idx];
        ASFW_LOG(Async, "[%3u] t=%llu txid=%u gen=%u ev=%{public}s a=0x%08x b=0x%08x",
                 i, e.t_ns, e.txid, e.gen, EventName(e.ev), e.a, e.b);
    }

    ASFW_LOG_ERROR(Async, "=== End AT Trace Ring Dump ===");
}

} // namespace ASFW::Async::Engine
