// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuCommandDsp.hpp - the command DSP of the MOTU mk3 family (828mk3, 896mk3,
// UltraLite mk3, Traveler mk3, Track16): the mixer CueMix drives.
//
// The unit takes commands in block writes to kDspCommandAddress and, once the
// host has registered a destination (0x0b38/0x0b3c) and started messaging,
// sends its whole DSP state and then every front-panel change the same way.
// Each transaction frame is at most 248 bytes: a tag byte, a sequence number,
// commands, zero padding to a quadlet. A longer message continues in the next
// frame.
//
// A command addresses a parameter by four identifier bytes {channel, third
// level, second level, first level} and carries one byte or one float32. The
// first level selects the block: 0 monitor, 1 input, 2 mixer, 3 output,
// 4 reverb. The float32 is little-endian on the wire: the reference's code
// reads and writes it with from_le_bytes/to_le_bytes, although its comments
// say big-endian. Not yet observed on hardware.
//
// Behavior cross-checked with snd-firewire-ctl-services (LGPL-3.0), protocols/
// motu/src/command_dsp.rs: framing and sequence numbers (send_message,
// cache_dsp_messages), command layouts (DspCmd::parse, append_u8/append_f32),
// the mixer identifiers (MixerCmd), the monitor identifiers (MonitorCmd).
// Written fresh; no code is copied.
//
// Pure logic: no transport, no DriverKit.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ASFW::Audio::Motu::CommandDsp {

inline constexpr uint64_t kDspCommandAddress = 0xffff00010000ull;
/// Where the unit is told to send its messages (high and low halves).
inline constexpr uint32_t kMessageDestinationHi = 0x0b38;
inline constexpr uint32_t kMessageDestinationLo = 0x0b3c;
inline constexpr size_t kMaxFrameBytes = 248;
inline constexpr size_t kFramePrefixBytes = 2;

/// The first byte of a frame. Commands travel as kTagCommands; kTagBegin then
/// kTagCommands with no commands start messaging, kTagCancel stops it.
inline constexpr uint8_t kTagCancel = 0x00;
inline constexpr uint8_t kTagBegin = 0x01;
inline constexpr uint8_t kTagCommands = 0x02;

namespace Op {
inline constexpr uint8_t kPadding = 0x00;
inline constexpr uint8_t kResource = 0x23;
inline constexpr uint8_t kQuadletMultiple = 0x46;
inline constexpr uint8_t kByteMultiple = 0x49;
inline constexpr uint8_t kDrain = 0x62;
inline constexpr uint8_t kEnd = 0x65;
inline constexpr uint8_t kQuadletSingle = 0x66;
inline constexpr uint8_t kByteSingle = 0x69;
} // namespace Op

inline constexpr size_t kResourceBytes = 6;
inline constexpr size_t kByteSingleBytes = 6;
inline constexpr size_t kQuadletSingleBytes = 9;

enum class Block : uint8_t { Monitor = 0, Input = 1, Mixer = 2, Output = 3, Reverb = 4 };

/// One parameter value. `id` is {channel, third, second, first level}.
struct Command final {
    std::array<uint8_t, 4> id{};
    bool quadlet{false};
    uint8_t byte{0};
    float value{0.0f};

    [[nodiscard]] static constexpr Command Byte(Block block, uint8_t second, uint8_t third,
                                                uint8_t channel, uint8_t v) noexcept {
        return {{channel, third, second, static_cast<uint8_t>(block)}, false, v, 0.0f};
    }
    [[nodiscard]] static constexpr Command Float(Block block, uint8_t second, uint8_t third,
                                                 uint8_t channel, float v) noexcept {
        return {{channel, third, second, static_cast<uint8_t>(block)}, true, 0, v};
    }
    [[nodiscard]] constexpr uint8_t Channel() const noexcept { return id[0]; }
    [[nodiscard]] constexpr uint8_t Third() const noexcept { return id[1]; }
    [[nodiscard]] constexpr uint8_t Second() const noexcept { return id[2]; }
    [[nodiscard]] constexpr uint8_t First() const noexcept { return id[3]; }

    friend bool operator==(const Command&, const Command&) noexcept = default;
};

