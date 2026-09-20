// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexStreamProfile.hpp - Resolved stream geometry and host-start recipe for duplex audio

#pragma once

#include "../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../../Discovery/DiscoveryTypes.hpp"
#include "../DeviceProtocolChoice.hpp"
#include "../../Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../../Wire/AMDTP/AmdtpTypes.hpp"
#include "../../Wire/AMDTP/PcmSlotMap.hpp"
#include "../../Wire/MOTU/MotuPortLayout.hpp"
#include "../../DriverKit/Config/MOTU/MotuV2Profile.hpp"
#include "../../Engine/Direct/Rx/RxCaptureChannelMap.hpp"
#include "../AudioTypes.hpp"
#include "../IDeviceProtocol.hpp"

#include <array>
#include <cstdint>

namespace ASFW::Audio::Backends {

enum class DuplexHostDirection : uint8_t {
    kReceive,
    kTransmit,
};

// The device enable happens before this recipe's startOrder. DICE requires all
// host contexts to be prepared while GLOBAL_ENABLE is clear, then starts IR
// before IT after a short post-enable settling interval.
struct DuplexStartOrderRecipe {
    // DICE programs device RX/TX before starting the corresponding host context.
    // FW-72 can select the inverse interleave for a protocol that requires it.
    bool startReceiveBeforeDeviceRx{false};
    bool startTransmitBeforeDeviceTx{false};
    // DICE reports an explicit GLOBAL clock-lock state before its stream
    // enable. BeBoB with an internal clock has no equivalent pre-CMP state;
    // validate its PCR leases and packet liveness after host RX/TX starts.
    bool requiresPreStreamClockLock{true};
    std::array<DuplexHostDirection, 2> prepareOrder{
        DuplexHostDirection::kReceive,
        DuplexHostDirection::kTransmit,
    };
    std::array<DuplexHostDirection, 2> startOrder{
        DuplexHostDirection::kReceive,
        DuplexHostDirection::kTransmit,
    };
    uint32_t postDeviceEnableDelayMs{2};
};

struct DuplexStopOrderRecipe {
    // Preserve AV/C's PCR/host-context interleave. DICE leaves this false and
    // retains its existing StopAll -> StopDuplex sequence.
    bool disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive{false};
};

// Host receive geometry for one device-to-host stream. `pcmChannels == 0`
// preserves the legacy single-stream full-width receive path.
struct DuplexCaptureStreamGeometry {
    uint8_t isoChannel{AudioStreamWireInfo::kInvalidIsoChannel};
    uint32_t pcmChannelOffset{0};
    uint32_t pcmChannels{0};
    uint32_t am824Slots{0};
    uint32_t bandwidthUnits{0};
    uint64_t allowedIsoChannels{0};
};

// Retained even though the current host IT seam derives its own packet geometry:
// the resolver remains the one place that owns both directions' stream facts.
struct DuplexPlaybackStreamGeometry {
    uint8_t isoChannel{AudioStreamWireInfo::kInvalidIsoChannel};
    uint32_t pcmChannels{0};
    uint32_t am824Slots{0};
    uint32_t bandwidthUnits{0};
    uint64_t allowedIsoChannels{0};
};

struct DuplexStreamProfile {
    AudioDuplexChannels channels{};
    AudioStreamRuntimeCaps runtimeCaps{};
    std::array<DuplexCaptureStreamGeometry, kMaxAudioStreamsPerDirection> captureStreams{};
    std::array<DuplexPlaybackStreamGeometry, kMaxAudioStreamsPerDirection> playbackStreams{};
    Encoding::AudioWireFormat captureWireFormat{Encoding::AudioWireFormat::kAM824};
    Encoding::AudioWireFormat playbackWireFormat{Encoding::AudioWireFormat::kAM824};
    // Loud OXFW units stamp an unreliable dbs in device->host packets; when set,
    // the RX decode takes its stride from the configured AM824 slot count instead
    // of the CIP header (Linux snd-oxfw SND_OXFW_QUIRK_WRONG_DBS semantics).
    bool captureTrustConfiguredStride{false};

