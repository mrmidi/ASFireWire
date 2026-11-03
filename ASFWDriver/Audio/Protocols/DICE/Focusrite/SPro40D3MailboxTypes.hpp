// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3MailboxTypes.hpp - Pure codecs and types for the Saffire Pro 40
// (TCD3070, model 0x0000DE) DICE III DCP vendor mailbox.
//
// Wire-format facts (behavioral facts only; this is a fresh implementation,
// no LGPL code is copied):
//   - references/snd-firewire-ctl-services/protocols/dice/src/focusrite/spro40d3.rs
//     (descriptor layout + command opcodes + serializers, cited per item below)
//   - tools/firmware_analysis/Pro40d3-DCP.md (transport: 16-byte header,
//     request area at window +0x1C0, response area at +0x30)
//   - documentation/spro40d3-stage0-characterization.md (live 48-byte descriptor)
//
// Layering: this header is pure byte math. It knows no transport, no DriverKit,
// no bus concepts. The serialized engine lives in SPro40D3MailboxEngine.hpp.

#pragma once

#include "../../../../Common/WireFormat.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

// ============================================================================
// DCP window / descriptor (spro40d3.rs init_communication; Stage 0 §4)
// ============================================================================

inline constexpr uint64_t kDcpBaseAddress   = 0xFFFFE0400000ULL;
inline constexpr size_t   kDcpDescriptorSize = 48;
/// TCAT DCP magic (dates 18.10.1966 / 04.06.1973), verified on hardware
/// 2026-11-03 (documentation/evidence/stage0-tcd3070/mailbox-descriptor-raw.txt).
inline constexpr uint64_t kDcpMagic         = 0x1810196604061973ULL;
/// Owner field value when no host owns the mailbox (Stage 0 live read).
inline constexpr uint64_t kDcpOwnerNone     = 0xFFFF000000000000ULL;

/// Decoded 48-byte descriptor at kDcpBaseAddress. Only these 0x30 bytes are a
/// static zone; window offsets +0x30 and beyond drive the RPC transport
/// (tools/firmware_analysis/Pro40d3-DCP.md, dcp_HandleAsyncRequest 0x1BAB4) and
/// must never be read except paired with a completed request (Stage 0 §7a).
struct DcpDescriptor {
    uint64_t notificationAddress{}; ///< +0x08; doubles as the ownership CAS field
    uint64_t writeAddress{};        ///< +0x20; request area (window +0x1C0)
    uint64_t readAddress{};         ///< +0x28; response area (window +0x30)
    uint32_t version{};             ///< +0x14 (2 on hardware)
    uint32_t maxRequestSize{};      ///< +0x18 (1024 on hardware)
    uint32_t responseAreaSize{};    ///< +0x1C (400 on hardware; 384 payload)
};

enum class DcpCodecError : uint8_t {
    kTooShort,
    kBadMagic,
};

/// Addresses in the descriptor are stored as two big-endian quadlets with the
/// LOW half first (spro40d3.rs deserialize_address: val = a1 | a2 << 32).
[[nodiscard]] constexpr uint64_t ReadDcpAddress(std::span<const uint8_t> p) noexcept {
    const uint64_t lo = ::ASFW::FW::ReadBE32(p.data());
    const uint64_t hi = ::ASFW::FW::ReadBE32(p.data() + 4);
    return lo | (hi << 32);
}

[[nodiscard]] inline std::expected<DcpDescriptor, DcpCodecError>
ParseDcpDescriptor(std::span<const uint8_t> raw) noexcept {
    if (raw.size() < kDcpDescriptorSize) return std::unexpected(DcpCodecError::kTooShort);
    if (ReadDcpAddress(raw.subspan(0)) != kDcpMagic)
        return std::unexpected(DcpCodecError::kBadMagic);
    DcpDescriptor d;
    d.notificationAddress = ReadDcpAddress(raw.subspan(8));
    d.version             = ::ASFW::FW::ReadBE32(raw.data() + 0x14);
    d.maxRequestSize      = ::ASFW::FW::ReadBE32(raw.data() + 0x18);
    d.responseAreaSize    = ::ASFW::FW::ReadBE32(raw.data() + 0x1C);
    d.writeAddress        = ReadDcpAddress(raw.subspan(0x20));
    d.readAddress         = ReadDcpAddress(raw.subspan(0x28));
    return d;
}

