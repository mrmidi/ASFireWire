#include "AudioIOPath.hpp"

namespace ASFW::Driver {

// Thin routing seam between the HAL-facing WriteEnd handler and the protocol
// engine. The output view passes through unmodified — the producer
// (VirtualAudioDevice's IO block) already builds the ring-window view, and
// the payload writer owns the wrap arithmetic. The format is cached for the
// Step 6 scenario pump and future validation; it is deliberately not used to
// second-guess the view.

void AudioIOPath::BindDiceTxEngine(
    Protocols::Audio::DICE::DiceTxStreamEngine* engine) noexcept {
    diceTxEngine_ = engine;
}

void AudioIOPath::SetOutputFormatFloat32(uint32_t channels,
                                         uint32_t frameCapacity) noexcept {
    outputChannels_ = channels;
    outputFrameCapacity_ = frameCapacity;
}

void AudioIOPath::HandleWriteEnd(
    const Protocols::Audio::AMDTP::HostAudioBufferView& output) noexcept {
    if (diceTxEngine_ == nullptr) {
        return;
    }
    diceTxEngine_->WriteHostOutputFloat32(output);
}

} // namespace ASFW::Driver
