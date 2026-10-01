// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "BeBoB/BeBoBProtocol.hpp"

namespace ASFW::Audio {

// Standard plug-0 AV/C/CMP lifecycle, shared with BeBoB and Oxford.
// Cross-validated with Linux sound/firewire/oxfw/oxfw-stream.c:105-120,188
// and sound/firewire/bebob/bebob_stream.c. No vendor mixer commands are issued.
class GenericAvcProtocol final : public BeBoB::BeBoBProtocol {
public:
    using BeBoBProtocol::BeBoBProtocol;
    const char* GetName() const override { return "Generic AV/C Audio"; }
    void AdoptDiscoveredGeometry(const AudioStreamRuntimeCaps& caps) noexcept override { caps_ = caps; }
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& caps) const override {
        if (caps_.sampleRateHz == 0 || caps_.hostInputPcmChannels == 0 ||
            caps_.hostOutputPcmChannels == 0) return false;
        caps = caps_;
        return true;
    }
protected:
    const char* DeviceName() const override { return GetName(); }
    AudioStreamRuntimeCaps DeviceCaps() const override { return caps_; }
    // Start at the observed rate. A different rate requires fresh geometry.
    std::vector<uint32_t> SupportedRates() const override { return {caps_.sampleRateHz}; }
private:
    AudioStreamRuntimeCaps caps_{};
};

} // namespace ASFW::Audio
