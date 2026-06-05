#pragma once

#include "AudioTransportControlBlock.hpp"

#include <cstdint>

class IOUserAudioDevice;

namespace ASFW::Audio::Runtime {

struct DirectAudioBindingSnapshot {
    uint64_t generation{0};

    int32_t* inputBase{nullptr};
    uint64_t inputBytes{0};
    uint32_t inputFrames{0};
    uint32_t inputChannels{0};

    const int32_t* outputBase{nullptr};
    uint64_t outputBytes{0};
    uint32_t outputFrames{0};
    uint32_t outputChannels{0};

    AudioTransportControlBlock* control{nullptr};
    uint32_t sampleRateHz{0};
    IOUserAudioDevice* audioDevice{nullptr};
    bool valid{false};

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
    virtual bool CopyDirectAudioBinding(DirectAudioBindingSnapshot& out) noexcept = 0;
};

} // namespace ASFW::Audio::Runtime

#if !defined(ASFW_HOST_TEST) && __has_include(<net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>)
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#define ASFW_HAS_GENERATED_AUDIO_NUB_HEADER 1
#endif

#if defined(ASFW_HAS_GENERATED_AUDIO_NUB_HEADER)
namespace ASFW::Audio::Runtime {

class NubDirectAudioBindingSource final : public IDirectAudioBindingSource {
public:
    explicit NubDirectAudioBindingSource(ASFWAudioNub* nub) noexcept : nub_(nub) {}

    bool CopyDirectAudioBinding(DirectAudioBindingSnapshot& out) noexcept override {
        if (!nub_) {
            return false;
        }
        const int32_t* outBase = nullptr;
        uint64_t outBytes = 0;
        uint32_t outFrames = 0;
        uint32_t outChannels = 0;
        int32_t* inBase = nullptr;
        uint64_t inBytes = 0;
        uint32_t inFrames = 0;
        uint32_t inChannels = 0;
        ASFW::Audio::Runtime::AudioTransportControlBlock* control = nullptr;
        uint32_t sampleRateHz = 0;
        IOUserAudioDevice* audioDevice = nullptr;
        uint64_t gen = 0;

        bool ready = nub_->GetDirectAudioBinding(&outBase, &outBytes, &outFrames, &outChannels,
                                                &inBase, &inBytes, &inFrames, &inChannels,
                                                &control, &sampleRateHz, &audioDevice, &gen);
        if (ready) {
            out.generation = gen;
            out.outputBase = outBase;
            out.outputBytes = outBytes;
            out.outputFrames = outFrames;
            out.outputChannels = outChannels;
            out.inputBase = inBase;
            out.inputBytes = inBytes;
            out.inputFrames = inFrames;
            out.inputChannels = inChannels;
            out.control = control;
            out.sampleRateHz = sampleRateHz;
            out.audioDevice = audioDevice;
            out.valid = true;
            return true;
        }
        out = {};
        return false;
    }

private:
    ASFWAudioNub* nub_{nullptr};
};

} // namespace ASFW::Audio::Runtime
#endif