    // MOTU only: PCM chunks per data block, per direction. Its samples are 3-byte chunks
    // rather than quadlet slots, so the am824Slots geometry above does not describe them
    // and the receive path needs this instead. Zero for every other family.
    uint32_t captureMotuPcmChunks{0};
    uint32_t playbackMotuPcmChunks{0};
    // MOTU only: which chunk each host input channel reads. The playback map rides the
    // audio driver's TxStreamPolicy instead, since that side encodes playback.
    Encoding::Motu::MotuPortMap captureMotuPorts{};
    DuplexStartOrderRecipe startOrder{};
    DuplexStopOrderRecipe stopOrder{};
    AudioEngine::Direct::Rx::RxCaptureChannelMap captureChannelMap{};
    Wire::PcmSlotMap playbackChannelMap{};
};

// The coordinator deliberately delegates all device identity checks and stream
// geometry policy to this resolver. It owns fixed-vs-dynamic channel policy,
// device quirks, AM824 geometry, resource costs, and host-start ordering as
// immutable profile data.
class DuplexStreamProfileResolver final {
  public:
    [[nodiscard]] static DuplexStreamProfile Resolve(const Discovery::DeviceRecord& record,
                                                     const IDeviceProtocol* protocol) noexcept {
        AudioStreamRuntimeCaps caps{};
        const bool haveCaps = protocol != nullptr && protocol->GetRuntimeAudioStreamCaps(caps);
        return Resolve(record, haveCaps ? caps : AudioStreamRuntimeCaps{});
    }

    [[nodiscard]] static DuplexStreamProfile Resolve(const Discovery::DeviceRecord& record,
                                                     const AudioStreamRuntimeCaps& caps) noexcept {
        return Build(record, caps, ResolveChannels(record, caps));
    }

    // Device prepare can refresh stream caps after channels have already been
    // assigned and written into the pending restart session. Retain that channel
    // assignment while resolving the refreshed wire geometry.
    [[nodiscard]] static DuplexStreamProfile
    Resolve(const Discovery::DeviceRecord& record, const AudioStreamRuntimeCaps& caps,
            const AudioDuplexChannels& assignedChannels) noexcept {
        return Build(record, caps, assignedChannels);
    }

  private:
    static constexpr uint64_t kAllIsoChannels = ~uint64_t{0};
    static constexpr uint8_t kDefaultCaptureIsoChannel = 1;
    static constexpr uint8_t kDefaultPlaybackIsoChannel = 0;

    // Linux sound/firewire/iso-resources.c:48-76 calculates bandwidth from the
    // maximum CIP payload plus the three-quadlet isoch packet header, scaled to
    // S400 allocation units. Its 512-unit fallback is used when optimized gap
    // count information is unavailable; DeviceRecord currently carries link
    // speed but not the live gap count, so use that conservative reference path.
    [[nodiscard]] static constexpr uint32_t
    PacketBandwidthUnits(uint32_t maxPayloadBytes, FW::FwSpeed speed) noexcept {
        const uint32_t alignedPayloadBytes = (maxPayloadBytes + 3U) & ~3U;
        const uint32_t packetBytesAtSpeed = 12U + alignedPayloadBytes;

        uint32_t packetUnits = packetBytesAtSpeed;
        switch (speed) {
            case FW::FwSpeed::S100:
                packetUnits *= 4U;
                break;
            case FW::FwSpeed::S200:
                packetUnits *= 2U;
                break;
            case FW::FwSpeed::S400:
                break;
            case FW::FwSpeed::S800:
                packetUnits = (packetUnits + 1U) / 2U;
                break;
        }
        return packetUnits + 512U;
    }

    [[nodiscard]] static constexpr uint32_t
    AmdtpBandwidthUnits(uint32_t am824Slots, uint32_t sampleRateHz,
                        FW::FwSpeed speed) noexcept {
        const auto rate = Encoding::AmdtpRateGeometryForSampleRate(
            sampleRateHz != 0 ? sampleRateHz : 48000U);
        const uint32_t blocksPerPacket = rate ? rate->sytIntervalFrames : 8U;
        const uint32_t slots = am824Slots != 0 ? am824Slots : 1U;
        const uint32_t maxPayloadBytes = 8U + blocksPerPacket * slots * 4U;
        return PacketBandwidthUnits(maxPayloadBytes, speed);
    }

    [[nodiscard]] static constexpr uint64_t FixedChannelMask(uint8_t channel) noexcept {
        return IsValidIsoChannel(channel) ? (uint64_t{1} << channel) : 0;
    }

    [[nodiscard]] static constexpr bool IsValidIsoChannel(uint8_t channel) noexcept {
        return channel <= 0x3F;
    }

