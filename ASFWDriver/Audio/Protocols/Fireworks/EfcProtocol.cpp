// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// EfcProtocol.cpp — Echo Fireworks Command (EFC) codec. See EfcProtocol.hpp.

#include "EfcProtocol.hpp"

#include <algorithm>
#include <cstring>

namespace ASFW::Audio::Fireworks::Efc {

namespace {

constexpr size_t kIdxLength = 0;
constexpr size_t kIdxVersion = 1;
constexpr size_t kIdxSeqnum = 2;
constexpr size_t kIdxCategory = 3;
constexpr size_t kIdxCommand = 4;
constexpr size_t kIdxStatus = 5;

// HWINFO GET_CAPS parameter layout, in quadlets after the header.
constexpr size_t kHwFlags = 0;
constexpr size_t kHwGuidHi = 1;
constexpr size_t kHwGuidLo = 2;
constexpr size_t kHwType = 3;
constexpr size_t kHwVersion = 4;
constexpr size_t kHwVendorName = 5;   // 8 quadlets
constexpr size_t kHwModelName = 13;   // 8 quadlets
constexpr size_t kHwSupportedClocks = 21;
constexpr size_t kHwRxPcm = 22;
constexpr size_t kHwTxPcm = 23;
constexpr size_t kHwPhysOut = 24;
constexpr size_t kHwPhysIn = 25;
constexpr size_t kHwPhysOutGroupCount = 26;
constexpr size_t kHwPhysOutGroups = 27;  // 4 quadlets (8 x {type, count})
constexpr size_t kHwPhysInGroupCount = 31;
constexpr size_t kHwPhysInGroups = 32;   // 4 quadlets
constexpr size_t kHwMidiOut = 36;
constexpr size_t kHwMidiIn = 37;
constexpr size_t kHwMaxRate = 38;
constexpr size_t kHwMinRate = 39;
constexpr size_t kHwDspVersion = 40;
constexpr size_t kHwArmVersion = 41;
constexpr size_t kHwMixerPlayback = 42;
constexpr size_t kHwMixerCapture = 43;
constexpr size_t kHwFpgaVersion = 44;
constexpr size_t kHwRxPcm2x = 45;
constexpr size_t kHwTxPcm2x = 46;
constexpr size_t kHwRxPcm4x = 47;
constexpr size_t kHwTxPcm4x = 48;

void CopyName(std::span<const uint8_t> paramBytes, size_t firstQuadlet,
              std::array<char, kHwInfoNameBytes + 1>& out) noexcept {
    out.fill('\0');
    const size_t offset = firstQuadlet * 4;
    if (paramBytes.size() < offset + kHwInfoNameBytes) {
        return;
    }
    std::memcpy(out.data(), paramBytes.data() + offset, kHwInfoNameBytes);
    out[kHwInfoNameBytes] = '\0';
}

void CopyGroups(std::span<const uint8_t> paramBytes, size_t firstQuadlet,
                std::array<PhysGroup, kHwInfoMaxGroups>& out) noexcept {
    const size_t offset = firstQuadlet * 4;
    for (size_t i = 0; i < kHwInfoMaxGroups; ++i) {
        const size_t at = offset + i * 2;
        if (at + 1 >= paramBytes.size()) {
            break;
        }
        out[i] = PhysGroup{.type = paramBytes[at], .count = paramBytes[at + 1]};
    }
}

[[nodiscard]] uint32_t QuadletOrZero(std::span<const uint8_t> paramBytes, size_t index) noexcept {
    const size_t offset = index * 4;
    if (paramBytes.size() < offset + 4) {
        return 0;
    }
    return ReadBigEndian32(paramBytes.data() + offset);
}

} // namespace

const char* StatusName(uint32_t status) noexcept {
    switch (static_cast<Status>(status)) {
        case Status::kOk: return "ok";
        case Status::kBad: return "bad";
        case Status::kBadCommand: return "bad command";
        case Status::kCommErr: return "comm err";
        case Status::kBadQuadCount: return "bad quad count";
        case Status::kUnsupported: return "unsupported";
        case Status::k1394Timeout: return "1394 timeout";
        case Status::kDspTimeout: return "DSP timeout";
        case Status::kBadRate: return "bad rate";
        case Status::kBadClock: return "bad clock";
        case Status::kBadChannel: return "bad channel";
        case Status::kBadPan: return "bad pan";
        case Status::kFlashBusy: return "flash busy";
        case Status::kBadMirror: return "bad mirror";
        case Status::kBadLed: return "bad LED";
        case Status::kBadParameter: return "bad parameter";
        case Status::kIncomplete: return "incomplete";
    }
    return "unknown";
}

uint32_t ReadBigEndian32(const uint8_t* bytes) noexcept {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

void WriteBigEndian32(uint8_t* bytes, uint32_t value) noexcept {
    bytes[0] = static_cast<uint8_t>(value >> 24);
    bytes[1] = static_cast<uint8_t>(value >> 16);
    bytes[2] = static_cast<uint8_t>(value >> 8);
    bytes[3] = static_cast<uint8_t>(value);
}

uint32_t Response::Quadlet(size_t index) const noexcept {
    return QuadletOrZero(paramBytes, index);
}

std::vector<uint8_t> EncodeCommand(uint32_t seqnum,
                                   Category category,
                                   uint32_t command,
                                   std::span<const uint32_t> params) {
    const uint32_t totalQuadlets = kHeaderQuadlets + static_cast<uint32_t>(params.size());
    std::vector<uint8_t> frame(static_cast<size_t>(totalQuadlets) * 4, 0);
    WriteBigEndian32(frame.data() + kIdxLength * 4, totalQuadlets);
    WriteBigEndian32(frame.data() + kIdxVersion * 4, kProtocolVersion);
    WriteBigEndian32(frame.data() + kIdxSeqnum * 4, seqnum);
    WriteBigEndian32(frame.data() + kIdxCategory * 4, static_cast<uint32_t>(category));
    WriteBigEndian32(frame.data() + kIdxCommand * 4, command);
    WriteBigEndian32(frame.data() + kIdxStatus * 4, 0);
    for (size_t i = 0; i < params.size(); ++i) {
        WriteBigEndian32(frame.data() + (kHeaderQuadlets + i) * 4, params[i]);
    }
    return frame;
}

std::optional<Response> DecodeResponse(std::span<const uint8_t> wire) {
    if (wire.size() < kHeaderBytes) {
        return std::nullopt;
    }
    Response response{};
    response.header.lengthQuadlets = ReadBigEndian32(wire.data() + kIdxLength * 4);
    response.header.version = ReadBigEndian32(wire.data() + kIdxVersion * 4);
    response.header.seqnum = ReadBigEndian32(wire.data() + kIdxSeqnum * 4);
    response.header.category = ReadBigEndian32(wire.data() + kIdxCategory * 4);
    response.header.command = ReadBigEndian32(wire.data() + kIdxCommand * 4);
    response.header.status = ReadBigEndian32(wire.data() + kIdxStatus * 4);

    if (response.header.lengthQuadlets < kHeaderQuadlets) {
        return std::nullopt;
    }
    if (response.header.version < kProtocolVersion) {
        return std::nullopt;
    }
    // Trust the smaller of the advertised and the delivered length so a
    // truncated write never reads past the payload.
    const size_t advertisedBytes = static_cast<size_t>(response.header.lengthQuadlets) * 4;
    const size_t availableBytes = std::min(advertisedBytes, wire.size());
    const size_t paramBytes = availableBytes - kHeaderBytes;
    response.paramBytes.assign(wire.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes),
                               wire.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + paramBytes));
    return response;
}

