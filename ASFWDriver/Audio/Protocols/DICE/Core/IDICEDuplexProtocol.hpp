// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IDICEDuplexProtocol.hpp - Typed DICE duplex restart/control surface

#pragma once

#include "DICERestartSession.hpp"

#include <DriverKit/IOReturn.h>
#include <atomic>
#include <functional>

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Audio::DICE {

class IDICEDuplexProtocol {
public:
    using PrepareCallback = std::function<void(IOReturn, DiceDuplexPrepareResult)>;
    using StageCallback = std::function<void(IOReturn, DiceDuplexStageResult)>;
    using ConfirmCallback = std::function<void(IOReturn, DiceDuplexConfirmResult)>;
    using ClockApplyCallback = std::function<void(IOReturn, DiceClockApplyResult)>;
    using HealthCallback = std::function<void(IOReturn, DiceDuplexHealthResult)>;
    using VoidCallback = std::function<void(IOReturn)>;

    virtual ~IDICEDuplexProtocol() = default;

    // Pre-read the device's static stream format (DICE TX_NUMBER/RX_NUMBER +
    // per-stream channel counts) and publish it through GetRuntimeAudioStreamCaps
    // BEFORE duplex channel resolution. A multi-stream device (e.g. Venice F32 =
    // 2×16) must allocate an iso channel + IRM reservation per stream; without
    // this the first start resolves with empty caps (streamCount=1) and brings
    // the device up as a single aggregate stream it rejects. Cross-validated with
    // FFADO dice_avdevice.cpp prepare() (m_nb_rx/m_nb_tx loop). Default is a no-op
    // success for protocols that derive geometry by other means.
    virtual void EnsureRuntimeStreamGeometry(VoidCallback callback) {
        callback(kIOReturnSuccess);
    }

    virtual void PrepareDuplex(const AudioDuplexChannels& channels,
                               const DiceDesiredClockConfig& desiredClock,
                               PrepareCallback callback) = 0;
    virtual void ProgramRx(StageCallback callback) = 0;
    virtual void ProgramTxAndEnableDuplex(StageCallback callback) = 0;
    virtual void ConfirmDuplexStart(ConfirmCallback callback) = 0;
    virtual void ApplyClockConfig(const DiceDesiredClockConfig& desiredClock,
                                  ClockApplyCallback callback) = 0;
    virtual void ReadDuplexHealth(HealthCallback callback) = 0;

    virtual void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept {
        (void)cancel;
    }
    [[nodiscard]] virtual IOReturn StopDuplex() = 0;
    [[nodiscard]] virtual ::ASFW::IRM::IRMClient* GetIRMClient() const = 0;
};

} // namespace ASFW::Audio::DICE