    [[nodiscard]] static constexpr uint32_t ClampStreamCount(uint32_t count) noexcept {
        if (count == 0) {
            return 1;
        }
        return count > kMaxAudioStreamsPerDirection ? kMaxAudioStreamsPerDirection : count;
    }

    /// Everything below used to be nine hand-written identity predicates
    /// sitting next to the geometry they gate. They are catalog rows now: the
    /// facts are the same, but they live with the rest of what is known about
    /// the device instead of being rediscovered here.
    [[nodiscard]] static DeviceProfiles::Audio::DeviceStreamTraits
    TraitsFor(const Discovery::DeviceRecord& record) noexcept {
        return DeviceProfiles::Audio::AudioDeviceCatalog::StreamTraitsFor(record.identity);
    }

    [[nodiscard]] static AudioDuplexChannels
    ResolveChannels(const Discovery::DeviceRecord& record,
                    const AudioStreamRuntimeCaps& caps) noexcept {
        AudioDuplexChannels channels{
            .deviceToHostIsoChannel = kDefaultCaptureIsoChannel,
            .hostToDeviceIsoChannel = kDefaultPlaybackIsoChannel,
        };

        channels.captureStreamCount = ClampStreamCount(caps.deviceToHostStreamCount);
        channels.playbackStreamCount = ClampStreamCount(caps.hostToDeviceStreamCount);
#if 0
        // DISABLED 2026-09-20 — clamps the WRONG DIRECTION. Kept, not deleted,
        // because the hazard it was written for may well be real; only our
        // transcription of it was wrong. TODO below says what would settle it.
        //
        // WHY IT EXISTS. libffado forces its playback stream count to 1 for
        // Alesis model 0x000000/0x000001 and Focusrite Saffire PRO 26
        // (dice_avdevice.cpp:1686-1700):
        //
        //     /* special case for Alesis io14, which announces two receive
        //      * transmitters, but only has one. Same is true for Alesis
        //      * Multimix16 and Focusrite Saffire PRO 26. */
        //     if (FW_VENDORID_ALESIS == ...) { case 0x1: case 0x0: m_nb_rx = 1; }
        //
        // The guarded hazard is real in principle: arming an iso channel and
        // reserving bandwidth for a stream the device will never consume.
        //
        // WHY IT IS DISABLED. Two reasons, in increasing order of weight.
        //
        // 1. It is applied to the wrong direction. libffado clamps m_nb_rx and
        //    never touches m_nb_tx, and dice_avdevice.cpp:1057-1062 says which
        //    is which:
        //
        //        for (i=0; i<m_nb_tx; i++) prepareSP(i, Port::E_Capture);
        //        for (i=0; i<m_nb_rx; i++) prepareSP(i, Port::E_Playback);
        //
        //    So m_nb_rx is host PLAYBACK. captureStreamCount here comes from
        //    caps.deviceToHostStreamCount, which DICETcatProtocol fills from the
        //    device's DICE TX section — host CAPTURE. This clamps the one
        //    direction libffado deliberately leaves alone.
        //
        //    The trap is that "rx" names opposite things in the two codebases:
        //    libffado follows the device (rx = what the device receives =
        //    playback), ASFW's profile fields follow the host (Rx* = capture).
        //
        // 2. The vendor's own driver clamps NOTHING. AlesisFirewire.kext's
        //    PopulateDeviceStruct (0xd920) loops the full TX_NUMBER and the full
        //    RX_NUMBER with no bound in either direction — not even the sanity
        //    rejects MidasFW and PaeFireStudio carry (TX >= 3 / RX > 4 → error).
        //    Alesis ships no clamp for Alesis hardware.
        //
        // WHAT IT COST. Two things, the second worse than the first.
        //
        // The recorded MultiMix dump
        // (documentation/fixtures/alesismultimix.txt) reports DICE TX NUMBER = 2:
        // 12 PCM (MIC_LINE_1..4, LINE_5..12) plus 2 PCM (MAIN_IN L/R) = 14
        // capture channels. This clamp armed only stream 0, silently dropping
        // MAIN_IN L/R — 12 channels where the device carries 14.
        //
        // And because Build() below selects per-stream geometry only when
        // captureStreamCount > 1 (`multiCapture`, :346), forcing the count to 1
        // ALSO made stream 0 report caps.deviceToHostAm824Slots — the device's
        // AGGREGATE slot count — instead of its own. On a 2x16+1 device that is
        // 34 slots attributed to a stream physically carrying 17, and since
        // bandwidthUnits is computed from am824Slots, the IRM reservation was
        // sized from the aggregate as well.
        //
        // Meanwhile that unit's DICE RX NUMBER is already 1, so libffado's
        // actual clamp would have been a no-op on it. We paid two real inputs
        // and a mis-sized reservation, and bought none of the protection the
        // workaround was for.
        //
        // It had also become a second authority on a fact the profile already
        // owns: AlesisMultiMixProfile declares capture geometry a SEED (device
        // wins) while asserting its single PLAYBACK stream on libffado's
        // authority. With this enabled the two disagreed, and they are consumed
        // by different paths — this one feeds iso allocation and bandwidth, the
        // profile feeds publication — so a MultiMix could publish 14 channels
        // while one capture stream was armed.
        //
        // WHAT HARDWARE ALREADY SAYS. A contributor dumped a MultiMix
        // (documentation/fixtures/alesismultimix.txt): DICE TX NUMBER = 2
        // (12 + 2), DICE RX NUMBER = 1 (2 PCM). **That unit does not
        // over-report playback at all** — libffado's clamp would be a no-op on
        // it, and ours actively removed a capture stream it really has.
        //
        // TODO(FW-DICE-ALESIS): one variant is still untested, and it is
        // precisely the accused one. libffado names the MultiMix **16**:
        // "Same is true for Alesis Multimix16 and Focusrite Saffire PRO 26."
        // The dump we hold is a 12-input unit (MIC_LINE_1..4, LINE_5..12, plus
        // MAIN_IN L/R), and all of 8/12/16 publish vendor 0x000595 model
        // 0x000000, so one dump cannot speak for the range.
        //
        //   a) Get a MultiMix **16** dump — DICE TX_NUMBER, RX_NUMBER and each
        //      stream's NUMBER_AUDIO. The Saffire PRO 26 would corroborate.
        //   b) If it reports RX_NUMBER > 1 with only one real playback stream,
        //      re-enable against PLAYBACK — channels.playbackStreamCount —
        //      never capture, and scope it so the 12 is unaffected.
        //   c) Prefer libffado's own suggestion over a per-model rule. The
        //      FIXME immediately above its clamp proposes the general form:
        //      "Maybe check the number of channels and ignore receivers with
        //      zero channels?" That needs no vendor/model table and has no
        //      direction to get wrong. No recorded device shows a zero-channel
        //      stream yet, which is why it is not implemented on spec alone.
        //
        // If the 16 also reports RX_NUMBER = 1, delete this block and the trait
        // outright: libffado's workaround would then have no basis on any
        // hardware we have seen, and Alesis's own driver already clamps nothing.
        //
        // The trait field and the two catalog rows that set it are deliberately
        // left in place, so re-enabling is this block plus a direction fix
        // rather than an archaeology exercise. See
        // documentation/DICE_TCAT_ARCHITECTURE.md §2.9 and §3.3.
        if (TraitsFor(record).clampCaptureStreamsToOne) {
            channels.captureStreamCount = 1;
        }
#endif

        // Stream zero retains the legacy scalar channel. Remaining streams are
        // assigned the lowest channel not already used by either direction.
        uint64_t usedChannels = 0;
        const auto markUsed = [&usedChannels](uint8_t channel) noexcept {
            if (channel <= 0x3F) {
                usedChannels |= (uint64_t{1} << channel);
            }
        };
        const auto nextFree = [&usedChannels]() noexcept -> uint8_t {
            for (uint8_t channel = 0; channel <= 0x3F; ++channel) {
                if ((usedChannels & (uint64_t{1} << channel)) == 0) {
                    usedChannels |= (uint64_t{1} << channel);
                    return channel;
                }
            }
            return AudioStreamWireInfo::kInvalidIsoChannel;
        };

        channels.captureIsoChannels[0] = IsValidIsoChannel(caps.deviceToHostIsoChannel)
                                             ? caps.deviceToHostIsoChannel
                                             : channels.deviceToHostIsoChannel;
        channels.playbackIsoChannels[0] = IsValidIsoChannel(caps.hostToDeviceIsoChannel)
                                              ? caps.hostToDeviceIsoChannel
                                              : channels.hostToDeviceIsoChannel;
        markUsed(channels.captureIsoChannels[0]);
        markUsed(channels.playbackIsoChannels[0]);

        for (uint32_t i = 1; i < channels.captureStreamCount; ++i) {
            channels.captureIsoChannels[i] = nextFree();
        }
        for (uint32_t i = 1; i < channels.playbackStreamCount; ++i) {
            channels.playbackIsoChannels[i] = nextFree();
        }

        channels.deviceToHostIsoChannel = channels.captureIsoChannels[0];
        channels.hostToDeviceIsoChannel = channels.playbackIsoChannels[0];
        return channels;
    }

