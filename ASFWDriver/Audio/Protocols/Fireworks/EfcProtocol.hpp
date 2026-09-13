// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// EfcProtocol.hpp — Echo Fireworks Command (EFC) wire contract.
//
// Fireworks devices (Echo AudioFire, Mackie Onyx 400F/1200F, Gibson RIP) are
// controlled by EFC transactions: the host block-writes a command frame to the
// device's command register and the device answers with a block write to a
// fixed response window in the *host's* address space. Streaming itself is
// plain CMP + IEC 61883-6 AM824 (blocking, SYT-unaware), which the BeBoB base
// already provides; EFC is only needed for capability discovery, clock/rate
// control and transport-mode selection.
//
// Fresh implementation. Wire layout cross-validated against
// references/linux-sound-firewire-stack/firewire/fireworks/fireworks_transaction.c
// (addresses, seqnum echo) and fireworks_command.c (header fields, categories,
// hwinfo/clock parameter layouts). No reference source is copied.
//
// Pure codec: no DriverKit, no bus access — host-testable.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ASFW::Audio::Fireworks::Efc {

// ---- Addresses --------------------------------------------------------------
// Host -> device: command frame (block write to the device).
inline constexpr uint16_t kCommandAddressHi = 0xECC0;
inline constexpr uint32_t kCommandAddressLo = 0x00000000;
inline constexpr uint64_t kCommandOffset = 0xECC000000000ULL;
// Device -> host: response frame (block write into the local node's space).
inline constexpr uint16_t kResponseAddressHi = 0xECC0;
inline constexpr uint32_t kResponseAddressLo = 0x80000000;
inline constexpr uint64_t kResponseOffset = 0xECC080000000ULL;
// Largest response frame a Fireworks device emits (Linux SND_EFW_RESPONSE_MAXIMUM_BYTES).
inline constexpr uint32_t kResponseWindowBytes = 0x200;

// ---- Framing ---------------------------------------------------------------
inline constexpr uint32_t kHeaderQuadlets = 6;   // length, version, seqnum, category, command, status
inline constexpr uint32_t kHeaderBytes = kHeaderQuadlets * 4;
inline constexpr uint32_t kProtocolVersion = 1;

// Sequence numbers: the host sends an even seqnum, the device echoes seqnum + 1.
// User-space (hwdep) clients own 0..0xFFFE on Linux; the kernel driver starts
// above that so the two never collide. Same convention here.
inline constexpr uint32_t kSeqnumFirst = 0x00010000;
inline constexpr uint32_t kSeqnumStep = 2;
inline constexpr uint32_t kSeqnumLast = 0xFFFFFFFCU;

// ---- Timing (Linux fireworks_transaction.c / fireworks_command.c) -----------
inline constexpr uint32_t kTimeoutMs = 125;      // per attempt
inline constexpr uint32_t kMaxAttempts = 3;
inline constexpr uint32_t kClockSettleMs = 150;  // GET_CLOCK reads stale for ~100 ms after SET_CLOCK

// ---- Command categories / commands -----------------------------------------
enum class Category : uint32_t {
    kHwInfo = 0,
    kTransport = 2,
    kHwCtl = 3,
};

enum class HwInfoCommand : uint32_t {
    kGetCaps = 0,
    kGetPolled = 1,
    kSetRespAddr = 2,
};

enum class TransportCommand : uint32_t {
    kSetTxMode = 0,
};

enum class HwCtlCommand : uint32_t {
    kSetClock = 0,
    kGetClock = 1,
    kIdentify = 5,
};

enum class Status : uint32_t {
    kOk = 0,
    kBad = 1,
    kBadCommand = 2,
    kCommErr = 3,
    kBadQuadCount = 4,
    kUnsupported = 5,
    k1394Timeout = 6,
    kDspTimeout = 7,
    kBadRate = 8,
    kBadClock = 9,
    kBadChannel = 10,
    kBadPan = 11,
    kFlashBusy = 12,
    kBadMirror = 13,
    kBadLed = 14,
    kBadParameter = 15,
    kIncomplete = 0x80000000,
};

[[nodiscard]] const char* StatusName(uint32_t status) noexcept;

enum class ClockSource : uint32_t {
    kInternal = 0,
    kWordClock = 2,
    kSpdif = 3,
    kAdat1 = 4,
    kAdat2 = 5,
    kContinuous = 6,
};

enum class TransportMode : uint32_t {
    kWindows = 0,
    kIec61883 = 1,
};

enum class PhysGroupType : uint8_t {
    kAnalog = 0,
    kSpdif = 1,
    kAdat = 2,
    kSpdifOrAdat = 3,
    kAnalogMirroring = 4,
    kHeadphones = 5,
    kI2s = 6,
    kGuitar = 7,
    kPiezoGuitar = 8,
    kGuitarString = 9,
};

// ---- Frames ----------------------------------------------------------------
struct Header {
    uint32_t lengthQuadlets{0};  // whole frame, header included
    uint32_t version{0};
    uint32_t seqnum{0};
    uint32_t category{0};
    uint32_t command{0};
    uint32_t status{0};
};

