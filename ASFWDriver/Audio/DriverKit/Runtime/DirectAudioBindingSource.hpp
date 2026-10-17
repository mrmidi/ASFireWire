#pragma once

#include "AudioTransportControlBlock.hpp"
#include "../../Ports/IMidiReceiveWake.hpp"
#include "../../Wire/AM824/MpxMidiDemux.hpp"
#include "../../../Midi/Transport/MidiTransportBlock.hpp"

#include <cstdint>

namespace ASFW::Audio::Runtime {

/// Where the receive path puts MPX-MIDI bytes lifted off the wire.
///
/// A value snapshot like everything else here: the pointer is valid only for
/// the batch it was copied for, because the endpoint owns the mapping and the
/// consumer drops its copy when the transport quiesces.
struct MidiReceiveBinding final {
    /// Changes whenever the target, geometry or wake changes. Independent of
    /// the audio binding's generation on purpose: attaching MIDI must never be
    /// able to rebind (and so reset) the audio replay state.
    uint64_t generation{0};
    uint64_t endpointId{0};
    ASFW::Midi::MidiTransportBlock* block{nullptr};
    /// Device -> host: the slot index and port count of the capture stream.
    ASFW::Encoding::MpxMidiGeometry geometry{};
    ASFW::Audio::Ports::IMidiReceiveWake* wake{nullptr};

    [[nodiscard]] bool Valid() const noexcept {
        return block != nullptr && geometry.Valid();
    }
};

struct DirectAudioBindingSnapshot {
    uint64_t generation{0};

    float* inputBase{nullptr};
    uint64_t inputBytes{0};
    uint32_t inputFrames{0};
    uint32_t inputChannels{0};

    const float* outputBase{nullptr};
    uint64_t outputBytes{0};
    uint32_t outputFrames{0};
    uint32_t outputChannels{0};

    AudioTransportControlBlock* control{nullptr};
    uint32_t sampleRateHz{0};
    bool valid{false};

    /// The endpoint's MIDI attachment, if it has one. Absent for every endpoint
    /// that publishes no MIDI service.
    MidiReceiveBinding midiRx{};

    [[nodiscard]] bool HasInput() const noexcept {
        return inputBase != nullptr && inputFrames > 0 && inputChannels > 0;
    }

    [[nodiscard]] bool HasOutput() const noexcept {
        return outputBase != nullptr && outputFrames > 0 && outputChannels > 0;
    }

    [[nodiscard]] bool IsValidDuplex() const noexcept {
        return HasInput() && HasOutput() && control != nullptr && sampleRateHz > 0 && valid;
    }
};

class IDirectAudioBindingSource {
public:
    virtual ~IDirectAudioBindingSource() = default;
    [[nodiscard]] virtual bool IsTransmitTimingReady() noexcept { return false; }
    [[nodiscard]] virtual bool CopyDirectAudioBinding(DirectAudioBindingSnapshot& out) noexcept = 0;
};

} // namespace ASFW::Audio::Runtime