bool ResponseMatches(const Response& response,
                     uint32_t commandSeqnum,
                     Category category,
                     uint32_t command) noexcept {
    return response.header.seqnum == commandSeqnum + 1 &&
           response.header.category == static_cast<uint32_t>(category) &&
           response.header.command == command;
}

std::optional<HwInfo> ParseHwInfo(std::span<const uint8_t> paramBytes) {
    if (paramBytes.size() < kHwInfoMinQuadlets * 4) {
        return std::nullopt;
    }
    HwInfo info{};
    info.flags = QuadletOrZero(paramBytes, kHwFlags);
    info.guid = (static_cast<uint64_t>(QuadletOrZero(paramBytes, kHwGuidHi)) << 32) |
                QuadletOrZero(paramBytes, kHwGuidLo);
    info.type = QuadletOrZero(paramBytes, kHwType);
    info.version = QuadletOrZero(paramBytes, kHwVersion);
    CopyName(paramBytes, kHwVendorName, info.vendorName);
    CopyName(paramBytes, kHwModelName, info.modelName);
    info.supportedClocks = QuadletOrZero(paramBytes, kHwSupportedClocks);
    info.rxPcmChannels[0] = QuadletOrZero(paramBytes, kHwRxPcm);
    info.txPcmChannels[0] = QuadletOrZero(paramBytes, kHwTxPcm);
    info.physOut = QuadletOrZero(paramBytes, kHwPhysOut);
    info.physIn = QuadletOrZero(paramBytes, kHwPhysIn);
    info.physOutGroupCount = std::min<uint32_t>(QuadletOrZero(paramBytes, kHwPhysOutGroupCount),
                                                kHwInfoMaxGroups);
    CopyGroups(paramBytes, kHwPhysOutGroups, info.physOutGroups);
    info.physInGroupCount = std::min<uint32_t>(QuadletOrZero(paramBytes, kHwPhysInGroupCount),
                                               kHwInfoMaxGroups);
    CopyGroups(paramBytes, kHwPhysInGroups, info.physInGroups);
    info.midiOutPorts = QuadletOrZero(paramBytes, kHwMidiOut);
    info.midiInPorts = QuadletOrZero(paramBytes, kHwMidiIn);
    info.maxSampleRate = QuadletOrZero(paramBytes, kHwMaxRate);
    info.minSampleRate = QuadletOrZero(paramBytes, kHwMinRate);
    info.dspVersion = QuadletOrZero(paramBytes, kHwDspVersion);
    info.armVersion = QuadletOrZero(paramBytes, kHwArmVersion);
    info.mixerPlaybackChannels = QuadletOrZero(paramBytes, kHwMixerPlayback);
    info.mixerCaptureChannels = QuadletOrZero(paramBytes, kHwMixerCapture);
    info.fpgaVersion = QuadletOrZero(paramBytes, kHwFpgaVersion);
    if (paramBytes.size() >= (kHwTxPcm4x + 1) * 4) {
        info.hasMultiplierCounts = true;
        info.rxPcmChannels[1] = QuadletOrZero(paramBytes, kHwRxPcm2x);
        info.txPcmChannels[1] = QuadletOrZero(paramBytes, kHwTxPcm2x);
        info.rxPcmChannels[2] = QuadletOrZero(paramBytes, kHwRxPcm4x);
        info.txPcmChannels[2] = QuadletOrZero(paramBytes, kHwTxPcm4x);
    }
    return info;
}

std::optional<uint32_t> MultiplierModeForRate(uint32_t sampleRateHz) noexcept {
    switch (sampleRateHz) {
        case 32000U:
        case 44100U:
        case 48000U:
            return 0U;
        case 88200U:
        case 96000U:
            return 1U;
        case 176400U:
        case 192000U:
            return 2U;
        default:
            return std::nullopt;
    }
}

std::optional<Clock> ParseClock(const Response& response) noexcept {
    if (response.ParamQuadletCount() < kClockQuadlets) {
        return std::nullopt;
    }
    return Clock{.source = response.Quadlet(0),
                 .sampleRateHz = response.Quadlet(1),
                 .index = response.Quadlet(2)};
}

std::array<uint32_t, kClockQuadlets> EncodeClock(const Clock& clock) noexcept {
    return {clock.source, clock.sampleRateHz, clock.index};
}

} // namespace ASFW::Audio::Fireworks::Efc
