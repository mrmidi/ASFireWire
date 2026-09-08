// SPDX-License-Identifier: Apache-2.0
#include "PreSonusFireStudioProjectProfile.hpp"

#include "../../../../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {
namespace {

void FillStreamConfig(DiceStreamConfig& out, DiceStreamDirection direction) noexcept {
    using Profile = PreSonusFireStudioProjectProfile;
    out = DiceStreamConfig{};
    out.direction = direction;
    out.sampleRate = Profile::kSampleRateHz;
    out.pcmChannels = Profile::kPcmChannels;
    out.midiSlots = Profile::kMidiSlots;
    out.dbs = Profile::kDbs;
    out.streamMode = Encoding::StreamMode::kBlocking;
    out.framesPerDataPacket = 8;
    out.fdf = 0x02;
    out.fmt = 0x10;
}

} // namespace

const char* PreSonusFireStudioProjectProfile::Name() const noexcept {
    return "PreSonus FireStudio Project (DICE)";
}

bool PreSonusFireStudioProjectProfile::Matches(const DiceDeviceIdentity& identity) const noexcept {
    using namespace ASFW::DeviceProfiles::Audio;
    return identity.vendorId == kPreSonusVendorId && identity.modelId == kFireStudioProjectModelId;
}

DiceDeviceQuirks PreSonusFireStudioProjectProfile::Quirks() const noexcept {
    // The maintainer's inspection of the original PreSonus KEXT reports
    // raw sign-extended 24-in-32 playback PCM and zeroed unwritten samples:
    // https://github.com/mrmidi/ASFireWire/pull/105#issuecomment-5581934008
    // Build 9 passed bounded raw-PCM playback tests with zeroed silence;
    // see captures/presonus-firestudio-project/README.md for evidence and limits.
    // These listening tests do not establish bit-perfect vendor-format parity.
    // Keep capture decoding, MIDI defaults and NO-DATA framing unchanged.
    DiceDeviceQuirks quirks{};
    quirks.tx.hostToDevicePcmEncoding = Encoding::AudioWireFormat::kRawPcm24In32;
    return quirks;
}

std::vector<uint32_t> PreSonusFireStudioProjectProfile::SupportedSampleRates() const {
    // Both rates use the captured low-rate 10 PCM + 1 MIDI geometry. Linux
    // dice-stream.c:19-30 groups 44.1/48 kHz in mode 0; the existing generic
    // blocking AM824 path derives cadence and FDF from the selected rate.
    return {kSupportedSampleRatesHz.begin(), kSupportedSampleRatesHz.end()};
}

bool PreSonusFireStudioProjectProfile::BuildDefaultTxStreamConfig(DiceStreamConfig& out) const noexcept {
    FillStreamConfig(out, DiceStreamDirection::HostToDevice);
    return true;
}

bool PreSonusFireStudioProjectProfile::BuildDefaultRxStreamConfig(DiceStreamConfig& out) const noexcept {
    FillStreamConfig(out, DiceStreamDirection::DeviceToHost);
    return true;
}

} // namespace ASFW::Isoch::Audio::DICE::Profiles
