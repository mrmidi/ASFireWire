// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// IDICEDuplexProtocol.hpp - Typed DICE duplex restart/control surface

#pragma once

#include "DICERestartSession.hpp"

#include <DriverKit/IOReturn.h>
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

    virtual ~IDICEDuplexProtocol() = default;

    virtual void PrepareDuplex(const AudioDuplexChannels& channels,
                               const DiceDesiredClockConfig& desiredClock,
                               PrepareCallback callback) = 0;
    virtual void ProgramRx(StageCallback callback) = 0;
    virtual void ProgramTxAndEnableDuplex(StageCallback callback) = 0;
    virtual void ConfirmDuplexStart(ConfirmCallback callback) = 0;
    virtual void ApplyClockConfig(const DiceDesiredClockConfig& desiredClock,
                                  ClockApplyCallback callback) = 0;
    virtual void ReadDuplexHealth(HealthCallback callback) = 0;

    [[nodiscard]] virtual IOReturn StopDuplex() = 0;
    [[nodiscard]] virtual ::ASFW::IRM::IRMClient* GetIRMClient() const = 0;
};

} // namespace ASFW::Audio::DICE