// ============================================================================
// Message framing (spro40d3.rs send_message; 16-byte transport header)
// ============================================================================
//
// Request/response frames share one layout:
//   +0x00  4   command opcode
//   +0x04  2   command counter, big-endian u16 (wraps; echoed in the response)
//   +0x06  2   payload length = total frame length - 16, big-endian u16
//   +0x08  8   reserved (zero)
//   +0x10  ..  payload
//
// The notification the device writes to the registered host address carries
// the response payload length at frame[6..8] (BE16) and the response status
// at frame[8..12] (BE32) (spro40d3.rs resp.connect_requested).

inline constexpr size_t kMessageHeaderSize = 16;

struct Opcode {
    std::array<uint8_t, 4> bytes;
    constexpr uint8_t operator[](size_t i) const { return bytes[i]; }
    [[nodiscard]] constexpr bool operator==(const Opcode& o) const noexcept {
        return bytes == o.bytes;
    }
};

inline constexpr Opcode kOpcodeSetVolume     {0x80, 0x00, 0x20, 0x02};
inline constexpr Opcode kOpcodeGetMeter      {0x80, 0x00, 0x10, 0x01};
inline constexpr Opcode kOpcodeGetControl    {0x80, 0x80, 0x00, 0x00};
inline constexpr Opcode kOpcodeSetControl    {0x80, 0x80, 0x00, 0x01};
inline constexpr Opcode kOpcodeWriteSwNotice {0x80, 0x80, 0x00, 0x02};
inline constexpr Opcode kOpcodeSetRouting    {0x80, 0x00, 0x30, 0x02};

/// Counter-reset ritual frame: 16 bytes, first byte 0x80, rest zero; sent when
/// a response reports rstatus != 0, after which the counter restarts at 0 and
/// the failed command is retried once (spro40d3.rs send_command).
inline constexpr size_t kCounterResetFrameSize = 16;

/// Writes the 16-byte header; `payloadLength` counts bytes AFTER the header.
inline void WriteMessageHeader(std::span<uint8_t> frame, Opcode opcode,
                               uint16_t counter, uint16_t payloadLength) noexcept {
    for (size_t i = 0; i < 4; ++i) frame[i] = opcode[i];
    frame[4] = static_cast<uint8_t>(counter >> 8);
    frame[5] = static_cast<uint8_t>(counter);
    frame[6] = static_cast<uint8_t>(payloadLength >> 8);
    frame[7] = static_cast<uint8_t>(payloadLength);
    for (size_t i = 8; i < kMessageHeaderSize; ++i) frame[i] = 0;
}

/// Decoded notification (device → registered host address block write).
struct DcpNotification {
    uint16_t responsePayloadLength{}; ///< frame[6..8]
    uint32_t responseStatus{};        ///< frame[8..12]
};

[[nodiscard]] inline std::expected<DcpNotification, DcpCodecError>
ParseNotification(std::span<const uint8_t> frame) noexcept {
    if (frame.size() < 12) return std::unexpected(DcpCodecError::kTooShort);
    return DcpNotification{
        .responsePayloadLength = static_cast<uint16_t>((frame[6] << 8) | frame[7]),
        .responseStatus        = ::ASFW::FW::ReadBE32(frame.data() + 8),
    };
}

/// Decoded response header (first 16 bytes of a paired response-area read).
struct DcpResponseHeader {
    Opcode opcode{};
    uint16_t counter{};
    uint16_t payloadLength{};
};

