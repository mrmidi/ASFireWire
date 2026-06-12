#include "AudioClockPublisher.hpp"

#include <AudioDriverKit/AudioDriverKit.h>
#include "../../../Logging/Logging.hpp"

namespace ASFW::AudioEngine::Direct {

ASFW::Audio::Runtime::HostClockAnchorPublishResult
AudioClockPublisher::Publish(uint64_t sampleFrame,
                             uint64_t hostTicks,
                             uint32_t hostNanosPerSampleQ8) noexcept {
    if (!IsBound()) {
        return {};
    }

    const auto result = binding_->control->PublishHostClockAnchor(
        sampleFrame, hostTicks, hostNanosPerSampleQ8);
    if (result.accepted) {
        binding_->control->counters.CountRxZtsPublished();
    }
    return result;
}

} // namespace ASFW::AudioEngine::Direct