    [[nodiscard]] static DuplexStreamProfile Build(const Discovery::DeviceRecord& record,
                                                   const AudioStreamRuntimeCaps& caps,
                                                   const AudioDuplexChannels& channels) noexcept {
        DuplexStreamProfile profile{
            .channels = channels,
            .runtimeCaps = caps,
        };
        const auto traits = TraitsFor(record);
        const uint64_t allowedChannels =
            traits.cmpChoosesIsoChannel ? kAllIsoChannels : 0;

        // AM824 uses one data-block slot per PCM channel plus any MIDI slots;
        // the controller consumes the already-discovered DBS values unchanged.
        // Cross-validated with Linux sound/firewire/amdtp-am824.c:42-96.
        const bool multiCapture = channels.captureStreamCount > 1;
        uint32_t captureChannelOffset = 0;
        for (uint32_t i = 0; i < channels.captureStreamCount; ++i) {
            const AudioStreamWireInfo& stream = caps.deviceToHostStreams[i];
            DuplexCaptureStreamGeometry& geometry = profile.captureStreams[i];
            geometry.isoChannel = channels.CaptureChannel(i);
            geometry.pcmChannelOffset = captureChannelOffset;
            geometry.pcmChannels = multiCapture ? stream.pcmChannels : 0;
            geometry.am824Slots = multiCapture ? stream.am824Slots : caps.deviceToHostAm824Slots;
            geometry.bandwidthUnits = AmdtpBandwidthUnits(
                geometry.am824Slots, caps.sampleRateHz, record.link.localToNode);
            // CMP (including BridgeCo/BeBoB) does not own a fixed channel;
            // IRM selects one, which is then committed back to its PCR.
            geometry.allowedIsoChannels = traits.cmpChoosesIsoChannel
                                              ? allowedChannels
                                              : FixedChannelMask(geometry.isoChannel);
            captureChannelOffset += geometry.pcmChannels;
        }

        for (uint32_t i = 0; i < channels.playbackStreamCount; ++i) {
            const AudioStreamWireInfo& stream = caps.hostToDeviceStreams[i];
            DuplexPlaybackStreamGeometry& geometry = profile.playbackStreams[i];
            geometry.isoChannel = channels.PlaybackChannel(i);
            geometry.pcmChannels = stream.pcmChannels != 0
                                       ? stream.pcmChannels
                                       : (i == 0 ? caps.hostOutputPcmChannels : 0U);
            geometry.am824Slots = stream.am824Slots != 0
                                      ? stream.am824Slots
                                      : (i == 0 ? caps.hostToDeviceAm824Slots : 0U);
            geometry.bandwidthUnits = AmdtpBandwidthUnits(
                geometry.am824Slots, caps.sampleRateHz, record.link.localToNode);
            geometry.allowedIsoChannels = traits.cmpChoosesIsoChannel
                                              ? allowedChannels
                                              : FixedChannelMask(geometry.isoChannel);
        }

        if (ChooseAudioBackend(record) == AudioBackendKind::MotuRegister) {
            // MOTU is chunk-framed in both directions. The chunk counts come from the
            // device's own registers via PrepareDuplex, which MotuV2Protocol reports as
            // runtime caps -- there is no profile table to read them from, since model_id
            // is 0.
            profile.captureWireFormat = Encoding::AudioWireFormat::kMotuV2;
            profile.playbackWireFormat = Encoding::AudioWireFormat::kMotuV2;
            profile.captureMotuPcmChunks = caps.deviceToHostPcmChunks != 0
                                               ? caps.deviceToHostPcmChunks
                                               : caps.hostInputPcmChannels;
            profile.playbackMotuPcmChunks = caps.hostToDevicePcmChunks != 0
                                                ? caps.hostToDevicePcmChunks
                                                : caps.hostOutputPcmChannels;
            // The version of the unit the catalog actually matched, not the
            // flat shim, which takes the first non-zero value across every unit
            // directory and so can name a version no single unit published.
            const auto choice = ChooseDeviceProtocol(record);
            profile.captureMotuPorts = Isoch::Audio::MOTU::Profiles::CapturePortsForSwVersion(
                choice.has_value() ? choice->unitVersion
                                   : record.unitSwVersion.value_or(0U));
        }

        // Runtime-conditional on purpose: this device switches wire format with
        // its configuration, so the identity grants the permission and the
        // measured geometry decides whether it applies.
        if (traits.rawPcm24In32WhenEightInNineSlots && caps.hostInputPcmChannels == 8 &&
            caps.deviceToHostAm824Slots == 9) {
            profile.captureWireFormat = Encoding::AudioWireFormat::kRawPcm24In32;
        }
        if (traits.rawPcm24In32WhenEightInNineSlots && caps.hostOutputPcmChannels == 8 &&
            caps.hostToDeviceAm824Slots == 9) {
            profile.playbackWireFormat = Encoding::AudioWireFormat::kRawPcm24In32;
        }

        if (traits.captureTrustConfiguredStride) {
            // The capture-side CIP dbs field is untrusted and the configured
            // slot count is the authority. Loud/Mackie (snd-oxfw oxfw.c:189-196;
            // amdtp-stream.c:766-769 substitutes the configured data-block
            // size), and Fireworks, which gives dbc its own meaning, whose
            // NO-DATA packets carry tag 0, and whose firmware 4.6.0 stamps a
            // wrong dbs above 88.2 kHz.
            profile.captureTrustConfiguredStride = true;
        }

        using DeviceProfiles::Audio::StreamStartShape;
        switch (traits.startShape) {
        case StreamStartShape::ApogeeInterleaved:
            // Preserve the prior AVCAudioBackend ordering:
            // host IR -> CMP oPCR -> host IT -> CMP iPCR. The runner uses these
            // profile flags to interleave host starts with the neutral device
            // stages. CMP operations retain their adapter-owned 250 ms timeout.
            profile.startOrder.startReceiveBeforeDeviceRx = true;
            profile.startOrder.startTransmitBeforeDeviceTx = true;
            profile.startOrder.postDeviceEnableDelayMs = 0;
            profile.stopOrder
                .disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive = true;
            break;

        case StreamStartShape::CmpReceiveThenTransmit:
            // Linux's CMP choreography: reserve both resources, establish the
            // remote iPCR then oPCR, and only then start the AMDTP domain,
            // receive before transmit. Shared by three families that used to
            // state it three times over, identically:
            //
            //  - BeBoB. Keep the wire-visible ordering explicit rather than
            //    inheriting an incidental generic/DICE default.
            //    bebob_stream.c:525-590, 593-674.
            //  - The Oxford-run Onyx-i, which shares the BeBoB base's
            //    choreography and is SYT-unaware (snd-oxfw CIP_UNAWARE_SYT), so
            //    there is no pre-stream clock to lock against.
            //  - The Fireworks-run Onyx 400F (fireworks_stream.c:
            //    cmp_connection_establish for both plugs, then
            //    amdtp_domain_start), SYT-unaware with an internal clock the
            //    host cannot observe before the connection exists.
            //
            // The pre-stream lock gate must not run for any of them.
            // Field-verified on the 400F 2026-09-13: with the gate on, the
            // coordinator polled HWCTL GET_CLOCK ~50 times and failed the start
            // at GlobalClockLock.
            profile.startOrder.startReceiveBeforeDeviceRx = false;
            profile.startOrder.startTransmitBeforeDeviceTx = false;
            profile.startOrder.requiresPreStreamClockLock = false;
            profile.startOrder.startOrder = {
                DuplexHostDirection::kReceive,
                DuplexHostDirection::kTransmit,
            };
            profile.startOrder.postDeviceEnableDelayMs = 0;
            break;

        case StreamStartShape::TransmitFirst:
            // INT202/203 are output-only in CoreAudio but retain both DICE
            // directions on the wire. Start host IT first after GLOBAL_ENABLE
            // so its AM824 packets can establish the device receive-clock path;
            // source lock is inspected by the DICE adapter post-start, not used
            // as a coordinator admission gate. Linux's Weiss tables retain
            // two PCM channels for both DICE directions (dice-weiss.c:10-35).
            profile.startOrder.requiresPreStreamClockLock = false;
            profile.startOrder.startOrder = {
                DuplexHostDirection::kTransmit,
                DuplexHostDirection::kReceive,
            };
            break;

        case StreamStartShape::Default:
            break;
        }
        return profile;
    }
};

} // namespace ASFW::Audio::Backends
