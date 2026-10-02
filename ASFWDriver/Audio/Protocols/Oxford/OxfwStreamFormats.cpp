// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfwStreamFormats.cpp — see the header for the two-tier contract (FW-139).

#include "OxfwStreamFormats.hpp"

#include "../../../Logging/Logging.hpp"
#include "../../../Protocols/AVC/Commands/GeneralCommands.hpp"
#include "../../../Protocols/AVC/Commands/StreamFormatCommand.hpp"
#include "../../../Protocols/AVC/Core/IAvcUnit.hpp"
#include "../../../Protocols/AVC/Core/RateCodes.hpp"

#include <algorithm>
#include <memory>

namespace ASFW::Audio::Oxford {

namespace {

constexpr uint8_t kPcrPlug0 = 0x00;

/// A device that keeps answering past this is misbehaving; the reference walks
/// until failure, but an unbounded loop against a broken device is a hang.
constexpr uint8_t kMaxListEntries = 32;

struct DetectState {
    /// The unit may be destroyed between steps; a dead unit ends detection
    /// silently (its caller's discovery session died with it).
    Common::LiveRef<AVC::IAvcUnit> unit;
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
    if (!state->unit) return; // Unit gone: its caller's session went with it.
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
        // One line per entry. A count plus the first rate is enough to see
        // which tier answered but not enough to act on: deciding what to
        // advertise to CoreAudio, and whether a compound-AM824 write is even
        // representable, needs the rate *and* the channel layout of each entry.
        // Discovery-time only, so the line count is bounded and cold.
        for (size_t i = 0; i < state->set.entries.size(); ++i) {
            const auto& entry = state->set.entries[i];
            ASFW_LOG(Oxfw, "stream formats: [%zu] %u Hz pcm=%u midi=%u", i, entry.sampleRateHz,
                     static_cast<unsigned>(entry.pcmChannels),
                     static_cast<unsigned>(entry.midiSlots));
        }
    }
    auto callback = std::move(state->callback);
    if (callback) {
        callback(status, state->set);
    }
}

/// Tier 1 — walk the advertised list until the device stops answering.
void QueryListEntry(const StatePtr& state) {
    const auto dir = state->isOutput ? AVC::Cmd::PlugDirection::kOutput : AVC::Cmd::PlugDirection::kInput;

    auto* unit = state->unit.Get();
    if (!unit) return;
    unit->Status(
        AVC::Cmd::StreamFormatCommand{
            .operands = AVC::Cmd::StreamFormatOperands{
                .form = AVC::Cmd::StreamFormatSubfunction::kList,
                .opcode = AVC::Cmd::StreamFormatOpcode::kExtendedStreamFormat,
                .plug = AVC::Cmd::PlugAddress::UnitPlug(dir, AVC::Cmd::UnitPlugType::kPcr, kPcrPlug0),
                .index = state->listIndex,
            },
        },
        [state](AVC::Expected<AVC::Cmd::StreamFormatReply> res) {
            if (!res) {
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

            if (res->format.kind == AVC::Cmd::StreamFormat::Kind::kCompoundAm824) {
                const auto rateHz = AVC::ToHz(res->format.compound.rate);
                state->set.entries.push_back(StreamFormatEntry{
                    .sampleRateHz = rateHz.value_or(0),
                    .pcmChannels = static_cast<uint8_t>(res->format.compound.PcmChannels()),
                    .midiSlots = static_cast<uint8_t>(res->format.compound.MidiChannels()),
                });
            }

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
    const auto dir = state->isOutput ? AVC::Cmd::PlugDirection::kOutput : AVC::Cmd::PlugDirection::kInput;

    auto* unit = state->unit.Get();
    if (!unit) return;
    unit->Status(
        AVC::Cmd::StreamFormatCommand{
            .operands = AVC::Cmd::StreamFormatOperands{
                .form = AVC::Cmd::StreamFormatSubfunction::kSingle,
                .opcode = AVC::Cmd::StreamFormatOpcode::kExtendedStreamFormat,
                .plug = AVC::Cmd::PlugAddress::UnitPlug(dir, AVC::Cmd::UnitPlugType::kPcr, kPcrPlug0),
            },
        },
        [state](AVC::Expected<AVC::Cmd::StreamFormatReply> res) {
            if (!res || res->format.kind != AVC::Cmd::StreamFormat::Kind::kCompoundAm824) {
                ASFW_LOG_ERROR(Oxfw, "stream formats: current-format query failed");
                Finish(state, kIOReturnNotResponding);
                return;
            }
            state->set.assumed = true;
            const auto rateHz = AVC::ToHz(res->format.compound.rate);
            state->probeTemplate = StreamFormatEntry{
                .sampleRateHz = rateHz.value_or(0),
                .pcmChannels = static_cast<uint8_t>(res->format.compound.PcmChannels()),
                .midiSlots = static_cast<uint8_t>(res->format.compound.MidiChannels()),
            };
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
    const auto sfc = AVC::CipSfcFromHz(hz);
    if (!sfc) {
        ++state->probeIndex;
        ProbeNextRate(state);
        return;
    }

    const auto plugDir = state->isOutput ? AVC::Cmd::PlugSignalDirection::kOutput
                                         : AVC::Cmd::PlugSignalDirection::kInput;

    auto* unit = state->unit.Get();
    if (!unit) return;
    unit->Inquiry(
        AVC::Cmd::PlugSignalFormatCommand{
            .operands = AVC::Cmd::PlugSignalFormatOperands{
                .direction = plugDir,
                .plugId = kPcrPlug0,
                .format = AVC::Cmd::PlugSignalFormat{
                    .plugId = kPcrPlug0,
                    .fmt = 0x90,
                    .fdf = {static_cast<uint8_t>(*sfc), 0xFF, 0xFF},
                },
            },
        },
        [state, hz](AVC::Expected<AVC::Cmd::PlugSignalFormat> res) {
            if (res) {
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

void DetectStreamFormats(AVC::IAvcUnit& unit,
                         bool isOutput,
                         StreamFormatSetCallback callback) {
    auto state = std::make_shared<DetectState>(DetectState{
        .unit = Common::LiveRef<AVC::IAvcUnit>(unit),
        .isOutput = isOutput,
        .callback = std::move(callback),
    });
    QueryListEntry(state);
}

} // namespace ASFW::Audio::Oxford
