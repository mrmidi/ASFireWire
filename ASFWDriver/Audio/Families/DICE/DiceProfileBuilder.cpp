// SPDX-License-Identifier: Apache-2.0

#include "DiceProfileBuilder.hpp"
#include "../Common/CommonProfileBuilder.hpp"
#include "../../Shared/AudioGeometryPolicy.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Audio::Families::DICE {

std::expected<Devices::ResolvedAudioEndpointProfile, Devices::ProfileBuildError>
BuildProfile(const Devices::ProfileBuildContext& context) noexcept {
    const auto* facts = std::get_if<Devices::DiceProbeFacts>(&context.probeFacts);
    if (facts == nullptr) {
        return std::unexpected(Devices::ProfileBuildError::WrongProbeFacts);
    }
    auto result = Common::BuildBase(context, facts->streams, facts->supportedRates);
    if (!result) {
        return result;
    }
    auto& profile = *result;
    profile.streamMode = Devices::StreamModePolicy::Blocking;
    profile.captureIsoChannelPolicy = Duplex::IsoChannelPolicy::Fixed;
    profile.playbackIsoChannelPolicy = Duplex::IsoChannelPolicy::Fixed;
    profile.facets.push_back({Devices::FacetKind::Clock, 1});

    using DeviceProfiles::Audio::ProfileBuilderId;
    switch (context.staticPlan.profileBuilder) {
        case ProfileBuilderId::FocusriteSPro24Dsp:
            // Wire capture from the original Saffire.kext shows asymmetric
            // encoding: device TX/capture uses ordinary 0x40-labelled AM824,
            // while host TX/playback uses raw signed 24-bit PCM in 32-bit
            // slots. Keep the default AM824 capture format from BuildBase.
            if (facts->streams.hostOutputPcmChannels == 8 &&
                facts->streams.hostToDeviceAm824Slots == 9) {
                profile.playbackWireFormat = Encoding::AudioWireFormat::kRawPcm24In32;
            }
            profile.facets.push_back({Devices::FacetKind::Mixer, 0x53503234});
            profile.txPacketPolicy.preserveFdfInNoDataPackets = true;
            break;
        case ProfileBuilderId::WeissInt202:
        case ProfileBuilderId::WeissInt203:
            profile.startPolicy.requiresPreStreamClockLock = false;
            profile.startPolicy.startOrder = {
                Duplex::HostDirection::kTransmit,
                Duplex::HostDirection::kReceive,
            };
            break;
        // [derived, not hardware-validated] The Liquid Saffire 56 is grouped
        // with its Saffire siblings: every Focusrite DICE device we have tested
        // takes host->device PCM with no AM824 labels. If it turns out to want
        // labelled AM824, this is the line to change.
        case ProfileBuilderId::FocusriteLiquidS56:
        case ProfileBuilderId::FocusriteSPro14:
        case ProfileBuilderId::FocusriteSPro24:
        case ProfileBuilderId::AlesisMultiMix:
        case ProfileBuilderId::MidasVeniceF32:
        case ProfileBuilderId::PreSonusStudioLive1602:
            profile.playbackWireFormat = Encoding::AudioWireFormat::kRawPcm24In32;
            profile.txPacketPolicy.preserveFdfInNoDataPackets = true;
            if (context.staticPlan.profileBuilder == ProfileBuilderId::AlesisMultiMix) {
                profile.txPacketPolicy.initializeNonAudioSlots = false;
            }
            // Midas Venice F16/F24/F32: derive marketing name from measured geometry.
            // All three share the same DICE identity (product 0x001) and differ
            // only in physical channel count. Unrecognized geometry keeps the
            // catalog name.
            if (context.staticPlan.profileBuilder == ProfileBuilderId::MidasVeniceF32) {
                const char* derivedName = nullptr;
                switch (facts->streams.hostInputPcmChannels) {
                    case 16: derivedName = DeviceProfiles::Audio::kMidasVeniceF16ModelName; break;
                    case 24: derivedName = DeviceProfiles::Audio::kMidasVeniceF24ModelName; break;
                    case 32: derivedName = DeviceProfiles::Audio::kMidasVeniceF32ModelName; break;
                    default: break;
                }
                if (derivedName) {
                    profile.deviceName = derivedName;
                }
                ASFW_LOG(DICE, "DiceProfileBuilder: Midas Venice geometry %u/%u \u2192 name '%s'",
                         facts->streams.hostInputPcmChannels,
                         facts->streams.hostOutputPcmChannels,
                         profile.deviceName.c_str());
            }
            break;
        default:
            return std::unexpected(Devices::ProfileBuildError::UnsupportedBuilder);
    }
    // Carry device-reported channel names to the nub for CoreAudio.
    profile.deviceInputChannelNames = facts->inputLabels;
    profile.deviceOutputChannelNames = facts->outputLabels;
    Common::AddDefaultTiming(profile, 500);

    const bool isSaffire =
        context.staticPlan.profileBuilder == ProfileBuilderId::FocusriteSPro24Dsp ||
        context.staticPlan.profileBuilder == ProfileBuilderId::FocusriteSPro24 ||
        context.staticPlan.profileBuilder == ProfileBuilderId::FocusriteSPro14 ||
        context.staticPlan.profileBuilder == ProfileBuilderId::FocusriteLiquidS56;

    for (uint8_t i = 0; i < profile.timingCount; ++i) {
        const uint32_t rate = profile.timing[i].sampleRateHz;
        const uint32_t framesPerPacket = rate > 96000 ? 32U : (rate > 48000 ? 16U : 8U);
        const uint32_t addend = rate > 96000 ? 4U : (rate > 48000 ? 2U : 0U);
        if (isSaffire) {
            // Empirically calibrated for Focusrite Saffire family:
            // 1. Hardware Latency: 105 frames roundtrip (53 in / 52 out @ 48k).
            //    - Sourced from physical loopback RTL tests (tools/rtl/rtl_loopback -d "Saffire"),
            //      which measured invariant RTL_ts = 105.01 frames across buffer sizes (512, 128, 64).
            //    - Validated via Oblique Audio RTL Utility at 64 samples (+47 sample residual
            //      against legacy vendor declaration of 29 in / 29 out).
            //    - Confirmed by DAWBench LLP database footnote "* I/O not reporting AD/DA" in Focusrite Driver 4.0.0.
            //    - Symmetrical declaration reduces DAW residual to +0.01 frames (< 0.2 µs).
            // 2. RX Safety Offset: 10 packets (80 frames @ 48k).
            //    - Covers the 8-packet (64 frame) completion batch floor with 16 frames (2 packets / 250 µs) headroom.
            //    - Validated against Instruments.app ZTS jitter (52 ns std dev phase lock, 210 ns spread).
            //    - Saves 48 frames (1.0 ms) of roundtrip latency compared to legacy 16 packets (128 frames).
            // 3. TX Safety Offset: 6 packets nominal (clamped to 60 frames / 10 slots by payload-finality).
            //    - Verified via TX Latency Metering (E0 -> E2): min hardware wait = 2,015.3 µs (96.7 frames),
            //      leaving a 36.7 frame (765 µs) margin above the 60-frame deadline with zero substitutions.
            profile.timing[i].inputLatencyFrames =
                Shared::AudioGeometryPolicy::SaffireReportedInputLatencyFrames(rate);
            profile.timing[i].outputLatencyFrames =
                Shared::AudioGeometryPolicy::SaffireReportedOutputLatencyFrames(rate);
            profile.timing[i].inputSafetyFrames =
                Shared::AudioGeometryPolicy::SaffireRxSafetyOffsetFrames(rate);
            profile.timing[i].outputSafetyFrames =
                Shared::AudioGeometryPolicy::SaffireTxSafetyOffsetFrames(rate);
        } else {
            // Generic DICE fallback (e.g. Alesis, Midas, PreSonus)
            profile.timing[i].inputLatencyFrames =
                rate > 96000 ? 119U : (rate > 48000 ? 59U : 29U);
            profile.timing[i].outputLatencyFrames = profile.timing[i].inputLatencyFrames;
            profile.timing[i].inputSafetyFrames = (16U + addend) * framesPerPacket;
            profile.timing[i].outputSafetyFrames = (6U + addend) * framesPerPacket;
        }
    }
    return result;
}

} // namespace ASFW::Audio::Families::DICE
