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
    binding_->control->counters.CountRxZtsPublished();
    // Hot-path RX clock diagnostic, disabled for audio stability.
    // const uint64_t count =
    //     binding_->control->counters.ztsRxPublished.load(std::memory_order_relaxed);
    // if (count <= 8 || (count % 1024) == 0) {
    //     ASFW_LOG(DirectAudio,
    //              "ZTS publish source=rx count=%llu sample=%llu host=%llu nsPerSampleQ8=%u",
    //              count,
    //              sampleFrame,
    //              hostTicks,
    //              hostNanosPerSampleQ8);
    // }

    if (binding_->audioDevice) {
        binding_->audioDevice->UpdateCurrentZeroTimestamp(sampleFrame, hostTicks);
        binding_->control->counters.CountRxAdkZtsPublished();
    }
}

} // namespace ASFW::AudioEngine::Direct
