#pragma once

#include <algorithm>
#include <cstdint>

namespace ASFW::Audio::Runtime {

struct PlaybackRingRangeUpdate final {
    uint64_t oldestValidFrame{0};
    uint64_t writtenEndFrame{0};
    bool discontinuity{false};
    bool overrun{false};
};

[[nodiscard]] constexpr PlaybackRingRangeUpdate
UpdatePlaybackRingRange(uint64_t previousWrittenEnd,
                        uint64_t previousOldestValid,
                        uint64_t writeStart,
                        uint64_t writeEnd,
                        uint64_t consumedEnd,
                        uint32_t capacityFrames) noexcept {
    PlaybackRingRangeUpdate result{
        .oldestValidFrame = previousOldestValid,
        .writtenEndFrame = previousWrittenEnd,
    };
    if (writeEnd <= writeStart || writeEnd <= previousWrittenEnd) {
        return result;
    }

    const bool firstWrite = previousWrittenEnd == 0;
    result.discontinuity = !firstWrite && writeStart != previousWrittenEnd;
    result.writtenEndFrame = writeEnd;

    if (firstWrite || result.discontinuity) {
        result.oldestValidFrame = writeStart;
    }
    if (capacityFrames != 0 && writeEnd > capacityFrames) {
        result.oldestValidFrame =
            std::max(result.oldestValidFrame, writeEnd - capacityFrames);
    }
    result.overrun = consumedEnd < result.oldestValidFrame;
    return result;
}

} // namespace ASFW::Audio::Runtime
