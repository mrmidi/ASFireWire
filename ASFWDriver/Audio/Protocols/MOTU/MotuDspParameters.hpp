// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuDspParameters.hpp - every parameter of the command DSP CueMix drives:
// monitor, mixer, input and output strips (trim, EQ, dynamics, reverb sends)
// and the reverb, as one table. The decoder, the state, the control model and
// parameter writes all read it, so a parameter exists in one place.
//
// Identifiers, value kinds and ranges cross-checked with
// snd-firewire-ctl-services (LGPL-3.0) protocols/motu/src/command_dsp.rs:
// MonitorCmd, MixerCmd, InputCmd, OutputCmd, ReverbCmd and the
// MotuCommandDsp*Specification constants. Every non-byte value travels as a
// float32 (the reference casts its integer parameters from it); booleans and
// enums are one byte. Written fresh; no code copied.

#pragma once

#include "MotuCommandDsp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <span>

namespace ASFW::Audio::Motu::CommandDsp {

enum class Kind : uint8_t { Bool, Enum, Float };

/// What the identifier's channel byte counts.
enum class Scope : uint8_t {
    Global,       ///< channel 0 only
    Input,        ///< one per DSP input
    Output,       ///< one per DSP output (stereo pair)
    Mixer,        ///< one per mix
    MixerSource,  ///< one per mix; the source is second level 2 + index
};

struct ParamSpec final {
    Block block;
    uint8_t second;  ///< for MixerSource: the base, 2
    uint8_t third;
    Kind kind;
    Scope scope;
    float min{0.0f};
    float max{1.0f};
    float step{0.0f};  ///< 0: continuous
    const char* name{""};
    std::span<const char* const> items{};
};

namespace Items {
inline constexpr const char* kRollOff[] = {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "30 dB/oct", "36 dB/oct"};
inline constexpr const char* kFilter5[] = {"Type 1", "Type 2", "Type 3", "Type 4", "Shelf"};
inline constexpr const char* kFilter4[] = {"Type 1", "Type 2", "Type 3", "Type 4"};
inline constexpr const char* kDetect[] = {"Peak", "RMS"};
inline constexpr const char* kLeveler[] = {"Compress", "Limit"};
inline constexpr const char* kRoom[] = {"A", "B", "C", "D", "E"};
inline constexpr const char* kSplit[] = {"Output", "Mixer"};
inline constexpr const char* kSourcePair[] = {"Width", "L/R Balance"};
inline constexpr const char* kInputPair[] = {"Left/Right", "Mono/Stereo"};
} // namespace Items

namespace Detail {
constexpr ParamSpec B(Block b, uint8_t s, uint8_t t, Scope sc, const char* n) {
    return {b, s, t, Kind::Bool, sc, 0, 1, 0, n, {}};
}
constexpr ParamSpec E(Block b, uint8_t s, uint8_t t, Scope sc, const char* n, std::span<const char* const> items) {
    // An empty list is filled from the model's ports (destinations).
    return {b, s, t, Kind::Enum, sc, 0, items.empty() ? 0.0f : static_cast<float>(items.size() - 1), 1, n, items};
}
constexpr ParamSpec F(Block b, uint8_t s, uint8_t t, Scope sc, float lo, float hi, float step, const char* n) {
    return {b, s, t, Kind::Float, sc, lo, hi, step, n, {}};
}
// The EQ and dynamics of a strip. Inputs start the EQ at second level 1,
// outputs at 0, so outputs pass base - 1.
#define ASFW_MOTU_STRIP(BLOCK, SCOPE, BASE)                                                        \
    B(BLOCK, BASE + 0, 0, SCOPE, "EQ"),                                                            \
    B(BLOCK, BASE + 1, 0, SCOPE, "HPF"), E(BLOCK, BASE + 1, 1, SCOPE, "HPF Slope", Items::kRollOff), \
    F(BLOCK, BASE + 1, 2, SCOPE, 20, 20000, 1, "HPF Freq"),                                        \
    B(BLOCK, BASE + 2, 0, SCOPE, "LF"), E(BLOCK, BASE + 2, 1, SCOPE, "LF Type", Items::kFilter5),  \
    F(BLOCK, BASE + 2, 2, SCOPE, 20, 20000, 1, "LF Freq"), F(BLOCK, BASE + 2, 3, SCOPE, -20, 20, 0, "LF Gain"), \
    F(BLOCK, BASE + 2, 4, SCOPE, 0.01f, 3, 0, "LF Width"),                                         \
    B(BLOCK, BASE + 3, 0, SCOPE, "LMF"), E(BLOCK, BASE + 3, 1, SCOPE, "LMF Type", Items::kFilter4), \
    F(BLOCK, BASE + 3, 2, SCOPE, 20, 20000, 1, "LMF Freq"), F(BLOCK, BASE + 3, 3, SCOPE, -20, 20, 0, "LMF Gain"), \
    F(BLOCK, BASE + 3, 4, SCOPE, 0.01f, 3, 0, "LMF Width"),                                        \
    B(BLOCK, BASE + 4, 0, SCOPE, "MF"), E(BLOCK, BASE + 4, 1, SCOPE, "MF Type", Items::kFilter4),  \
    F(BLOCK, BASE + 4, 2, SCOPE, 20, 20000, 1, "MF Freq"), F(BLOCK, BASE + 4, 3, SCOPE, -20, 20, 0, "MF Gain"), \
    F(BLOCK, BASE + 4, 4, SCOPE, 0.01f, 3, 0, "MF Width"),                                         \
    B(BLOCK, BASE + 5, 0, SCOPE, "HMF"), E(BLOCK, BASE + 5, 1, SCOPE, "HMF Type", Items::kFilter4), \
    F(BLOCK, BASE + 5, 2, SCOPE, 20, 20000, 1, "HMF Freq"), F(BLOCK, BASE + 5, 3, SCOPE, -20, 20, 0, "HMF Gain"), \
    F(BLOCK, BASE + 5, 4, SCOPE, 0.01f, 3, 0, "HMF Width"),                                        \
    B(BLOCK, BASE + 6, 0, SCOPE, "HF"), E(BLOCK, BASE + 6, 1, SCOPE, "HF Type", Items::kFilter5),  \
    F(BLOCK, BASE + 6, 2, SCOPE, 20, 20000, 1, "HF Freq"), F(BLOCK, BASE + 6, 3, SCOPE, -20, 20, 0, "HF Gain"), \
    F(BLOCK, BASE + 6, 4, SCOPE, 0.01f, 3, 0, "HF Width"),                                         \
    B(BLOCK, BASE + 7, 0, SCOPE, "LPF"), E(BLOCK, BASE + 7, 1, SCOPE, "LPF Slope", Items::kRollOff), \
    F(BLOCK, BASE + 7, 2, SCOPE, 20, 20000, 1, "LPF Freq"),                                        \
    B(BLOCK, BASE + 8, 0, SCOPE, "Dynamics"),                                                      \
    B(BLOCK, BASE + 9, 0, SCOPE, "Compressor"), F(BLOCK, BASE + 9, 1, SCOPE, -48, 0, 1, "Comp Threshold"), \
    F(BLOCK, BASE + 9, 2, SCOPE, 1, 10, 0, "Comp Ratio"), F(BLOCK, BASE + 9, 3, SCOPE, 10, 100, 1, "Comp Attack"), \
    F(BLOCK, BASE + 9, 4, SCOPE, 10, 100, 1, "Comp Release"), F(BLOCK, BASE + 9, 5, SCOPE, -6, 0, 0, "Comp Gain"), \
    E(BLOCK, BASE + 9, 6, SCOPE, "Comp Detect", Items::kDetect),                                   \
    B(BLOCK, BASE + 10, 0, SCOPE, "Leveler"), E(BLOCK, BASE + 10, 1, SCOPE, "Leveler Mode", Items::kLeveler), \
    F(BLOCK, BASE + 10, 2, SCOPE, 0, 100, 1, "Leveler Makeup"), F(BLOCK, BASE + 10, 3, SCOPE, 0, 100, 1, "Leveler Reduce")
} // namespace Detail

using Detail::B;
using Detail::E;
using Detail::F;

inline constexpr ParamSpec kParams[] = {
    // Monitor (MonitorCmd). Focus is a UI selection, not a parameter, and is left out.
    F(Block::Monitor, 0, 0x00, Scope::Global, 0, 1, 0, "Main Volume"),
    B(Block::Monitor, 0, 0x01, Scope::Global, "Talkback"),
    B(Block::Monitor, 0, 0x02, Scope::Global, "Listenback"),
    F(Block::Monitor, 0, 0x05, Scope::Global, 0, 1, 0, "Talkback Volume"),
    F(Block::Monitor, 0, 0x06, Scope::Global, 0, 1, 0, "Listenback Volume"),
    // Return Assign is an output index; its items are the model's outputs.
    E(Block::Monitor, 0, 0x08, Scope::Global, "Return Assign", {}),
    // Mixer (MixerCmd). Output Assign's items are the model's outputs.
    E(Block::Mixer, 0, 0x00, Scope::Mixer, "Destination", {}),
    B(Block::Mixer, 0, 0x01, Scope::Mixer, "Mute"),
    F(Block::Mixer, 0, 0x02, Scope::Mixer, 0, 1, 0, "Volume"),
    F(Block::Mixer, 1, 0x00, Scope::Mixer, 0, 1, 0, "Reverb Send"),
    F(Block::Mixer, 1, 0x01, Scope::Mixer, 0, 1, 0, "Reverb Return"),
    B(Block::Mixer, kMixerSourceSecondLevelBase, 0x00, Scope::MixerSource, "Mute"),
    B(Block::Mixer, kMixerSourceSecondLevelBase, 0x01, Scope::MixerSource, "Solo"),
    F(Block::Mixer, kMixerSourceSecondLevelBase, 0x02, Scope::MixerSource, -1, 1, 0, "Pan"),
    F(Block::Mixer, kMixerSourceSecondLevelBase, 0x03, Scope::MixerSource, 0, 1, 0, "Gain"),
    E(Block::Mixer, kMixerSourceSecondLevelBase, 0x04, Scope::MixerSource, "Stereo Mode", Items::kSourcePair),
    F(Block::Mixer, kMixerSourceSecondLevelBase, 0x05, Scope::MixerSource, -1, 1, 0, "Stereo Balance"),
    F(Block::Mixer, kMixerSourceSecondLevelBase, 0x06, Scope::MixerSource, 0, 1, 0, "Stereo Width"),
    // Input strips (InputCmd). Pad, nominal level, phantom, limiter, lookahead
    // and soft clip are microphone functions; the UltraLite mk3 hard-wires them
    // (version_3.rs: MIC_COUNT 0 for the FireWire model).
    B(Block::Input, 0, 0x00, Scope::Input, "Phase"),
    B(Block::Input, 0, 0x01, Scope::Input, "Stereo Pair"),
    F(Block::Input, 0, 0x02, Scope::Input, -96, 22, 1, "Trim"),
    B(Block::Input, 0, 0x03, Scope::Input, "Swap"),
    E(Block::Input, 0, 0x04, Scope::Input, "Stereo Mode", Items::kInputPair),
    F(Block::Input, 0, 0x05, Scope::Input, 0, 1, 0, "Width"),
    ASFW_MOTU_STRIP(Block::Input, Scope::Input, 1),
    F(Block::Input, 0x0c, 0x00, Scope::Input, 0, 1, 0, "Reverb Send"),
    F(Block::Input, 0x0c, 0x02, Scope::Input, -1, 1, 0, "Reverb Balance"),
    // Output strips (OutputCmd).
    ASFW_MOTU_STRIP(Block::Output, Scope::Output, 0),
    F(Block::Output, 0x0b, 0x00, Scope::Output, 0, 1, 0, "Reverb Send"),
    F(Block::Output, 0x0b, 0x01, Scope::Output, 0, 1, 0, "Reverb Return"),
    B(Block::Output, 0x0c, 0x00, Scope::Output, "Monitor"),
    B(Block::Output, 0x0c, 0x01, Scope::Output, "Talkback"),
    B(Block::Output, 0x0c, 0x02, Scope::Output, "Listenback"),
    // Reverb (ReverbCmd).
    B(Block::Reverb, 0, 0x00, Scope::Global, "Reverb"),
    E(Block::Reverb, 0, 0x01, Scope::Global, "Reverb Split", Items::kSplit),
    F(Block::Reverb, 0, 0x02, Scope::Global, 0, 100, 1, "Pre-Delay"),
    F(Block::Reverb, 0, 0x03, Scope::Global, 1000, 20000, 1, "Shelf Freq"),
    F(Block::Reverb, 0, 0x04, Scope::Global, -40, 0, 1, "Shelf Attenuation"),
    F(Block::Reverb, 0, 0x05, Scope::Global, 100, 60000, 1, "Decay Time"),
    F(Block::Reverb, 0, 0x06, Scope::Global, 0, 100, 1, "Low Freq Time"),
    F(Block::Reverb, 0, 0x07, Scope::Global, 0, 100, 1, "Mid Freq Time"),
    F(Block::Reverb, 0, 0x08, Scope::Global, 0, 100, 1, "High Freq Time"),
    F(Block::Reverb, 0, 0x09, Scope::Global, 100, 20000, 1, "Low Crossover"),
    F(Block::Reverb, 0, 0x0a, Scope::Global, 100, 20000, 1, "High Crossover"),
    F(Block::Reverb, 0, 0x0b, Scope::Global, -1, 1, 0, "Reverb Width"),
    E(Block::Reverb, 0, 0x0c, Scope::Global, "Room Shape", Items::kRoom),
    F(Block::Reverb, 0, 0x0d, Scope::Global, 50, 400, 1, "Reflection Size"),
    F(Block::Reverb, 0, 0x0e, Scope::Global, 0, 1, 0, "Reflection Level"),
};
#undef ASFW_MOTU_STRIP

/// The DSP ports of a model.
struct ModelDspPorts final {
    std::span<const char* const> sources;  ///< mixer inputs
    std::span<const char* const> outputs;  ///< mixer destinations and output strips
    std::span<const char* const> inputs;   ///< input strips

