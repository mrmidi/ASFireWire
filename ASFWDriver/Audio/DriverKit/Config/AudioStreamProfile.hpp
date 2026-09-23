// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioStreamProfile.hpp - Protocol-neutral ADK stream geometry contract.

#pragma once

#include "IAudioDeviceProfile.hpp"
#include "../../Wire/AMDTP/PcmSlotMap.hpp"
#include "../../Wire/MOTU/MotuPortLayout.hpp"

#include <cstdint>

namespace ASFW::Isoch::Audio {

enum class AudioStreamDirection : uint8_t {
    HostToDevice,
    DeviceToHost,
};

struct AudioStreamConfig final {
    AudioStreamDirection direction{AudioStreamDirection::HostToDevice};
    uint32_t sampleRate{48000};
    Encoding::StreamMode streamMode{Encoding::StreamMode::kBlocking};
    uint8_t sid{0};
    uint8_t pcmChannels{2};
    uint8_t dbs{2};
    uint8_t midiSlots{0};
    uint8_t framesPerDataPacket{8};
    uint8_t fdf{0x02};
    uint8_t fmt{0x10};
    uint8_t sourceChannelOffset{0};
};

struct AudioStreamTxPolicy final {
    Encoding::AudioWireFormat hostToDevicePcmEncoding{Encoding::AudioWireFormat::kAM824};
    bool variableDbs{false};
    uint32_t defaultNonAudioSlotWord{0x80000000};
    bool initializeNonAudioSlots{true};
    bool preserveFdfInNoDataPackets{false};
    bool emptyPacketsDuringIdle{false};
    bool dbcIsEndEvent{false};
    /// MOTU only: chunk behind each host output channel. Empty encodes in wire order.
    Encoding::Motu::MotuPortMap motuPlaybackPorts{};
    ::ASFW::Audio::Wire::PcmSlotMap playbackChannelMap{};

};

// What a profile's compiled-in stream geometry MEANS when the device's own
// registers say something else.
//
// The resolver needs this because two very different things were being spelled
// the same way. A geometry taken from a register dump or a reference stack is
// EVIDENCE: a device contradicting it means one of the two is wrong, framing
// and bandwidth would disagree, and refusing to publish is the right outcome.
// A geometry invented so the nub has plausible numbers before the device has
// been read is a SEED: being contradicted is not a defect, it is the seed doing
// its job and then being replaced.
//
// Treating every constant as evidence made seeded profiles unpublishable.
// AlesisMultiMixProfile.cpp says in its own header that its counts are
// unverified and that no reference stack publishes MultiMix geometry because
// they all read it from the device (Linux has no dice.c row for model 0x000000
// at all; libffado reads NB_TX from the device, dice_avdevice.cpp:1661-1680).
// A contributed register dump does report a different shape than the seed. Under
// a single rule that is a conflict and the endpoint disappears; under this one
// it is simply the device answering.
//
// Note this is about the CHANNEL and STREAM COUNTS only. Framing constants that
// the DICE registers do not carry -- fdf, fmt, frames per data packet, the DBS
// policy -- stay profile-owned in every case.
enum class StreamGeometryAuthority : uint8_t {
    /// A pre-caps placeholder. The device's answer replaces it silently.
    kSeed,
    /// Evidence from a dump or a reference stack. A contradiction is a defect.
    kAsserted,
};

// ADK packet allocation and AMDTP encoding are shared by multiple protocol
// families. Identity matching and control-plane quirks intentionally do not
// belong here.
class IAudioStreamProfile : public IAudioDeviceProfile {
public:
    virtual ~IAudioStreamProfile() override = default;

    [[nodiscard]] virtual bool BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept = 0;
    [[nodiscard]] virtual bool BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept = 0;
    [[nodiscard]] virtual uint32_t TxStreamCount() const noexcept { return 1; }
    [[nodiscard]] virtual uint32_t RxStreamCount() const noexcept { return 1; }
    [[nodiscard]] virtual AudioStreamTxPolicy TxStreamPolicy() const noexcept { return {}; }

