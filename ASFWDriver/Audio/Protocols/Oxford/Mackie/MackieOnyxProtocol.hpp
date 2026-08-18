// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MackieOnyxProtocol.hpp — Runtime duplex control for the Mackie Onyx-i (Oxford run).
//
// Deliberate cross-family reuse: this Oxford/OXFW971 device subclasses the
// BeBoB protocol base, because that base is in practice a protocol-neutral
// AV/C + CMP duplex engine — FCP dispatch, INPUT/OUTPUT PLUG SIGNAL FORMAT
// (0x18/0x19) programming, CMP connect/verify/teardown, plug-0 streams, and
// no-op mixer configuration are all exactly what an OXFW unit needs. Linux
// uses the identical signal-format command for OXFW rate control
// (snd-oxfw oxfw-stream.c set_rate -> avc_general_set_sig_fmt), and both
// families stream plain AM824 over CMP-connected unit plugs. If a second
// OXFW device adopts this base, renaming the base to something family-neutral
// is the right upstream follow-up.
//
// Stream geometry is the live 820i capture (asymmetric 8-in/2-out; see
// MackieOnyx820iProfile for the matching ADK isoch geometry and the sibling
// caveat on the shared 0x081216 model id). The Loud wire quirks (blocking
// transmission, untrusted capture DBS, SYT-unaware) are applied via
// DeviceStreamModeQuirks and the DuplexStreamProfile policy, not here.

#pragma once

#include "../../BeBoB/BeBoBProtocol.hpp"

#include <vector>

namespace ASFW::Audio::Oxford::Mackie {

class MackieOnyxProtocol final : public BeBoB::BeBoBProtocol {
public:
    MackieOnyxProtocol(Protocols::Ports::FireWireBusOps& busOps,
                       Protocols::Ports::FireWireBusInfo& busInfo,
                       Discovery::DeviceRouteToken route,
                       IRM::IRMClient* irmClient,
                       CMP::CMPClient* cmpClient,
                       Scheduling::ITimerScheduler* timerScheduler) noexcept;

    const char* GetName() const override { return "Mackie Onyx-i (Oxford)"; }

protected:
    const char* DeviceName() const override { return "Mackie Onyx-i (Oxford)"; }
    [[nodiscard]] AudioStreamRuntimeCaps DeviceCaps() const override { return caps_; }
    [[nodiscard]] std::vector<uint32_t> SupportedRates() const override;
    void ReadClockHealth(HealthCallback callback) override;

private:
    AudioStreamRuntimeCaps caps_{};
};

} // namespace ASFW::Audio::Oxford::Mackie
