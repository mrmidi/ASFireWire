#pragma once

#include <cstdint>

namespace ASFW::Audio {

// Saffire isoch buffer / latency model, lifted from the Saffire.kext
// decompile (Saffire::UpdateIsochBufferParams, 0xf506) — the constants the
// original driver fed IOAudioFamily's setSampleOffset/setSampleLatency.
//
// Provenance (the trace that established this is host-internal and fully
// liftable — no bench capture in the loop):
//
//   - UpdateIsochBufferParams is written only from initHardware and
//     RestartStreaming, never from a register-read path: the values are a
//     pure latencyMode × sampleRate lookup, not device state.
//   - latencyMode is a host-side preference (Focusrite's buffer-size /
//     "safe mode" setting), not a device register.
//   - Base delay-packet table (input, output):
//         mode 0 (lowest): 14, 2
//         mode 1:          16, 6
//         mode 2:          18, 10
//         mode 3 (safest): 20, 14
//     plus a rate bump to BOTH columns: +2 at 88.2/96 kHz, +4 at 176.4/192.
//   - The same function sets the isoch DMA-program depth: 160 / 80 / 40
//     packets for ≤48 / ≤96 / ≤192 kHz. (The trace prose calls this "a
//     constant ~20 ms hardware buffer regardless of rate", but in packet
//     time 160/80/40 × 125 µs = 20/10/5 ms — only the ≤48 k family is
//     20 ms. Prose/table discrepancy, flagged for adjudication; this
//     header encodes the packet counts.)
//   - Delay in frames = delayPackets × framesPerDataPacket (8 / 16 / 32 at
//     ≤48 / ≤96 / ≤192 kHz). Because the delay is counted in packets, the
//     time-domain offset is rate-independent until the high-rate bump.
//
// Second flagged discrepancy: the trace prose says "safest mode = 160
// frames (~3.3 ms)" for the OUTPUT offset at 48 kHz, but the table gives
// output mode 3 = 14 × 8 = 112 frames (160 = the INPUT column, 20 × 8).
// This header encodes the TABLE.
//
// Note: this is DICE-family behavior above the IEC 61883 spec (a quirk, not
// spec-derived) — the numbers are device-validated, not derivable.
//
// Policy recommendation from the trace, for consumers: expose latencyMode
// as a safe-mode preference, start at mode 0–1 for TX (output delay 2–6
// packets), widen only if the engine's frames-without-packet misses climb.
// How these delay frames map onto the ADK surface (cursor offsets vs
// SetSafetyOffset/SetLatency) is TimingCursorPolicy's call — see there.

enum class SaffireLatencyMode : uint8_t {
    kLowest = 0,
    kLow = 1,
    kMedium = 2,
    kSafest = 3,
};

struct SaffireIsochBufferParams final {
    uint32_t inputDelayPackets{0};
    uint32_t outputDelayPackets{0};
    uint32_t dmaProgramDepthPackets{0};
    uint32_t framesPerDataPacket{0};

    [[nodiscard]] constexpr uint32_t InputDelayFrames() const noexcept {
        return inputDelayPackets * framesPerDataPacket;
    }
    [[nodiscard]] constexpr uint32_t OutputDelayFrames() const noexcept {
        return outputDelayPackets * framesPerDataPacket;
    }
    // 125 µs per isoch packet.
    [[nodiscard]] constexpr uint32_t DmaProgramDepthMicroseconds() const noexcept {
        return dmaProgramDepthPackets * 125u;
    }
};

class SaffireIsochLatency final {
public:
    [[nodiscard]] static constexpr bool Lookup(
        SaffireLatencyMode mode, uint32_t sampleRate,
        SaffireIsochBufferParams& outParams) noexcept {
        uint32_t rateBumpPackets = 0;
        uint32_t framesPerDataPacket = 0;
        uint32_t dmaDepthPackets = 0;
        switch (sampleRate) {
            case 44100u:
            case 48000u:
                rateBumpPackets = 0;
                framesPerDataPacket = 8;
                dmaDepthPackets = 160;
                break;
            case 88200u:
            case 96000u:
                rateBumpPackets = 2;
                framesPerDataPacket = 16;
                dmaDepthPackets = 80;
                break;
            case 176400u:
            case 192000u:
                rateBumpPackets = 4;
                framesPerDataPacket = 32;
                dmaDepthPackets = 40;
                break;
            default:
                return false; // not a Saffire-supported rate
        }

        uint32_t inputBasePackets = 0;
        uint32_t outputBasePackets = 0;
        switch (mode) {
            case SaffireLatencyMode::kLowest:
                inputBasePackets = 14;
                outputBasePackets = 2;
                break;
            case SaffireLatencyMode::kLow:
                inputBasePackets = 16;
                outputBasePackets = 6;
                break;
            case SaffireLatencyMode::kMedium:
                inputBasePackets = 18;
                outputBasePackets = 10;
                break;
            case SaffireLatencyMode::kSafest:
                inputBasePackets = 20;
                outputBasePackets = 14;
                break;
            default:
                return false;
        }

        outParams.inputDelayPackets = inputBasePackets + rateBumpPackets;
        outParams.outputDelayPackets = outputBasePackets + rateBumpPackets;
        outParams.dmaProgramDepthPackets = dmaDepthPackets;
        outParams.framesPerDataPacket = framesPerDataPacket;
        return true;
    }
};

} // namespace ASFW::Audio