[[nodiscard]] inline std::expected<DcpResponseHeader, DcpCodecError>
ParseResponseHeader(std::span<const uint8_t> frame) noexcept {
    if (frame.size() < kMessageHeaderSize) return std::unexpected(DcpCodecError::kTooShort);
    DcpResponseHeader h;
    for (size_t i = 0; i < 4; ++i) h.opcode.bytes[i] = frame[i];
    h.counter       = static_cast<uint16_t>((frame[4] << 8) | frame[5]);
    h.payloadLength = static_cast<uint16_t>((frame[6] << 8) | frame[7]);
    return h;
}

// ============================================================================
// Command payloads (spro40d3.rs serializers; byte offsets cited per function)
// ============================================================================

inline constexpr size_t kSetVolumeFrameSize = 20;

/// Mixer crosspoint volume: channel (0..17) into mix bus (0..15).
/// spro40d3.rs serialize_set_volume: vol BE16 at [16..18], channel [18], mix [19].
inline void SerializeSetVolume(std::span<uint8_t, kSetVolumeFrameSize> frame,
                               uint8_t channel, uint8_t mix, int32_t volume) noexcept {
    std::ranges::fill(frame, uint8_t{0});
    for (size_t i = 0; i < 4; ++i) frame[i] = kOpcodeSetVolume[i];
    frame[16] = static_cast<uint8_t>(volume >> 8);
    frame[17] = static_cast<uint8_t>(volume);
    frame[18] = channel;
    frame[19] = mix;
}

inline constexpr size_t kGetMeterFrameSize = 24;

/// spro40d3.rs serialize_get_meter: count at [17], position at [19].
inline void SerializeGetMeter(std::span<uint8_t, kGetMeterFrameSize> frame,
                              uint8_t position, uint8_t count) noexcept {
    std::ranges::fill(frame, uint8_t{0});
    for (size_t i = 0; i < 4; ++i) frame[i] = kOpcodeGetMeter[i];
    frame[17] = count;
    frame[19] = position;
}

/// Meter values are BE i32 at response payload offset 16 + 4*i
/// (spro40d3.rs deserialize_meter). `frame` is the whole response frame.
inline void DeserializeMeters(std::span<const uint8_t> frame,
                              std::span<int32_t> outMeters) noexcept {
    for (size_t i = 0; i < outMeters.size(); ++i) {
        const size_t pos = kMessageHeaderSize + i * 4;
        outMeters[i] = static_cast<int32_t>(::ASFW::FW::ReadBE32(frame.data() + pos));
    }
}

/// Meter positions depend on the current rate mode (spro40d3.rs
/// get_master_meter / get_mixer_meter: >48000 uses the high-rate positions).
inline constexpr uint8_t kMasterMeterPosLowRate  = 0x3a;
inline constexpr uint8_t kMasterMeterPosHighRate = 0x32;
inline constexpr uint8_t kMixerMeterPosLowRate   = 0x28;
inline constexpr uint8_t kMixerMeterPosHighRate  = 0x20;

inline constexpr size_t kGetControlFrameSize = 24;

/// spro40d3.rs serialize_get_control: pos/len BE32 at [16..24].
inline void SerializeGetControl(std::span<uint8_t, kGetControlFrameSize> frame,
                                uint32_t position, uint32_t length) noexcept {
    std::ranges::fill(frame, uint8_t{0});
    for (size_t i = 0; i < 4; ++i) frame[i] = kOpcodeGetControl[i];
    ::ASFW::FW::WriteBE32(frame.data() + 16, position);
    ::ASFW::FW::WriteBE32(frame.data() + 20, length);
}

/// spro40d3.rs serialize_set_control: pos/len BE32 at [16..24], value from [24].
inline void SerializeSetControl(std::span<uint8_t> frame, uint32_t position,
                                std::span<const uint8_t> value) noexcept {
    std::ranges::fill(frame, uint8_t{0});
    for (size_t i = 0; i < 4; ++i) frame[i] = kOpcodeSetControl[i];
    ::ASFW::FW::WriteBE32(frame.data() + 16, position);
    ::ASFW::FW::WriteBE32(frame.data() + 20, static_cast<uint32_t>(value.size()));
    std::ranges::copy(value, frame.data() + 24);
}

