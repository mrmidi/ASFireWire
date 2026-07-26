// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfwStreamFormats.cpp — see the header for the two-tier contract (FW-139).

#include "OxfwStreamFormats.hpp"

#include "../../../Logging/Logging.hpp"
#include "../../../Protocols/AVC/AVCStreamFormatCommand.hpp"
#include "../../../Protocols/AVC/FCPTransport.hpp"
#include "../../../Protocols/AVC/StreamFormats/AVCUnitPlugSignalFormatCommand.hpp"

#include <algorithm>
#include <memory>

namespace ASFW::Audio::Oxford {

namespace {

using Protocols::AVC::AVCResult;
using Protocols::AVC::AVCStreamFormatCommand;
using Protocols::AVC::StreamFormat;
using SignalFormatCommand = Protocols::AVC::StreamFormats::AVCUnitPlugSignalFormatCommand;
using SignalSampleRate = Protocols::AVC::StreamFormats::SampleRate;

/// Unit-level address for the extended stream format commands.
constexpr uint8_t kUnitSubunit = 0xFF;
constexpr uint8_t kPcrPlug0 = 0x00;

/// A device that keeps answering past this is misbehaving; the reference walks
/// until failure, but an unbounded loop against a broken device is a hang.
constexpr uint8_t kMaxListEntries = 32;

/// AV/C stream-format frequency codes (TA 2001002) are NOT the FDF/SFC codes
/// used by the plug signal format command — 48 kHz is 0x04 here and 0x02 there.
/// Conflating the two tables silently reads the wrong rate, so both directions
/// go through the named enum rather than raw bytes.
[[nodiscard]] uint32_t StreamFormatRateToHz(uint8_t code) noexcept {
    return Protocols::AVC::StreamFormats::SampleRateToHz(static_cast<SignalSampleRate>(code));
}

[[nodiscard]] SignalSampleRate HzToSampleRate(uint32_t hz) noexcept {
    switch (hz) {
        case 32000:  return SignalSampleRate::k32000Hz;
        case 44100:  return SignalSampleRate::k44100Hz;
        case 48000:  return SignalSampleRate::k48000Hz;
        case 88200:  return SignalSampleRate::k88200Hz;
        case 96000:  return SignalSampleRate::k96000Hz;
        case 176400: return SignalSampleRate::k176400Hz;
        case 192000: return SignalSampleRate::k192000Hz;
        default:     return SignalSampleRate::kUnknown;
    }
}

/// Compound AM824 carries `number_of_format_infos` pairs of
/// [channel_count][format_code]; MBLA (0x06) counts as PCM and MIDI (0x0D) as
/// MIDI slots. Anything else is left out rather than guessed at.
void SplitCompoundChannels(const StreamFormat& format, StreamFormatEntry& entry) noexcept {
    constexpr uint8_t kMbla = 0x06;
    constexpr uint8_t kMidi = 0x0D;

    // rawData holds the format block from formatType onward; the info pairs
    // start after [type][subtype][rate][flags][count].
    constexpr size_t kInfoPairsOffset = 5;
    const auto& raw = format.rawData;
    if (format.formatSubtype != 0x40 || raw.size() <= kInfoPairsOffset) {
        return;
    }

    const size_t pairs = std::min<size_t>(format.numChannels,
                                          (raw.size() - kInfoPairsOffset) / 2U);
    for (size_t i = 0; i < pairs; ++i) {
        const uint8_t count = raw[kInfoPairsOffset + (i * 2U)];
        const uint8_t code = raw[kInfoPairsOffset + (i * 2U) + 1U];
        if (code == kMbla) {
            entry.pcmChannels = static_cast<uint8_t>(entry.pcmChannels + count);
        } else if (code == kMidi) {
            entry.midiSlots = static_cast<uint8_t>(entry.midiSlots + count);
        }
    }
}

[[nodiscard]] StreamFormatEntry EntryFrom(const StreamFormat& format) noexcept {
    StreamFormatEntry entry{};
    entry.sampleRateHz = StreamFormatRateToHz(format.sampleRate);
    SplitCompoundChannels(format, entry);
    return entry;
}

struct DetectState {
    Protocols::AVC::FCPTransport& transport;
    bool isOutput{false};
    StreamFormatSetCallback callback;
    StreamFormatSet set{};
    uint8_t listIndex{0};
    size_t probeIndex{0};
    StreamFormatEntry probeTemplate{};
};

using StatePtr = std::shared_ptr<DetectState>;

void QueryListEntry(const StatePtr& state);
void BeginAssumedPath(const StatePtr& state);
void ProbeNextRate(const StatePtr& state);

void Finish(const StatePtr& state, IOReturn status) {
    if (status == kIOReturnSuccess && state->set.entries.empty()) {
        // "Answered, but advertised nothing" is not a usable format set, and
        // reporting it as success would push an empty rate list downstream.
        ASFW_LOG_ERROR(Oxfw, "stream formats: %{public}s plug 0 reported no usable entries",
                       state->isOutput ? "output" : "input");
        status = kIOReturnNotFound;
    }
    if (status == kIOReturnSuccess) {
        ASFW_LOG(Oxfw, "stream formats: %{public}s plug 0 -> %zu entries (%{public}s)",
                 state->isOutput ? "output" : "input", state->set.entries.size(),
                 state->set.assumed ? "assumed" : "reported");
    }
    auto callback = std::move(state->callback);
    if (callback) {
        callback(status, state->set);
    }
}

/// Tier 1 — walk the advertised list until the device stops answering.
void QueryListEntry(const StatePtr& state) {
    auto command = std::make_shared<AVCStreamFormatCommand>(
        state->transport, kUnitSubunit, kPcrPlug0, !state->isOutput, state->listIndex);

    command->Submit([state, command](AVCResult result,
                                     const std::optional<StreamFormat>& format) {
        if (!Protocols::AVC::IsSuccess(result) || !format.has_value()) {
            if (state->listIndex == 0) {
                // The device does not implement the list at all. This is the
                // documented trigger for the assumed path, not an error.
                ASFW_LOG(Oxfw, "stream formats: list unsupported at index 0, probing rates");
                BeginAssumedPath(state);
                return;
            }
            // Ran off the end of a list that did answer — normal termination.
            state->set.assumed = false;
            Finish(state, kIOReturnSuccess);
            return;
        }

        state->set.entries.push_back(EntryFrom(*format));

        if (state->listIndex + 1U >= kMaxListEntries) {
            ASFW_LOG_ERROR(Oxfw, "stream formats: list exceeded %u entries, truncating",
                           kMaxListEntries);
            state->set.assumed = false;
            Finish(state, kIOReturnSuccess);
            return;
        }
        state->listIndex = static_cast<uint8_t>(state->listIndex + 1U);
        QueryListEntry(state);
    });
}

/// Tier 2 entry — read the one format the device will admit to, which supplies
/// the channel layout every probed rate inherits.
void BeginAssumedPath(const StatePtr& state) {
    auto command = std::make_shared<AVCStreamFormatCommand>(
        state->transport, kUnitSubunit, kPcrPlug0, !state->isOutput);

    command->Submit([state, command](AVCResult result,
                                     const std::optional<StreamFormat>& format) {
        if (!Protocols::AVC::IsSuccess(result) || !format.has_value()) {
            ASFW_LOG_ERROR(Oxfw, "stream formats: current-format query failed (result=%d)",
                           static_cast<int>(result));
            Finish(state, kIOReturnNotResponding);
            return;
        }
        state->set.assumed = true;
        state->probeTemplate = EntryFrom(*format);
        state->probeIndex = 0;
        ProbeNextRate(state);
    });
}

/// Tier 2 body — SPECIFIC INQUIRY per candidate rate. Inquiry, never control:
/// this must not retune the device while asking what it can do.
void ProbeNextRate(const StatePtr& state) {
    if (state->probeIndex >= std::size(kCandidateRatesHz)) {
        Finish(state, kIOReturnSuccess);
        return;
    }

    const uint32_t hz = kCandidateRatesHz[state->probeIndex];
    const SignalSampleRate rate = HzToSampleRate(hz);
    if (rate == SignalSampleRate::kUnknown) {
        ++state->probeIndex;
        ProbeNextRate(state);
        return;
    }

    auto command = std::make_shared<SignalFormatCommand>(
        state->transport, kPcrPlug0, !state->isOutput,
        SignalFormatCommand::InquireRate{.rate = rate});

    command->Submit([state, command, hz](AVCResult result,
                                         SignalFormatCommand::SignalFormat) {
        if (Protocols::AVC::IsSuccess(result)) {
            StreamFormatEntry entry = state->probeTemplate;
            entry.sampleRateHz = hz;
            state->set.entries.push_back(entry);
        }
        ++state->probeIndex;
        ProbeNextRate(state);
    });
}

} // namespace

bool StreamFormatSet::SupportsRate(uint32_t rateHz) const noexcept {
    return std::any_of(entries.begin(), entries.end(),
                       [rateHz](const StreamFormatEntry& entry) {
                           return entry.sampleRateHz == rateHz;
                       });
}

std::vector<uint32_t> StreamFormatSet::Rates() const {
    std::vector<uint32_t> rates;
    rates.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.sampleRateHz != 0 &&
            std::find(rates.begin(), rates.end(), entry.sampleRateHz) == rates.end()) {
            rates.push_back(entry.sampleRateHz);
        }
    }
    return rates;
}

void DetectStreamFormats(Protocols::AVC::FCPTransport& transport,
                         bool isOutput,
                         StreamFormatSetCallback callback) {
    auto state = std::make_shared<DetectState>(DetectState{
        .transport = transport,
        .isOutput = isOutput,
        .callback = std::move(callback),
    });
    QueryListEntry(state);
}

} // namespace ASFW::Audio::Oxford
