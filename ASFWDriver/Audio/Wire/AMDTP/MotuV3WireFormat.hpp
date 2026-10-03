#pragma once

#include <cstddef>
#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP::MotuV3Wire {

inline constexpr uint32_t kCipHeaderBytes = 8;
inline constexpr uint32_t kBytesPerQuadlet = 4;
inline constexpr uint32_t kSphBytes = 4;
inline constexpr uint32_t kBytesPerChunk = 3;
inline constexpr uint32_t kMessageChunks = 2;
inline constexpr uint32_t kTicksPerCycle = 3072;
inline constexpr uint32_t kCyclesPerSecond = 8000;
// The MOTU V3 SPH is a one-second timestamp, not an OHCI cycle timer: cycle
// 0..7999 in bits 24:12, offset 0..3071 in bits 11:0, and the cycle timer's
// `seconds` field (bits 31:25) left at zero. Observed on the wire: a passive
// bus capture of the official driver on OS X 10.11 carries zero in that field
// in every data packet, both directions (host->MOTU ch33 3013/3013,
// MOTU->host ch34 3071/3071, maxRaw 0x01f3f39c). Everything derived from
// kTickDomain therefore wraps once per second: SPH encoding, the shortest-path
// tick difference in MotuPhaseTrace.hpp, and the packetizer's Q32 phase.
inline constexpr uint32_t kSecondsPerWrap = 1;
inline constexpr uint64_t kTicksPerSecond =
    static_cast<uint64_t>(kTicksPerCycle) * kCyclesPerSecond;
inline constexpr uint64_t kTickDomain = kTicksPerSecond * kSecondsPerWrap;
// Official 828 Mk3 V3 playback SPH is three bus cycles ahead of transmit.
// NOT settled at the whole-cycle level: a capture of the official driver
// measures its DIFFERENTIAL lead against the device at 6654 ticks (2.17
// cycles), and a servo that folds modulo one cycle can never see the
// difference. The comparison inherits +/-1 cycle from projected-versus-observed
// transmit cycle, so it needs its own measurement before this constant moves.
inline constexpr int64_t kPresentationLeadTicks = 3 * kTicksPerCycle;

// The presentation phase the ORIGINAL driver actually holds against the device,
// as `rel` measures it -- i.e. after kPresentationLeadTicks is subtracted.
// Measured, not assumed: +510 ticks with MAD 0, 29 of 30 bursts inside a single
// tick across 87.5 s of a bus capture of the official driver. Direct
// observation of the running official stack, but one session, 48 kHz, one
// host/card pair.
//
// Two things this value is not. It is not zero, which is what the servo assumed
// before anyone measured it. And it is not exact: it carries +/-1024 ticks,
// because the capture analysis averages the
// three cadence buckets while [RxPhaseRel] locks onto an arbitrary one of them,
// and the buckets are gcd(4096, 3072) = 1024 apart.
inline constexpr int64_t kOracleRelSetpointTicks = 510;

// Captured El Capitan wire order. The first two 24-bit chunks are messages;
// CoreAudio outputs then map to the device's fixed 14 playback positions.
// Main L/R are PCM positions 10/11, hence full chunk indices 12/13.
inline constexpr uint8_t kCoreToWireChunk[14] = {
    12, 13, 4, 5, 6, 7, 8, 9, 10, 11, 2, 3, 14, 15,
};
inline constexpr size_t kCoreOutputChannels =
    sizeof(kCoreToWireChunk) / sizeof(kCoreToWireChunk[0]);

[[nodiscard]] constexpr uint32_t BuildCipQ0(uint8_t sid,
                                             uint8_t dbs,
                                             uint8_t dbc) noexcept {
    // The captured V3 IT header byte is 0x04 in the FN/QPC/SPH octet.
    // Keep the exact wire marker here instead of routing it through the generic
    // builder, whose field naming does not describe this proprietary layout.
    return (static_cast<uint32_t>(sid & 0x3fU) << 24) |
           (static_cast<uint32_t>(dbs) << 16) | 0x00000400U |
           static_cast<uint32_t>(dbc);
}

[[nodiscard]] constexpr uint32_t BuildCipQ1(uint8_t fmt,
                                             uint8_t fdf) noexcept {
    return 0x80000000U | (static_cast<uint32_t>(fmt & 0x3fU) << 24) |
           (static_cast<uint32_t>(fdf) << 16) | 0x0000ffffU;
}

[[nodiscard]] constexpr uint64_t NormalizeTicks(int64_t ticks) noexcept {
    const int64_t domain = static_cast<int64_t>(kTickDomain);
    ticks %= domain;
    if (ticks < 0) {
        ticks += domain;
    }
    return static_cast<uint64_t>(ticks);
}

// Bits 31:25 are deliberately not written. They exist in the OHCI cycle timer
// this value is derived from, but the device's own stream leaves them zero, so
// filling them would put a field we do not control on the wire.
[[nodiscard]] constexpr uint32_t EncodeSph(int64_t ticks) noexcept {
    const uint64_t normalized = NormalizeTicks(ticks);
    const uint32_t cycle = static_cast<uint32_t>(normalized / kTicksPerCycle);
    const uint32_t offset = static_cast<uint32_t>(normalized % kTicksPerCycle);
    return (cycle << 12) | offset;
}

// The two facts above are load-bearing together: dropping the `seconds` term is
// only lossless while the tick domain is exactly one second. A future edit that
// widens kSecondsPerWrap without restoring the field would alias silently on
// the wire instead of failing here.
static_assert(kTickDomain == kTicksPerSecond,
              "EncodeSph drops the seconds field, so the tick domain must be "
              "exactly one second.");
static_assert(EncodeSph(static_cast<int64_t>(kTicksPerSecond) - 1) <= 0x01FFFFFFu,
              "SPH must never reach the cycle timer's seconds field (31:25).");

[[nodiscard]] constexpr uint32_t AvailableChunks(uint8_t dbs) noexcept {
    const uint32_t blockBytes = static_cast<uint32_t>(dbs) * kBytesPerQuadlet;
    return blockBytes > kSphBytes
        ? (blockBytes - kSphBytes) / kBytesPerChunk
        : 0;
}

} // namespace ASFW::Protocols::Audio::AMDTP::MotuV3Wire