inline constexpr size_t kSwNoticeFrameSize = 20;

/// spro40d3.rs serialize_write_sw_notice: notice BE32 at [16..20]. Sent after a
/// successful set_control so the device re-reads its software-visible state.
inline void SerializeWriteSwNotice(std::span<uint8_t, kSwNoticeFrameSize> frame,
                                   uint32_t notice) noexcept {
    std::ranges::fill(frame, uint8_t{0});
    for (size_t i = 0; i < 4; ++i) frame[i] = kOpcodeWriteSwNotice[i];
    ::ASFW::FW::WriteBE32(frame.data() + 16, notice);
}

/// Software-notice values used by the reference (spro40d3.rs set_control_u32
/// call sites): which control group a set touched.
namespace sw_notice {
inline constexpr uint32_t kOutputVolume = 1; ///< set_output_vol
inline constexpr uint32_t kMuteDim      = 2; ///< set_mute / set_dim
inline constexpr uint32_t kMonitorPad   = 3; ///< set_active_monitor_pad / dim-mute hwctl
inline constexpr uint32_t kAdatAsSpdif  = 4; ///< set_use_adat_as_spdif
} // namespace sw_notice

// ============================================================================
// Control-store offsets (spro40d3.rs get_control_u32/set_control_u32 sites)
// ============================================================================

namespace control_offset {
inline constexpr uint32_t kMute          = 0x0c; ///< get_mute / set_mute
inline constexpr uint32_t kDim           = 0x10; ///< get_dim / set_dim
inline constexpr uint32_t kOutputVolBase = 0x14; ///< 0x0c+0x08, 5 u32s, 2 ch each (NOTE: inverted)
inline constexpr uint32_t kVolHwctlBase  = 0x28; ///< 0x0c+0x1c, 5 u32s of hwctl/mute bit pairs
inline constexpr uint32_t kHwAssign      = 0x3c; ///< dim/mute hardware-knob assignments
inline constexpr uint32_t kMonitorPad    = 0x40; ///< active monitor pad
inline constexpr uint32_t kMonitorVolume = 0x54; ///< hw knob value
inline constexpr uint32_t kAdatAsSpdif   = 0x5c; ///< ADAT carries S/PDIF
} // namespace control_offset

/// Output volumes are stored as one i8 per channel, two per u32, and INVERTED
/// against the host value (spro40d3.rs cache_output_group "NOTE: inverted":
/// device = VOL_MAX - host; VOL_MAX = 0xff for the ALSA 0..255 mute..0dB scale).
inline constexpr int32_t kOutputVolumeMax = 0xff;

[[nodiscard]] constexpr uint32_t PackOutputVolumePair(int32_t even, int32_t odd) noexcept {
    const uint32_t e = static_cast<uint32_t>(kOutputVolumeMax - even) & 0xff;
    const uint32_t o = static_cast<uint32_t>(kOutputVolumeMax - odd) & 0xff;
    return e | (o << 8);
}

constexpr void UnpackOutputVolumePair(uint32_t raw, int32_t& even, int32_t& odd) noexcept {
    even = kOutputVolumeMax - static_cast<int32_t>(raw & 0xff);
    odd  = kOutputVolumeMax - static_cast<int32_t>((raw >> 8) & 0xff);
}

// ============================================================================
// Routing (spro40d3.rs serialize_route / serialize_routing_{low,high}_rate)
// ============================================================================
//
// One route entry is a BE u32: (sourceId << 12) | destinationId.
// Source ids are symbolic "from" numbers on the host side; destination ids are
// raw device ids. The "from" number maps to a device source id by class:

