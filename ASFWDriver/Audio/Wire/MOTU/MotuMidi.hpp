// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuMidi.hpp - MIDI in MOTU data blocks.
//
// MOTU carries no AM824 MIDI slot. A data block holds at most one MIDI byte, in
// the message bytes between the SPH quadlet and the first PCM chunk: a flag byte
// (0x01 = a byte is present) and the byte itself. Which message bytes depends on
// the model and the direction.
//
// Wire truth cross-validated with Linux sound/firewire/motu:
//   - motu-stream.c:117-131      (flag/byte offsets: 2nd quadlet 4/6, 3rd quadlet 8/7)
//   - motu-protocol-v2.c:274-318 (V2 flags: 828mk2, Traveler, UltraLite, 8pre 2nd/2nd)
//   - motu-protocol-v3.c:272-338 (V3 flags: 3rd/3rd, the Audio Express 2nd out / 3rd in)
//   - amdtp-motu.c:211-254       (one byte per block; flag 0x01; cleared when idle)
//   - amdtp-motu.c:27-31,87-88   (pacing: one byte per rate/3093 blocks)
//
// Pure logic: no DriverKit, no allocation, no logging.

#pragma once

#include "MotuBlockLayout.hpp"
#include "../../Ports/IMidiByteSink.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ASFW::Encoding::Motu {

enum class MidiSlot : uint8_t { None, SecondQuadlet, ThirdQuadlet };

struct MidiSlotOffsets final {
    uint32_t flag;
    uint32_t byte;
    friend constexpr bool operator==(const MidiSlotOffsets&, const MidiSlotOffsets&) noexcept = default;
};

inline constexpr uint32_t kMidiFlagByteOffset3rdQ = 8;
inline constexpr uint32_t kMidiByteOffset3rdQ = 7;

[[nodiscard]] constexpr std::optional<MidiSlotOffsets> OffsetsOf(MidiSlot slot) noexcept {
    switch (slot) {
    case MidiSlot::SecondQuadlet: return MidiSlotOffsets{kMidiFlagByteOffset2ndQ, kMidiByteOffset2ndQ};
    case MidiSlot::ThirdQuadlet: return MidiSlotOffsets{kMidiFlagByteOffset3rdQ, kMidiByteOffset3rdQ};
    case MidiSlot::None: break;
    }
    return std::nullopt;
}

/// The MIDI slot of a model's stream. `capture` is device -> host (Linux "TX"),
/// otherwise host -> device (Linux "RX"). One MIDI port per direction.
[[nodiscard]] constexpr MidiSlot MidiSlotFor(uint32_t version, bool capture) noexcept {
    switch (version) {
    case 0x03: case 0x09: case 0x0d: case 0x0f:
        return MidiSlot::SecondQuadlet;
    case 0x15: case 0x19: case 0x1b: case 0x30: case 0x35: case 0x39:
        return MidiSlot::ThirdQuadlet;
    case 0x33: // Audio Express: different slots per direction.
        return capture ? MidiSlot::ThirdQuadlet : MidiSlot::SecondQuadlet;
    default:
        return MidiSlot::None;
    }
}

struct MotuMidiDemuxCounters final {
    uint64_t blocksInspected{0};
    uint64_t bytesDelivered{0};
};

/// Lift the MIDI bytes out of `eventCount` data blocks of `strideBytes` each.
/// Every byte goes to port 0: a MOTU stream carries one MIDI port. A block too
/// short to hold the slot is skipped, never read past.
inline void DemuxMotuMidi(const uint8_t* dataBlocks, uint32_t eventCount, size_t strideBytes,
                          MidiSlotOffsets offsets, uint64_t packetHostTicks,
                          ::ASFW::Audio::Ports::IMidiByteSink& sink,
                          MotuMidiDemuxCounters& counters) noexcept {
    if (dataBlocks == nullptr || strideBytes <= offsets.flag || strideBytes <= offsets.byte) return;
    for (uint32_t f = 0; f < eventCount; ++f) {
        const uint8_t* block = dataBlocks + static_cast<size_t>(f) * strideBytes;
        ++counters.blocksInspected;
        if ((block[offsets.flag] & kMidiFlagPresent) == 0) continue;
        sink.DeliverMidiBytes(0, block + offsets.byte, 1, packetHostTicks);
        ++counters.bytesDelivered;
    }
}

/// Host -> device pacing. The device's MIDI output runs at cable speed, so a
/// byte may go out only every `MidiDataBlockInterval(rate) + 1` data blocks;
/// the countdown runs across packets.
class MotuMidiTxPacer final {
public:
    void Configure(uint32_t sampleRateHz) noexcept {
        interval_ = static_cast<int32_t>(MidiDataBlockInterval(sampleRateHz));
        countdown_ = 0;
    }

    /// Whether one of the next `blocks` data blocks may carry a byte.
    [[nodiscard]] bool HasSlotWithin(uint32_t blocks) const noexcept {
        return countdown_ >= 0 && static_cast<uint32_t>(countdown_) < blocks;
    }

    /// Fill the MIDI slot of `blocks` data blocks. `take(uint8_t&)` yields the
    /// next pending byte and returns false when there is none; it is only asked
    /// on a block that may carry one. Every other block gets a clear slot, so
    /// stale bytes in a recycled packet buffer never reach the device.
    /// Returns the number of bytes written.
    template <typename Take>
    uint32_t Compose(uint8_t* dataBlocks, uint32_t blocks, size_t strideBytes,
                     MidiSlotOffsets offsets, Take&& take) noexcept {
        if (dataBlocks == nullptr || strideBytes <= offsets.flag || strideBytes <= offsets.byte) return 0;
        uint32_t written = 0;
        for (uint32_t f = 0; f < blocks; ++f) {
            uint8_t* block = dataBlocks + static_cast<size_t>(f) * strideBytes;
            uint8_t byte = 0;
            if (countdown_ == 0 && take(byte)) {
                block[offsets.byte] = byte;
                block[offsets.flag] = kMidiFlagPresent;
                ++written;
            } else {
                block[offsets.byte] = 0;
                block[offsets.flag] = 0;
            }
            if (--countdown_ < 0) countdown_ = interval_;
        }
        return written;
    }

private:
    int32_t interval_{0};
    int32_t countdown_{0};
};

} // namespace ASFW::Encoding::Motu
