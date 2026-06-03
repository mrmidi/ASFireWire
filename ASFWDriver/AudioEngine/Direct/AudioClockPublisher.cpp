#include "AudioClockPublisher.hpp"

#include <AudioDriverKit/AudioDriverKit.h>

namespace ASFW::AudioEngine::Direct {

void AudioClockPublisher::Publish(uint64_t sampleFrame,
                                  uint64_t hostTicks,
                                  uint32_t hostNanosPerSampleQ8) noexcept {
    if (!IsBound()) {
        return;
    }

    binding_->control->device.Publish(sampleFrame, hostTicks, hostNanosPerSampleQ8);
    binding_->control->counters.CountZtsPublished();

    binding_->audioDevice->UpdateCurrentZeroTimestamp(sampleFrame, hostTicks);
}

} // namespace ASFW::AudioEngine::Direct