inline constexpr uint32_t kSrcNone   = 0x000; ///< unconnected
inline constexpr uint32_t kSrcAnalog = 0x080; ///< analog inputs 1-8
inline constexpr uint32_t kSrcSpdif  = 0x180; ///< S/PDIF 1-2
inline constexpr uint32_t kSrcAdat   = 0x200; ///< ADAT 1-8 (1-4 at high rate)
inline constexpr uint32_t kSrcMixer  = 0x300; ///< mix bus outputs 1-16
inline constexpr uint32_t kSrcStream = 0x400; ///< host stream channels 1-20

inline constexpr uint32_t kDstAnalog = 0x80;  ///< analog outs (inverted pair order, below)
inline constexpr uint32_t kDstSpdif  = 0x180;
inline constexpr uint32_t kDstAdat   = 0x200;
inline constexpr uint32_t kDstMixer  = 0x300; ///< mixer inputs 1-18
inline constexpr uint32_t kDstStream = 0x400; ///< host stream channels

/// Host-side symbolic source numbers (spro40d3.rs serialize_route):
///   0 = none, 1-8 = analog ins, 9-10 = S/PDIF, 11-18 = ADAT,
///   19-38 = stream, 39+ = mixer outputs.
[[nodiscard]] constexpr uint32_t RouteSourceId(uint32_t from) noexcept {
    if (from < 1)  return kSrcNone;
    if (from < 9)  return kSrcAnalog + from - 1;
    if (from < 11) return kSrcSpdif + from - 9;
    if (from < 19) return kSrcAdat + from - 11;
    if (from < 39) return kSrcStream + from - 19;
    return kSrcMixer + from - 39;
}

[[nodiscard]] constexpr uint32_t EncodeRouteEntry(uint32_t from, uint32_t to) noexcept {
    return (RouteSourceId(from) << 12) | to;
}

/// Router model sizes (spro40d3.rs MIX_COUNT/OUTPUT_COUNT/CHANNEL_COUNT).
inline constexpr size_t kMixCount          = 16; ///< mix buses
inline constexpr size_t kOutputCount       = 22; ///< router output-source slots
inline constexpr size_t kChannelCount      = 18; ///< mixer input channels
inline constexpr size_t kMasterMeterCount  = 2;

using RouterOutSrc   = std::array<uint32_t, kOutputCount>;  ///< symbolic "from" per phys out
using RouterMixerSrc = std::array<uint32_t, kChannelCount>; ///< symbolic "from" per mix input
using RouterMeterSrc = std::array<uint32_t, kMasterMeterCount>;

/// Largest valid symbolic source number: mixer output 16 (kMixCount,
/// spro40d3.rs:29). The reference's serialize_route (spro40d3.rs:92-107)
/// places NO upper bound on its mixer branch: a `from` past the 16 mix
/// buses yields a source id that overflows the route entry's 12-bit source
/// field ((source << 12) | to, spro40d3.rs:106) - the excess bits wrap the
/// u32 and contaminate the destination field. The engine therefore rejects
/// any source outside [0, kMaxSymbolicRouteSource] BEFORE serialization
/// (review round 3, finding B). The codec itself stays faithful to the
/// reference; the gate lives at the engine boundary.
inline constexpr uint32_t kMaxSymbolicRouteSource = 39 + kMixCount - 1; // 54

/// Valid routing source domain: 0 (none), 1-8 analog, 9-10 S/PDIF,
/// 11-18 ADAT, 19-38 stream, 39-54 mixer outputs.
[[nodiscard]] constexpr bool IsValidRouteSource(uint32_t from) noexcept {
    return from <= kMaxSymbolicRouteSource;
}

/// Frame sizes (spro40d3.rs set_routing_{low,high}_rate).
inline constexpr size_t kRoutingLowRateFrameSize  = 316;
inline constexpr size_t kRoutingHighRateFrameSize = 284;

/// Physical output-pair order for indexing the routing arrays:
/// pair 0 = Monitor 1/2, 1 = Line 3/4, 2 = Line 5/6, 3 = Line 7/8 (HP1),
/// pair 4 = Line 9/10 (HP2).
inline constexpr size_t kOutputPairCount = 5;
inline constexpr size_t kHp1PairIndex = 3;
inline constexpr size_t kHp2PairIndex = 4;