namespace Detail {
inline void AppendFloatLe(std::vector<uint8_t>& out, float v) {
    const auto bits = std::bit_cast<uint32_t>(v);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
}
[[nodiscard]] inline float FloatLe(const uint8_t* p) noexcept {
    const uint32_t bits = uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) |
                          (uint32_t{p[3]} << 24);
    return std::bit_cast<float>(bits);
}
/// Length of the command at `p`, or 0 for padding, the end marker, an unknown
/// op, or a command that does not fit in `remaining`.
[[nodiscard]] inline size_t CommandLength(const uint8_t* p, size_t remaining) noexcept {
    if (remaining == 0) return 0;
    size_t length = 0;
    switch (p[0]) {
    case Op::kResource: length = kResourceBytes; break;
    case Op::kQuadletMultiple: length = remaining >= 2 ? 6 + 4 * size_t{p[1]} : 0; break;
    case Op::kByteMultiple: length = remaining >= 2 ? 6 + size_t{p[1]} : 0; break;
    case Op::kDrain: length = 1; break;
    case Op::kQuadletSingle: length = kQuadletSingleBytes; break;
    case Op::kByteSingle: length = kByteSingleBytes; break;
    default: return 0;  // padding, end, or unknown
    }
    return length <= remaining ? length : 0;
}
} // namespace Detail

/// Append one command in its single-value form.
inline void Encode(const Command& c, std::vector<uint8_t>& out) {
    if (c.quadlet) {
        out.push_back(Op::kQuadletSingle);
        out.insert(out.end(), c.id.begin(), c.id.end());
        Detail::AppendFloatLe(out, c.value);
    } else {
        out.push_back(Op::kByteSingle);
        out.push_back(c.byte);
        out.insert(out.end(), c.id.begin(), c.id.end());
    }
}

/// Decode a reassembled message into commands. A multiple command expands into
/// one command per channel, counting from 0. Resource reports and drain markers
/// carry no parameter and are skipped. Stops at padding, the end marker, an
/// unknown op or a truncated command; returns false only for the last two.
inline bool Decode(std::span<const uint8_t> message, std::vector<Command>& out) {
    size_t at = 0;
    while (at < message.size()) {
        const uint8_t* p = message.data() + at;
        const size_t remaining = message.size() - at;
        if (p[0] == Op::kPadding || p[0] == Op::kEnd) return true;
        const size_t length = Detail::CommandLength(p, remaining);
        if (length == 0) return false;
        switch (p[0]) {
        case Op::kByteSingle:
            out.push_back({{p[2], p[3], p[4], p[5]}, false, p[1], 0.0f});
            break;
        case Op::kQuadletSingle:
            out.push_back({{p[1], p[2], p[3], p[4]}, true, 0, Detail::FloatLe(p + 5)});
            break;
        case Op::kByteMultiple:
            for (uint8_t i = 0; i < p[1]; ++i) out.push_back({{i, p[3], p[4], p[5]}, false, p[6 + i], 0.0f});
            break;
        case Op::kQuadletMultiple:
            for (uint8_t i = 0; i < p[1]; ++i)
                out.push_back({{i, p[3], p[4], p[5]}, true, 0, Detail::FloatLe(p + 6 + 4 * size_t{i})});
            break;
        default:  // resource, drain
            break;
        }
        at += length;
    }
    return true;
}

/// Split a message into transaction frames: tag, sequence number, at most 246
/// bytes of commands, zero padding to a quadlet. A frame not quadlet-aligned
/// stops the unit sending messages on its own. The sequence number advances per
/// frame and wraps from 254 to 0, as the reference sends it.
inline std::vector<std::vector<uint8_t>> BuildFrames(std::span<const uint8_t> message, uint8_t tag,
                                                     uint8_t& sequence) {
    std::vector<std::vector<uint8_t>> frames;
    size_t at = 0;
    do {
        const size_t chunk = std::min(message.size() - at, kMaxFrameBytes - kFramePrefixBytes);
        std::vector<uint8_t> frame{tag, sequence};
        frame.insert(frame.end(), message.begin() + static_cast<std::ptrdiff_t>(at),
                     message.begin() + static_cast<std::ptrdiff_t>(at + chunk));
        while (frame.size() % 4 != 0) frame.push_back(0);
        frames.push_back(std::move(frame));
        sequence = static_cast<uint8_t>((sequence + 1) % 0xff);
        at += chunk;
    } while (at < message.size());
    return frames;
}

/// Reassembles the unit's message frames. A frame out of sequence drops what
/// was being collected. A message ends with a frame shorter than the maximum;
/// its trailing padding is removed. Frames whose first command byte is zero are
/// keep-alives and carry nothing.
class MessageReassembler final {
public:
    /// Feed one received frame. Returns a complete message when this frame ends one.
    std::optional<std::vector<uint8_t>> Feed(std::span<const uint8_t> frame) {
        if (frame.size() < kFramePrefixBytes) return std::nullopt;
        const uint8_t sequence = frame[1];
        if (state_ == State::Initialized) {
            expected_ = sequence;
            state_ = State::Idle;
        }
        if (sequence != expected_) {
            // Lost frames: whatever was collected cannot be trusted.
            cache_.clear();
            state_ = State::Idle;
            expected_ = static_cast<uint8_t>(sequence + 1);
            ++dropped_;
            return std::nullopt;
        }
        expected_ = static_cast<uint8_t>(sequence + 1);
        const auto body = frame.subspan(kFramePrefixBytes);
        if (state_ == State::Idle) {
            if (frame.size() <= 4 || body[0] == 0) return std::nullopt;  // keep-alive
            cache_.assign(body.begin(), body.end());
            if (frame.size() == kMaxFrameBytes) {
                state_ = State::Continuing;
                return std::nullopt;
            }
        } else {
            cache_.insert(cache_.end(), body.begin(), body.end());
            if (frame.size() == kMaxFrameBytes) return std::nullopt;
            state_ = State::Idle;
        }
        TrimPadding();
        return std::exchange(cache_, {});
    }