    [[nodiscard]] constexpr size_t Count(Scope scope) const noexcept {
        switch (scope) {
        case Scope::Global: return 1;
        case Scope::Input: return inputs.size();
        case Scope::Output: return outputs.size();
        case Scope::Mixer: return kMixerCount;
        case Scope::MixerSource: return kMixerCount;
        }
        return 0;
    }
};

inline constexpr const char* kUltraLiteMk3Inputs[] = {
    "Analog 1", "Analog 2", "Analog 3", "Analog 4", "Analog 5",
    "Analog 6", "Analog 7", "Analog 8", "S/PDIF 1", "S/PDIF 2"};
inline constexpr const char* kUltraLiteMk3Outputs[] = {
    "Main Out", "Analog 1-2", "Analog 3-4", "Analog 5-6", "Analog 7-8", "S/PDIF", "Phones"};

/// The DSP ports of a command-DSP model this code knows (version_3.rs), or nothing.
[[nodiscard]] constexpr std::optional<ModelDspPorts> DspPortsFor(uint32_t unitSwVersion) noexcept {
    if (unitSwVersion == 0x19)
        return ModelDspPorts{kUltraLiteMk3Inputs, kUltraLiteMk3Outputs, kUltraLiteMk3Inputs};
    return std::nullopt;
}

/// One parameter of one channel, as the unit addresses it.
struct ParamAddress final {
    const ParamSpec* spec{nullptr};
    uint8_t channel{0};
    uint8_t source{0};  ///< MixerSource only
};

/// The spec a command addresses, or nothing.
[[nodiscard]] inline std::optional<ParamAddress> Lookup(const ModelDspPorts& ports, const Command& c) noexcept {
    for (const auto& spec : kParams) {
        if (static_cast<uint8_t>(spec.block) != c.First() || spec.third != c.Third()) continue;
        uint8_t source = 0;
        if (spec.scope == Scope::MixerSource) {
            if (c.Second() < kMixerSourceSecondLevelBase) continue;
            source = static_cast<uint8_t>(c.Second() - kMixerSourceSecondLevelBase);
            if (source >= ports.sources.size()) return std::nullopt;
        } else if (spec.second != c.Second()) {
            continue;
        }
        if (c.Channel() >= ports.Count(spec.scope)) return std::nullopt;
        if ((spec.kind == Kind::Float) != c.quadlet) return std::nullopt;
        return ParamAddress{&spec, c.Channel(), source};
    }
    return std::nullopt;
}

/// A parameter's id in the control model: 0x5 in the top nibble keeps bit 31
/// clear (ASFW reserves it) and away from the clock source's 'MCLK'.
[[nodiscard]] constexpr uint32_t ParameterId(const ParamSpec& spec, uint8_t channel, uint8_t source) noexcept {
    const uint8_t second = spec.scope == Scope::MixerSource
        ? static_cast<uint8_t>(kMixerSourceSecondLevelBase + source) : spec.second;
    return 0x50000000U | (uint32_t{static_cast<uint8_t>(spec.block)} << 24) | (uint32_t{second} << 16) |
           (uint32_t{spec.third} << 8) | channel;
}
[[nodiscard]] constexpr uint32_t Id(Block block, uint8_t second, uint8_t third, uint8_t channel) noexcept {
    return 0x50000000U | (uint32_t{static_cast<uint8_t>(block)} << 24) | (uint32_t{second} << 16) |
           (uint32_t{third} << 8) | channel;
}
[[nodiscard]] constexpr bool IsDspParameterId(uint32_t id) noexcept {
    return (id & 0xf8000000U) == 0x50000000U && ((id >> 24) & 0x07U) <= static_cast<uint8_t>(Block::Reverb);
}
/// The command an id addresses, without its value.
[[nodiscard]] constexpr Command AddressOf(uint32_t id) noexcept {
    return {{static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id >> 16),
             static_cast<uint8_t>((id >> 24) & 0x07U)}, false, 0, 0.0f};
}

