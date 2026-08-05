// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfwStreamFormats.hpp — two-tier OXFW stream-format discovery (FW-139).
//
// Chip-common, not device-specific: every OXFW device discovers its formats
// this way, so this sits in the Oxford layer rather than in a vendor overlay.
//
// Both references implement discovery as two tiers, and which tier answered is
// itself load-bearing information:
//
//   Tier 1 (reported)  Enumerate EXTENDED STREAM FORMAT LIST on unit PCR plug 0,
//                      walking the index until the device stops answering. These
//                      are the formats the device actually advertises.
//   Tier 2 (assumed)   Only if tier 1 fails at index 0: read the current format,
//                      then probe each candidate rate with SPECIFIC INQUIRY and
//                      keep the ones the device would accept.
//
// Cross-validated with snd-firewire-ctl-services oxfw/src/lib.rs:244-335 and
// the kernel equivalent (sound/firewire/oxfw/oxfw-stream.c,
// avc_stream_get_format_list). Behaviour only — no reference code copied.
//
// `assumed` records which tier answered. It is not cosmetic: the reference
// routes the *write* path off it, writing a full compound-AM824 format only
// when the device reported its formats, and only the plug signal format when we
// inferred them (lib.rs:365-407). Writing a format the device never advertised
// is how you get a device that accepts the command and then streams garbage.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <DriverKit/IOReturn.h>

#include "../../../Protocols/AVC/StreamFormats/StreamFormatTypes.hpp"

namespace ASFW::Protocols::AVC {
class FCPTransport;
}

namespace ASFW::Audio::Oxford {

/// Candidate rates probed in the assumed path, in the reference's order
/// (oxfw/src/lib.rs:244). Not a claim about any particular device — the probe
/// is what decides which of these survive.
inline constexpr uint32_t kCandidateRatesHz[] = {32000, 44100, 48000,
                                                 88200, 96000, 176400, 192000};

struct StreamFormatEntry {
    uint32_t sampleRateHz{0};
    uint8_t pcmChannels{0};
    uint8_t midiSlots{0};
};

struct StreamFormatSet {
    std::vector<StreamFormatEntry> entries;

    /// False when the device enumerated its formats (tier 1); true when they
    /// were inferred by probing (tier 2). Callers that write a format MUST
    /// consult this — see the header note.
    bool assumed{false};

    [[nodiscard]] bool SupportsRate(uint32_t rateHz) const noexcept;

    /// Rates in discovery order, deduplicated.
    [[nodiscard]] std::vector<uint32_t> Rates() const;
};

/// Reports (status, formats). `formats` is meaningful only on success; an
/// empty set is reported as an error rather than as a device with no formats,
/// because those are different facts and only one of them is actionable.
using StreamFormatSetCallback = std::function<void(IOReturn, const StreamFormatSet&)>;

/// Detect the formats of unit PCR plug 0.
///
/// `isOutput` selects the plug direction from the *device's* point of view,
/// matching the reference: output = device transmits (host capture).
void DetectStreamFormats(Protocols::AVC::FCPTransport& transport,
                         bool isOutput,
                         StreamFormatSetCallback callback);

} // namespace ASFW::Audio::Oxford