/// Analog-out destination ids are written in INVERTED pair order
/// (spro40d3.rs serialize_routing_*: ANALOG + 8 - i*2 / + 9 - i*2): pair i
/// lands on destination ids (0x88 - 2i, 0x89 - 2i), so Mon 1/2 = 0x88/0x89 and
/// HP2 (Line 9/10) = 0x80/0x81.
[[nodiscard]] constexpr uint32_t AnalogOutDestinationForChannel(size_t pair, bool odd) noexcept {
    return kDstAnalog + 8 - pair * 2 + (odd ? 1 : 0);
}

/// Low-rate routing frame (44.1/48k): 18 fixed stream routes, 10 analog-out
/// routes (inverted pairs), 2 S/PDIF, 8 ADAT, 2 loopback, 18 mixer-input
/// routes, 2 master-meter routes. Byte offsets per spro40d3.rs
/// serialize_routing_low_rate; flags quadlet at payload [0..4] = 0.
void SerializeRoutingLowRate(std::span<uint8_t, kRoutingLowRateFrameSize> frame,
                             const RouterOutSrc& outSrc,
                             const RouterMixerSrc& mixerSrc,
                             const RouterMeterSrc& meterSrc) noexcept;

/// High-rate routing frame (88.2/96k): same layout shortened for 4 ADAT
/// channels; frame[17] = 1 marks the high-rate table. Byte offsets per
/// spro40d3.rs serialize_routing_high_rate.
void SerializeRoutingHighRate(std::span<uint8_t, kRoutingHighRateFrameSize> frame,
                              const RouterOutSrc& outSrc,
                              const RouterMixerSrc& mixerSrc,
                              const RouterMeterSrc& meterSrc) noexcept;

// ============================================================================
// State provenance (Stage 1 brief: Observed / Cached / Unknown)
// ============================================================================
//
// Family-local projection that maps onto ASFW::AudioModel::StateOrigin at
// integration time (Observed→Observed, Cached→Cached, Unknown→no value).
// A bus reset or lost ownership invalidates every Cached value: the hardware
// never confirmed it, and the device may have reverted to flash defaults.

enum class ValueProvenance : uint8_t {
    Unknown,  ///< never read from the device, or invalidated (bus reset)
    Observed, ///< read back from the device
    Cached,   ///< written by the host, never read back
};

template <typename T>
struct ProvenancedValue {
    T value{};
    ValueProvenance provenance{ValueProvenance::Unknown};
    [[nodiscard]] bool IsKnown() const noexcept {
        return provenance != ValueProvenance::Unknown;
    }
};

/// Control-plane state mirror. Meters are deliberately excluded (transient).
struct SPro40D3ControlState {
    /// Mixer crosspoint volumes [channel 0..17][mix 0..15] (set_volume command).
    std::array<std::array<ProvenancedValue<int32_t>, kMixCount>, kChannelCount> mixerVolumes{};
    /// Output pair volumes, 5 stereo pairs (control store, inverted packing).
    std::array<ProvenancedValue<int32_t>, kOutputPairCount * 2> outputVolumes{};
    ProvenancedValue<uint32_t> mute{};
    ProvenancedValue<uint32_t> dim{};
    ProvenancedValue<uint32_t> monitorPad{};
    ProvenancedValue<uint32_t> adatAsSpdif{};
    ProvenancedValue<uint32_t> monitorVolume{};
    /// Router source selections (host-side symbolic "from" numbers).
    std::array<ProvenancedValue<uint32_t>, kOutputCount>  routerOutSrc{};
    std::array<ProvenancedValue<uint32_t>, kChannelCount> routerMixerSrc{};
    std::array<ProvenancedValue<uint32_t>, kMasterMeterCount> routerMeterSrc{};

    void InvalidateAll() noexcept { *this = SPro40D3ControlState{}; }
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
