// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IDuplexDeviceControl.hpp - Protocol-neutral device-control seam

#pragma once

#include "DuplexRestartSession.hpp"

#include <DriverKit/IOReturn.h>

#include <atomic>
#include <functional>

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Audio {

class IDuplexDeviceControl {
public:
    using PrepareCallback = std::function<void(IOReturn, DuplexPrepareResult)>;
    using StageCallback = std::function<void(IOReturn, DuplexStageResult)>;
    using ConfirmCallback = std::function<void(IOReturn, DuplexConfirmResult)>;
    using ClockApplyCallback = std::function<void(IOReturn, ClockApplyResult)>;
    using HealthCallback = std::function<void(IOReturn, DuplexHealthResult)>;
    using VoidCallback = std::function<void(IOReturn)>;

    virtual ~IDuplexDeviceControl() = default;

    virtual void EnsureRuntimeStreamGeometry(VoidCallback callback) {
        callback(kIOReturnSuccess);
    }

    virtual void PrepareDuplex(const AudioDuplexChannels& channels,
                               const AudioClockConfig& desiredClock,
                               PrepareCallback callback) = 0;
    virtual void ProgramRx(StageCallback callback) = 0;
    virtual void ProgramTxAndEnableDuplex(StageCallback callback) = 0;
    virtual void ConfirmDuplexStart(ConfirmCallback callback) = 0;
    virtual void ApplyClockConfig(const AudioClockConfig& desiredClock,
                                  ClockApplyCallback callback) = 0;
    virtual void ReadDuplexHealth(HealthCallback callback) = 0;

    // Optional protocol-neutral staged teardown. Profiles which opt into the
    // interleaved stop recipe call playback disconnect before host IT stop,
    // then capture disconnect before host IR stop. Other protocols continue to
    // use StopDuplex() as one atomic device-side stage.
    virtual void DisconnectPlayback(VoidCallback callback) {
        callback(kIOReturnUnsupported);
    }
    virtual void DisconnectCapture(VoidCallback callback) {
        callback(kIOReturnUnsupported);
    }

    virtual void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept {
        (void)cancel;
    }
    [[nodiscard]] virtual IOReturn StopDuplex() = 0;
    [[nodiscard]] virtual ::ASFW::IRM::IRMClient* GetIRMClient() const = 0;
};

} // namespace ASFW::Audio
