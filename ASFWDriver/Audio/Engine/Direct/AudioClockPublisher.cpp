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

    // One authority per device: RX anchors reach CoreAudio only while the
    // device's timeline is in a Receive epoch. A Transmit epoch (M-Audio
    // special firmware) owns the clock, and an RX anchor must not interleave
    // with it in the one mailbox. No epoch at all (a rate outside the HAL
    // ladder, or a device without a timeline) keeps the previous behaviour.
    const auto& timeline = binding_->control->hardwareTimeline;
    const uint64_t epoch = timeline.Epoch();
    if (epoch != 0 &&
        timeline.Source() != ::ASFW::Audio::Runtime::HardwareTimelineSource::Receive) {
        return {};
    }

    const auto result = binding_->control->PublishHostClockAnchor(
        sampleFrame, hostTicks, hostNanosPerSampleQ8, epoch);
    if (result.accepted) {
        binding_->control->counters.CountRxZtsPublished();
    }
    return result;
}

} // namespace ASFW::AudioEngine::Direct