/// One value: a byte or a float, as it travels.
struct Value final {
    bool quadlet{false};
    uint8_t byte{0};
    float value{0.0f};
    friend bool operator==(const Value&, const Value&) noexcept = default;
};

/// The DSP state the unit has reported, by parameter id. Only parameters of
/// the table, on channels the model has, are kept.
class DspState final {
public:
    DspState() = default;
    explicit DspState(ModelDspPorts ports) : ports_(ports) {}

    /// Fold one command in. Returns whether it addressed a known parameter.
    bool Apply(const Command& c) {
        if (!ports_) return false;
        const auto at = Lookup(*ports_, c);
        if (!at) return false;
        values_[ParameterId(*at->spec, at->channel, at->source)] = {c.quadlet, c.byte, Conform(*at->spec, c)};
        return true;
    }
    [[nodiscard]] std::optional<Value> Get(uint32_t id) const {
        const auto found = values_.find(id);
        return found == values_.end() ? std::nullopt : std::optional{found->second};
    }
    [[nodiscard]] size_t Size() const noexcept { return values_.size(); }
    [[nodiscard]] const std::optional<ModelDspPorts>& Ports() const noexcept { return ports_; }
    friend bool operator==(const DspState& a, const DspState& b) noexcept { return a.values_ == b.values_; }

private:
    /// A float as the table can hold it. The unit sends stepped parameters as
    /// floats off the step (an UltraLite mk3 reported Analog 1 Trim as 0.12 on
    /// hardware, 2026-10-28); the reference casts them to i32, truncating
    /// (command_dsp.rs to_i32). Clamped to the range so that one value cannot
    /// invalidate the whole mixer snapshot.
    [[nodiscard]] static float Conform(const ParamSpec& spec, const Command& c) noexcept {
        if (!c.quadlet || spec.kind != Kind::Float) return c.value;
        float value = c.value;
        if (spec.step > 0.0f) value = std::trunc(value / spec.step) * spec.step;
        return std::clamp(value, spec.min, spec.max);
    }

    std::optional<ModelDspPorts> ports_{};
    std::map<uint32_t, Value> values_{};
};

} // namespace ASFW::Audio::Motu::CommandDsp
