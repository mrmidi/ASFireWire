// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DICETypes.hpp"
#include "../../AudioTypes.hpp"
#include "../../../Runtime/ResolvedAudioConfiguration.hpp"
#include "../../../../Common/WireFormat.hpp"

#include <array>
#include <optional>
#include <span>

namespace ASFW::Audio::DICE {

// Wire facts only. Missing modes are unknown, never inferred from another
// mode's channel counts. Linux dice-extension.c:84-137 and dice-stream.c:609-672.
struct DiceModeFormat {
    std::vector<Runtime::RateWireStream> playback;
    std::vector<Runtime::RateWireStream> capture;
};
using DiceRateFormats = std::array<std::optional<DiceModeFormat>, 3>;

[[nodiscard]] inline AudioStreamRuntimeCaps MakeDiceRuntimeCaps(
    const GlobalState& global, const StreamConfig& tx, const StreamConfig& rx,
    bool exposeCapture) {
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = exposeCapture ? tx.TotalPcmChannels() : 0,
        .hostOutputPcmChannels = rx.TotalPcmChannels(),
        .deviceToHostAm824Slots = tx.TotalAm824Slots(),
        .hostToDeviceAm824Slots = rx.TotalAm824Slots(),
        .sampleRateHz = global.sampleRate,
        .deviceRateMask = DiceDeviceRateMask(global.hasClockCaps, global.clockCaps),
        .deviceToHostIsoChannel = tx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel),
        .hostToDeviceIsoChannel = rx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel),
        .deviceToHostStreamCount = tx.numStreams,
        .hostToDeviceStreamCount = rx.numStreams,
    };
    const auto copy = [](const StreamConfig& source, auto& destination) {
        for (uint32_t i = 0; i < source.numStreams && i < kMaxAudioStreamsPerDirection; ++i) {
            const auto& entry = source.streams[i];
            destination[i] = {.isoChannel = entry.isoChannel >= 0 && entry.isoChannel < 64
                ? static_cast<uint8_t>(entry.isoChannel) : AudioStreamWireInfo::kInvalidIsoChannel,
                .pcmChannels = static_cast<uint16_t>(entry.pcmChannels),
                .am824Slots = static_cast<uint16_t>(entry.Am824Slots()),
                .midiPorts = static_cast<uint16_t>(entry.midiPorts)};
        }
    };
    copy(tx, caps.deviceToHostStreams);
    copy(rx, caps.hostToDeviceStreams);
    return caps;
}

[[nodiscard]] constexpr std::optional<uint32_t> DiceRateMode(uint32_t rate) noexcept {
    for (uint32_t i = 0; i < std::size(kDiceRateTable); ++i)
        if (kDiceRateTable[i].hz == rate) return i < 3 ? 0U : i < 5 ? 1U : 2U;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<DiceModeFormat>
DiceObservedFormat(const AudioStreamRuntimeCaps& caps) {
    DiceModeFormat format;
    const auto copy = [](uint32_t count, const auto& streams, auto& destination) {
        if (!count || count > kMaxAudioStreamsPerDirection) return false;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& stream = streams[i];
            const uint32_t midiSlots = stream.midiPorts != 0 ? 1 : 0;
            if (!stream.pcmChannels || stream.pcmChannels > Encoding::kMaxPcmChannels ||
                stream.midiPorts > 8 || stream.am824Slots != stream.pcmChannels + midiSlots ||
                stream.am824Slots > Encoding::kMaxAmdtpDbs) return false;
            destination.push_back(Runtime::RateWireStream{
                stream.pcmChannels, stream.am824Slots, midiSlots, {}, stream.midiPorts});
        }
        return true;
    };
    if (!copy(caps.hostToDeviceStreamCount, caps.hostToDeviceStreams, format.playback) ||
        !copy(caps.deviceToHostStreamCount, caps.deviceToHostStreams, format.capture)) return std::nullopt;
    return format;
}

/// The per-mode formats a device publishes. When the EAP read succeeded its
/// formats stand for every mode and the stream registers are not consulted
/// (Linux dice-stream.c:618-621): registers read before a clock select can
/// describe another mode (:635-643). Without EAP only the observed mode is
/// known (Linux's fallback); no clock probing, no guessed scaling.
[[nodiscard]] inline DiceRateFormats SelectPublishedFormats(bool eapRead, DiceRateFormats eap,
                                                            const AudioStreamRuntimeCaps& caps) {
    if (eapRead) return eap;
    DiceRateFormats observedOnly{};
    const auto mode = DiceRateMode(caps.sampleRateHz);
    const auto observed = DiceObservedFormat(caps);
    if (mode && observed) observedOnly[*mode] = *observed;
    return observedOnly;
}

// EAP entries carry PCM count, MIDI port count, names and AC3. Only the
// leading two quadlets are read here; entry stride is 0x10c, not eight bytes.
// Cross-validated with Linux dice-extension.c:34-41, 64-76.
inline constexpr uint32_t kDiceEapStreamEntryBytes = 0x10c;
[[nodiscard]] inline std::expected<Runtime::RateWireStream, IOReturn>
ParseDiceEapStream(std::span<const uint8_t> bytes) {
    if (bytes.size() != 8) return std::unexpected(kIOReturnBadArgument);
    const auto pcm = FW::ReadBE32(bytes.data());
    const auto ports = FW::ReadBE32(bytes.data() + 4);
    // Up to eight physical MIDI ports multiplex into one AM824 slot.
    if (!pcm || pcm > Encoding::kMaxPcmChannels || ports > 8)
        return std::unexpected(kIOReturnUnsupported);
    const uint32_t midiSlots = ports != 0 ? 1 : 0;
    if (pcm + midiSlots > Encoding::kMaxAmdtpDbs)
        return std::unexpected(kIOReturnUnsupported);
    return Runtime::RateWireStream{pcm, pcm + midiSlots, midiSlots, {}, ports};
}

[[nodiscard]] inline std::vector<Runtime::RateFormation>
DiceFormations(uint32_t mask, const DiceRateFormats& formats, bool exposeCapture = true) {
    std::vector<Runtime::RateFormation> result;
    for (const auto& rate : kDiceRateTable) {
        const auto mode = DiceRateMode(rate.hz);
        if (!(mask & rate.capsBit) || !mode || !formats[*mode]) continue;
        const auto& format = *formats[*mode];
        if (format.playback.empty() || (exposeCapture && format.capture.empty()) ||
            format.playback.size() > 2 ||
            format.capture.size() > kMaxAudioStreamsPerDirection) continue;
        result.push_back({rate.hz, Encoding::StreamMode::kBlocking,
            format.playback, exposeCapture ? format.capture : std::vector<Runtime::RateWireStream>{},
            true, false});
    }
    return result;
}

} // namespace ASFW::Audio::DICE