struct Response {
    Header header{};
    // Parameter bytes exactly as they arrived on the wire (big-endian quadlets).
    // Kept raw because hwinfo carries char[] name fields that must not be
    // byte-swapped, while numeric fields are read through Quadlet().
    std::vector<uint8_t> paramBytes{};

    [[nodiscard]] size_t ParamQuadletCount() const noexcept { return paramBytes.size() / 4; }
    [[nodiscard]] uint32_t Quadlet(size_t index) const noexcept;
};

[[nodiscard]] uint32_t ReadBigEndian32(const uint8_t* bytes) noexcept;
void WriteBigEndian32(uint8_t* bytes, uint32_t value) noexcept;

// Build a command frame: header + params, big-endian quadlets.
[[nodiscard]] std::vector<uint8_t> EncodeCommand(uint32_t seqnum,
                                                 Category category,
                                                 uint32_t command,
                                                 std::span<const uint32_t> params);

// Parse a response frame. Rejects frames shorter than the header or whose
// length field disagrees with the payload (short payloads are truncated to
// the advertised length; longer payloads are trimmed).
[[nodiscard]] std::optional<Response> DecodeResponse(std::span<const uint8_t> wire);

// The device answers with seqnum + 1 and echoes category/command.
[[nodiscard]] bool ResponseMatches(const Response& response,
                                   uint32_t commandSeqnum,
                                   Category category,
                                   uint32_t command) noexcept;

[[nodiscard]] constexpr uint32_t NextSeqnum(uint32_t current) noexcept {
    if (current < kSeqnumFirst || current >= kSeqnumLast) {
        return kSeqnumFirst;
    }
    return current + kSeqnumStep;
}

// ---- HWINFO GET_CAPS -------------------------------------------------------
inline constexpr size_t kHwInfoNameBytes = 32;
inline constexpr size_t kHwInfoMaxGroups = 8;
inline constexpr size_t kMultiplierModes = 3;  // 1x, 2x, 4x
// Quadlets a firmware must return before the 2x/4x channel counts, which
// older firmware omits (min_sample_rate is the last mandatory field).
inline constexpr size_t kHwInfoMinQuadlets = 40;
inline constexpr size_t kHwInfoFullQuadlets = 65;

struct PhysGroup {
    uint8_t type{0};
    uint8_t count{0};
};

struct HwInfo {
    uint32_t flags{0};
    uint64_t guid{0};
    uint32_t type{0};          // model id as the firmware sees it
    uint32_t version{0};
    std::array<char, kHwInfoNameBytes + 1> vendorName{};
    std::array<char, kHwInfoNameBytes + 1> modelName{};
    uint32_t supportedClocks{0};   // bitmask over ClockSource
    // AMDTP PCM channels per multiplier mode: [0]=1x, [1]=2x, [2]=4x.
    std::array<uint32_t, kMultiplierModes> rxPcmChannels{};  // host -> device (playback)
    std::array<uint32_t, kMultiplierModes> txPcmChannels{};  // device -> host (capture)
    uint32_t physOut{0};
    uint32_t physIn{0};
    uint32_t physOutGroupCount{0};
    std::array<PhysGroup, kHwInfoMaxGroups> physOutGroups{};
    uint32_t physInGroupCount{0};
    std::array<PhysGroup, kHwInfoMaxGroups> physInGroups{};
    uint32_t midiOutPorts{0};
    uint32_t midiInPorts{0};
    uint32_t maxSampleRate{0};
    uint32_t minSampleRate{0};
    uint32_t dspVersion{0};
    uint32_t armVersion{0};
    uint32_t mixerPlaybackChannels{0};
    uint32_t mixerCaptureChannels{0};
    uint32_t fpgaVersion{0};
    bool hasMultiplierCounts{false};  // 2x/4x fields present

    [[nodiscard]] bool SupportsClockSource(ClockSource source) const noexcept {
        return (supportedClocks & (1U << static_cast<uint32_t>(source))) != 0;
    }
};

[[nodiscard]] std::optional<HwInfo> ParseHwInfo(std::span<const uint8_t> paramBytes);

// Rate multiplier mode: 0 for <= 48 kHz, 1 for 88.2/96 kHz, 2 for 176.4/192 kHz.
[[nodiscard]] std::optional<uint32_t> MultiplierModeForRate(uint32_t sampleRateHz) noexcept;

// ---- HWCTL GET_CLOCK / SET_CLOCK --------------------------------------------
struct Clock {
    uint32_t source{0};        // ClockSource
    uint32_t sampleRateHz{0};
    uint32_t index{0};
};

inline constexpr size_t kClockQuadlets = 3;

[[nodiscard]] std::optional<Clock> ParseClock(const Response& response) noexcept;
[[nodiscard]] std::array<uint32_t, kClockQuadlets> EncodeClock(const Clock& clock) noexcept;

} // namespace ASFW::Audio::Fireworks::Efc
