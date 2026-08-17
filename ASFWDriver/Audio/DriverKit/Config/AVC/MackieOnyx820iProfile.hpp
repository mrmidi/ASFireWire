// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MackieOnyx820iProfile.hpp - ADK isoch geometry for the Mackie Onyx 820i (Oxford run).
//
// Matches the Oxford-run Onyx-i family id 0x081216, but the geometry below was captured
// from a real Onyx 820i (AV/C EXTENDED STREAM FORMAT INFORMATION, 2026-08-17): capture
// 8ch MBLA, playback 2ch MBLA, compound AM824, 44.1/48/88.2/96 kHz, one isoch plug per
// direction, no MIDI. The model id is shared across the Oxford-run Onyx-i mixers
// (820i/1220i/1620i); siblings with different channel widths must NOT reuse this static
// profile — they need a discovery-derived per-GUID profile (RegisterBeBoBProfile is the
// precedent) before enablement.
//
// Loud behavioral notes (Linux snd-oxfw, references/linux-sound-firewire-stack):
//   - blocking transmission vendor-wide (oxfw.c:189-196) — enforced here via streamMode
//     and in DeviceStreamModeQuirks;
//   - the device's capture-side CIP DBS field is untrusted (SND_OXFW_QUIRK_WRONG_DBS);
//     this profile's rx dbs (8) is the source of truth once the RX stride quirk lands;
//   - CIP_UNAWARE_SYT: no SYT-based sync (oxfw-stream.c:164-169).

#pragma once

#include "../DICE/DiceDeviceProfile.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {

// The ADK direct-audio allocation path consumes the shared isoch profile
// interface even for AV/C devices. The protocol/control path remains AV/C.
class MackieOnyx820iProfile final : public DICE::IDiceDeviceProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override;

    [[nodiscard]] bool Matches(const DICE::DiceDeviceIdentity& identity) const noexcept override;

    [[nodiscard]] DICE::DiceDeviceQuirks Quirks() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(
        DICE::DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(
        DICE::DiceStreamConfig& outConfig) const noexcept override;

    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override;

    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override;

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;
};

} // namespace ASFW::Isoch::Audio::AVC::Profiles
