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

    const bool updated = binding_->control->UpdateAuthoritativeZtsFromRx(sampleFrame, hostTicks, hostNanosPerSampleQ8);
    if (updated) {
        binding_->control->counters.CountRxZtsPublished();
    }

    const auto mode = binding_->control->ztsState.selectedMode.load(std::memory_order_relaxed);
    if (mode == ASFW::Audio::Runtime::ZtsPublicationMode::DirectToHAL && binding_->audioDevice) {
        binding_->audioDevice->UpdateCurrentZeroTimestamp(sampleFrame, hostTicks);
        binding_->control->ztsState.directPublications.fetch_add(1, std::memory_order_relaxed);
        binding_->control->counters.CountRxAdkZtsPublished();
    }
}

} // namespace ASFW::AudioEngine::Direct
