// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../ASFWDriver/Audio/Wire/IEC61883/CipHeader.hpp"
#include "../../ASFWDriver/Audio/Wire/IEC61883/DbcCounter.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include "PlaybackClip.hpp"

// Original Pro 40, 48 kHz only. Packet content, independent of native DMA.
// Reference behavior: Linux amdtp-stream.c:275-295,469-487,677-709;
// amdtp-am824.c:99-103,209-221,296-321; dice-stream.c:488-514.
namespace Pro40Packets {
namespace IEC = ASFW::Protocols::Audio::IEC61883;
struct Layout {
    uint8_t pcm, midi;
    constexpr uint8_t slots() const { return pcm + midi; }
    constexpr size_t dataBytes() const { return 8 + 8 * slots() * 4; }
};
inline constexpr std::array<Layout, 2> capture = {{{10,1}, {10,0}}};
inline constexpr std::array<Layout, 2> playback = {{{12,1}, {8,0}}};
constexpr bool supported(Layout layout) {
    return (layout.pcm == 10 && layout.midi <= 1) ||
        (layout.pcm == 12 && layout.midi == 1) || (layout.pcm == 8 && layout.midi == 0);
}
inline constexpr uint32_t ticksPerCycle = 3072;
inline constexpr uint32_t transferDelay = 0x2e00 - ticksPerCycle + 4096;
inline constexpr uint32_t sytDomain = 16 * ticksPerCycle;
static_assert(capture[0].dataBytes() == 360 && capture[1].dataBytes() == 328);
static_assert(playback[0].dataBytes() == 424 && playback[1].dataBytes() == 264);
constexpr uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline void write32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
struct Packet {
    uint8_t blocks = 0, dbc = 0;
    uint16_t syt = 0xffff;
};
enum class Error { none, size, header, source, layout, rate, timestamp, label, continuity };
struct Parsed { Packet packet{}; Error error = Error::none; explicit operator bool() const { return error == Error::none; } };
enum class PcmLabelPolicy { required, diceReceiveTiming };

// Input excludes the OHCI/isochronous header and trailer. Native code must use
// the actual received byte length, not the allocated buffer capacity.
inline Parsed parseEnvelope(std::span<const uint8_t> bytes, Layout layout, uint8_t sender) {
    if (!supported(layout)) return {{}, Error::layout};
    if (bytes.size() < 8 || (bytes.size() % 4)) return {{}, Error::size};
    const auto q0 = read32(bytes.data()), q1 = read32(bytes.data() + 4);
    if ((q0 & 0xc000ff00u) || (q1 >> 24) != 0x90) return {{}, Error::header};
    if (((q0 >> 24) & 63) != (sender & 63)) return {{}, Error::source};
    if (((q0 >> 16) & 255) != layout.slots() || !layout.slots()) return {{}, Error::layout};
    const size_t payload = bytes.size() - 8, blockBytes = layout.slots() * 4;
    if (payload != 0 && payload != 8 * blockBytes) return {{}, Error::size};
    const uint8_t fdf = uint8_t(q1 >> 16);
    if (fdf != 2 && !(payload == 0 && fdf == 0xff)) return {{}, Error::rate};
    const uint16_t syt = uint16_t(q1);
    if ((syt != 0xffff && (syt & 0xfff) >= ticksPerCycle) || (!payload && syt != 0xffff))
        return {{}, Error::timestamp};
    return {{uint8_t(payload ? 8 : 0), uint8_t(q0), syt}, Error::none};
}

inline Parsed parse(std::span<const uint8_t> bytes, Layout layout, uint8_t sender,
                    PcmLabelPolicy pcmLabels = PcmLabelPolicy::required) {
    const auto envelope=parseEnvelope(bytes,layout,sender);
    if(!envelope)return envelope;
    const size_t blockBytes=layout.slots()*4;
    for (size_t offset = 8; offset < bytes.size(); offset += blockBytes) {
        for (unsigned slot = 0; slot < layout.slots(); ++slot) {
            const auto label = bytes[offset + slot * 4];
            // Linux amdtp-am824.c:179-205 discards the receive PCM upper byte.
            // Timing replay does not decode/forward PCM; retain strict labels
            // for generated output and keep MIDI validation in both directions.
            if (slot < layout.pcm ? (pcmLabels == PcmLabelPolicy::required && label != 0x40)
                                 : label < 0x80 || label > 0x83)
                return {{}, Error::label};
        }
    }
    return envelope;
}

// Explicit original-DICE capture path, not a generic permissive packet parser.
// Geometry, CIP, rate, timestamps and MIDI stay checked. The captured startup
// fixture contains zero PCM words without MBLA labels; no cause is inferred.
inline Parsed parseCaptureTiming(std::span<const uint8_t> bytes, unsigned stream, uint8_t sender) {
    if (stream >= capture.size()) return {{}, Error::layout};
    return parse(bytes, capture[stream], sender, PcmLabelPolicy::diceReceiveTiming);
}

inline std::optional<int32_t> sample24(std::span<const uint8_t> slot) {
    if (slot.size() != 4 || slot[0] != 0x40) return {};
    const uint32_t value = read32(slot.data()) & 0xffffff;
    return value & 0x800000 ? int32_t(int64_t(value) - 0x1000000) : int32_t(value);
}

class Continuity {
    bool initialized_ = false;
    uint8_t next_ = 0;
public:
    bool accept(Packet p) {
        if (initialized_ && p.dbc != next_) return false;
        initialized_ = true; next_ = uint8_t(p.dbc + p.blocks); return true;
    }
    void reset() { initialized_ = false; next_ = 0; }
};

// Replay the measured phase relative to the packet's arrival cycle; do not
// substitute a host-timer cadence. Actual cycle stamps must come from hardware.
inline std::optional<uint32_t> phase(uint16_t syt, uint32_t sourceCycle) {
    if (syt == 0xffff || (syt & 0xfff) >= ticksPerCycle || sourceCycle >= 8000) return {};
    const auto deltaCycles = ((syt >> 12) + 16 - (sourceCycle & 15)) & 15;
    const auto ticks = deltaCycles * ticksPerCycle + (syt & 0xfff);
    return (ticks + sytDomain - transferDelay) % sytDomain;
}
inline std::optional<uint16_t> replaySyt(uint32_t phaseTicks, uint32_t outputCycle) {
    if (phaseTicks >= sytDomain || outputCycle >= 8000) return {};
    const auto ticks = outputCycle * ticksPerCycle + phaseTicks + transferDelay;
    return uint16_t(((ticks / ticksPerCycle) & 15) << 12 | (ticks % ticksPerCycle));
}

class Am824Stream {
    Layout layout_;
    IEC::CipHeaderBuilder builder_;
    IEC::DbcCounter counter_;
public:
    Am824Stream(Layout layout, uint8_t sender) : layout_(layout), builder_(IEC::CipHeaderConfig{
        .sid = sender, .dbs = layout.slots(), .fdf = 2, .noDataFdf = 2}) {}
    void reset(uint8_t dbc = 0) { counter_.Reset(dbc); }
private:
    // Single encoder for finite clips and live owned PCM. Content-slot behavior
    // checked against linux-dice/amdtp-am824.c:99-103,147-176,209-221.
    template<class Sample> std::optional<size_t> encode(std::span<uint8_t> output,
        uint8_t blocks,uint16_t syt,Sample sample) {
        if ((blocks != 0 && blocks != 8) || !supported(layout_) ||
            (blocks && (syt == 0xffff || (syt & 0xfff) >= ticksPerCycle))) return {};
        const size_t size = 8 + blocks * layout_.slots() * 4;
        if (output.size() < size) return {};
        const auto header = blocks ? builder_.BuildData(counter_.Current(), syt)
            : builder_.BuildNoData(counter_.Current());
        write32(output.data(), header.q0); write32(output.data() + 4, header.q1);
        for (unsigned frame = 0; frame < blocks; ++frame)
            for (unsigned slot = 0; slot < layout_.slots(); ++slot)
                write32(output.data() + 8 + (frame * layout_.slots() + slot) * 4,
                    slot < layout_.pcm ? 0x40000000u | sample(frame,slot) : 0x80000000u);
        counter_.AdvanceDataBlocks(blocks);
        return size;
    }
public:
    // Blocks/SYT come from a validated incoming sequence, never a host timer.
    // Failure leaves counters and output bytes unchanged. Missing/exhausted
    // finite clip and unused PCM slots are labelled silence; MIDI stays empty.
    std::optional<size_t> make(std::span<uint8_t> output,uint8_t blocks,uint16_t syt,
                               const PlaybackClip *clip=nullptr,size_t firstFrame=0) {
        if(clip && (!clip->valid() || clip->channels>layout_.pcm))return {};
        return encode(output,blocks,syt,[&](unsigned frame,unsigned slot) {
            return clip && firstFrame<clip->frames() && frame<clip->frames()-firstFrame ?
                clip->sampleBits(firstFrame+frame,slot):0;
        });
    }
    std::optional<size_t> makeInterleaved(std::span<uint8_t> output,uint8_t blocks,uint16_t syt,
        std::span<const int32_t> samples,unsigned channels) {
        if(!channels || channels>layout_.pcm || samples.size()!=size_t(blocks)*channels)return {};
        for(auto value:samples)if(value < -8388608 || value > 8388607)return {};
        return encode(output,blocks,syt,[&](unsigned frame,unsigned slot) {
            return slot<channels?uint32_t(samples[frame*channels+slot])&0xffffff:0;
        });
    }
};
} // namespace Pro40Packets