    [[nodiscard]] uint64_t DroppedFrames() const noexcept { return dropped_; }

private:
    void TrimPadding() {
        size_t at = 0;
        while (at < cache_.size()) {
            const size_t length = Detail::CommandLength(cache_.data() + at, cache_.size() - at);
            if (length == 0) break;
            at += length;
        }
        cache_.resize(at);
    }

    enum class State : uint8_t { Initialized, Idle, Continuing };
    State state_{State::Initialized};
    uint8_t expected_{0};
    std::vector<uint8_t> cache_;
    uint64_t dropped_{0};
};

//==============================================================================
// The mixer (first level 2) and the monitor (first level 0).
//==============================================================================

inline constexpr size_t kMixerCount = 8;
/// Mixer sources are addressed from second level 2 on (0 and 1 are the mixer
/// output and its reverb send/return).
inline constexpr uint8_t kMixerSourceSecondLevelBase = 2;

namespace MixerThird {
// Second level 0: the mixer output.
inline constexpr uint8_t kOutputAssign = 0x00;  // byte: index into the model's output list
inline constexpr uint8_t kOutputMute = 0x01;    // byte
inline constexpr uint8_t kOutputVolume = 0x02;  // float 0..1
// Second level 1: reverb.
inline constexpr uint8_t kReverbSend = 0x00;    // float
inline constexpr uint8_t kReverbReturn = 0x01;  // float
// Second level 2 + source: one mixer source.
inline constexpr uint8_t kSourceMute = 0x00;    // byte
inline constexpr uint8_t kSourceSolo = 0x01;    // byte
inline constexpr uint8_t kSourcePan = 0x02;     // float -1..1 (mono balance)
inline constexpr uint8_t kSourceGain = 0x03;    // float 0..1
} // namespace MixerThird

namespace MonitorThird {
inline constexpr uint8_t kMainVolume = 0x00;    // float 0..1
inline constexpr uint8_t kReturnAssign = 0x08;  // byte
} // namespace MonitorThird

/// Commands that set one mixer value.
namespace Set {
[[nodiscard]] constexpr Command OutputAssign(uint8_t mixer, uint8_t index) noexcept {
    return Command::Byte(Block::Mixer, 0, MixerThird::kOutputAssign, mixer, index);
}
[[nodiscard]] constexpr Command OutputMute(uint8_t mixer, bool mute) noexcept {
    return Command::Byte(Block::Mixer, 0, MixerThird::kOutputMute, mixer, mute ? 1 : 0);
}
[[nodiscard]] constexpr Command OutputVolume(uint8_t mixer, float v) noexcept {
    return Command::Float(Block::Mixer, 0, MixerThird::kOutputVolume, mixer, v);
}
[[nodiscard]] constexpr Command SourceMute(uint8_t mixer, uint8_t source, bool mute) noexcept {
    return Command::Byte(Block::Mixer, static_cast<uint8_t>(kMixerSourceSecondLevelBase + source),
                         MixerThird::kSourceMute, mixer, mute ? 1 : 0);
}
[[nodiscard]] constexpr Command SourceSolo(uint8_t mixer, uint8_t source, bool solo) noexcept {
    return Command::Byte(Block::Mixer, static_cast<uint8_t>(kMixerSourceSecondLevelBase + source),
                         MixerThird::kSourceSolo, mixer, solo ? 1 : 0);
}
[[nodiscard]] constexpr Command SourceGain(uint8_t mixer, uint8_t source, float v) noexcept {
    return Command::Float(Block::Mixer, static_cast<uint8_t>(kMixerSourceSecondLevelBase + source),
                          MixerThird::kSourceGain, mixer, v);
}
[[nodiscard]] constexpr Command SourcePan(uint8_t mixer, uint8_t source, float v) noexcept {
    return Command::Float(Block::Mixer, static_cast<uint8_t>(kMixerSourceSecondLevelBase + source),
                          MixerThird::kSourcePan, mixer, v);
}
[[nodiscard]] constexpr Command MainVolume(float v) noexcept {
    return Command::Float(Block::Monitor, 0, MonitorThird::kMainVolume, 0, v);
}
} // namespace Set

} // namespace ASFW::Audio::Motu::CommandDsp
