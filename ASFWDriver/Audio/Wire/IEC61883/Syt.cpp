#include "Syt.hpp"

namespace ASFW::Protocols::Audio::IEC61883 {

bool SytFormatter::IsNoInfo(uint16_t syt) noexcept {
    return syt == kNoInfo;
}

SytTimestamp SytFormatter::NoInfo() noexcept {
    return SytTimestamp{kNoInfo, false};
}

uint16_t SytFormatter::EncodeCycleOffset(uint32_t cycle, uint32_t offsetTicks) noexcept {
    return static_cast<uint16_t>(((cycle & 0xFu) << 12) | (offsetTicks & 0xFFFu));
}

} // namespace ASFW::Protocols::Audio::IEC61883