    // Per direction, because one profile can legitimately be both. The Alesis
    // MultiMix asserts its ONE playback stream -- libffado forces nb_rx to 1 for
    // this exact vendor/model because the device announces two receive
    // transmitters and has one (dice_avdevice.cpp:1686-1700) -- while its
    // capture counts are an unverified seed. Collapsing the two into a single
    // per-profile flag would have to throw away one of those facts.
    //
    // Asserted is the default: a profile that carries geometry is claiming to
    // know it, and opting out is the exception that has to be written down.
    [[nodiscard]] virtual StreamGeometryAuthority PlaybackGeometryAuthority() const noexcept {
        return StreamGeometryAuthority::kAsserted;
    }
    [[nodiscard]] virtual StreamGeometryAuthority CaptureGeometryAuthority() const noexcept {
        return StreamGeometryAuthority::kAsserted;
    }

    // Profiles with unequal playback streams override this geometry. Uniform
    // streams retain their default shape at successive channel offsets.
    [[nodiscard]] virtual bool BuildTxStreamConfig(
        uint32_t streamIndex, AudioStreamConfig& outConfig) const noexcept {
        if (streamIndex >= TxStreamCount() || !BuildDefaultTxStreamConfig(outConfig)) {
            return false;
        }
        outConfig.sourceChannelOffset = streamIndex * outConfig.pcmChannels;
        return true;
    }

    // Capture's counterpart. It existed only as a default, so every capture
    // stream was assumed to share stream 0's shape with nothing able to say
    // otherwise -- the recorded StudioLive 24.4.2 carries 16 + 10 on playback,
    // so unequal streams are real, not hypothetical.
    [[nodiscard]] virtual bool BuildRxStreamConfig(
        uint32_t streamIndex, AudioStreamConfig& outConfig) const noexcept {
        if (streamIndex >= RxStreamCount() || !BuildDefaultRxStreamConfig(outConfig)) {
            return false;
        }
        outConfig.sourceChannelOffset = streamIndex * outConfig.pcmChannels;
        return true;
    }

    // Budget StartIO grants the device-to-host stream to deliver the first
    // data-bearing packet (which seeds the HAL zero-timestamp anchor) before
    // the start attempt is failed. DICE devices stream data within a few
    // milliseconds of the isoch start, so the default stays tight. BeBoB
    // devices transmit only CIP NO-DATA until roughly one second after they
    // begin receiving host packets, so their profiles must widen this budget
    // (Linux waits 4 s; cross-validated with Linux bebob_stream.c:10,636-666).
    [[nodiscard]] virtual uint32_t InitialClockAnchorTimeoutMs() const noexcept { return 500; }

    // Sum the streams the device actually carries. The previous form was
    // pcmChannels * StreamCount(), which silently assumes every stream has
    // stream 0's width: the recorded Venice F24 carries 16 + 8, so that form
    // reports 32 where the device means 24, and the StudioLive 24.4.2's
    // 16 + 10 playback reports 32 where the device means 26. Profiles with
    // unequal streams had to override these to be correct; summing means they
    // no longer have to, and a profile that forgets is no longer wrong.
    [[nodiscard]] uint32_t TxChannelCount() const noexcept override {
        uint32_t total = 0;
        for (uint32_t i = 0; i < TxStreamCount(); ++i) {
            AudioStreamConfig config{};
            if (BuildTxStreamConfig(i, config)) {
                total += config.pcmChannels;
            }
        }
        return total;
    }

    [[nodiscard]] uint32_t RxChannelCount() const noexcept override {
        uint32_t total = 0;
        for (uint32_t i = 0; i < RxStreamCount(); ++i) {
            AudioStreamConfig config{};
            if (BuildRxStreamConfig(i, config)) {
                total += config.pcmChannels;
            }
        }
        return total;
    }

    [[nodiscard]] uint32_t TxMidiSlots() const noexcept override {
        AudioStreamConfig config{};
        return BuildDefaultTxStreamConfig(config) ? config.midiSlots : 0;
    }

    [[nodiscard]] uint32_t RxMidiSlots() const noexcept override {
        AudioStreamConfig config{};
        return BuildDefaultRxStreamConfig(config) ? config.midiSlots : 0;
    }

    [[nodiscard]] uint32_t TxDbs() const noexcept override {
        AudioStreamConfig config{};
        return BuildDefaultTxStreamConfig(config) ? config.dbs : 0;
    }

    [[nodiscard]] uint32_t RxDbs() const noexcept override {
        AudioStreamConfig config{};
        return BuildDefaultRxStreamConfig(config) ? config.dbs : 0;
    }
};

} // namespace ASFW::Isoch::Audio
