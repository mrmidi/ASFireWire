#include "AudioClockPublisher.hpp"

#include <AudioDriverKit/AudioDriverKit.h>
#include "../../Logging/Logging.hpp"

namespace ASFW::AudioEngine::Direct {

void AudioClockPublisher::Publish(uint64_t sampleFrame,
                                  uint64_t hostTicks,
                                  uint32_t hostNanosPerSampleQ8) noexcept {
    if (!IsBound()) {
        return;
    }

    binding_->control->device.Publish(sampleFrame, hostTicks, hostNanosPerSampleQ8);
    binding_->control->counters.CountZtsPublished();
    const uint64_t count =
        binding_->control->counters.ztsPublished.load(std::memory_order_relaxed);
    if (count <= 32 || (count % 100) == 0) {
        ASFW_LOG(DirectAudio,
                 "ZTS publish source=rx count=%llu sample=%llu host=%llu nsPerSampleQ8=%u",
                 count,
                 sampleFrame,
                 hostTicks,
                 hostNanosPerSampleQ8);
    }

    binding_->audioDevice->UpdateCurrentZeroTimestamp(sampleFrame, hostTicks);
}

} // namespace ASFW::AudioEngine::Direct
