// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DICEDuplexBringupController.hpp - Generic async duplex startup state machine for DICE devices

#pragma once

#include "DICETypes.hpp"
#include "DICETransaction.hpp"
#include "../../IDeviceProtocol.hpp"
#include "../../../Ports/ProtocolRegisterIO.hpp"
#include "../../../Ports/FireWireBusPort.hpp"
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOReturn.h>
#include <cstdint>
#include <functional>

namespace ASFW::Audio::DICE {

/// Manages generic DICE duplex startup/teardown.
///
/// All long-running methods (PrepareDuplex48k, ProgramRxForDuplex48k,
/// ProgramTxAndEnableDuplex48k, ConfirmDuplex48kStart, ReleaseOwner) are fully async — they use
/// DICETransaction callbacks directly and DispatchAsyncAfter for polling
/// retries.  No IOSleep anywhere in this class.
class DICEDuplexBringupController {
public:
    using VoidCallback = std::function<void(IOReturn)>;

    DICEDuplexBringupController(
        DICETransaction& diceReader,
        Protocols::Ports::ProtocolRegisterIO& io,
        Protocols::Ports::FireWireBusInfo& busInfo,
        IODispatchQueue* workQueue,
        GeneralSections sections);

    DICEDuplexBringupController(const DICEDuplexBringupController&) = delete;
    DICEDuplexBringupController& operator=(const DICEDuplexBringupController&) = delete;

    // Async duplex methods (were IOReturn, now void + callback)
    void PrepareDuplex48k(const AudioDuplexChannels& channels, VoidCallback callback);
    void ProgramRxForDuplex48k(VoidCallback callback);
    void ProgramTxAndEnableDuplex48k(VoidCallback callback);
    void ConfirmDuplex48kStart(VoidCallback callback);
    [[nodiscard]] IOReturn StopDuplex();       // stays sync — pure writes, no HW wait
    void ReleaseOwner(VoidCallback callback);

    [[nodiscard]] bool IsPrepared()     const noexcept { return duplexPrepared_; }
    [[nodiscard]] bool IsArmed()        const noexcept { return duplexArmed_; }
    [[nodiscard]] bool IsRunning()      const noexcept { return duplexRunning_; }
    [[nodiscard]] bool IsOwnerClaimed() const noexcept { return ownerClaimed_; }

private:
    // Async step chain for PrepareDuplex48k raw-parity path
    void DoReadGlobalStatus(AudioDuplexChannels channels, VoidCallback cb);
    void DoRefreshSectionLayout(AudioDuplexChannels channels, VoidCallback cb);
    void DoReadGlobalBeforeClaim(AudioDuplexChannels channels, VoidCallback cb);
    void DoReadOwnerBeforeClaim(AudioDuplexChannels channels, VoidCallback cb);
    void DoClaimOwner(AudioDuplexChannels channels, VoidCallback cb);
    void DoReadOwnerAfterClaim(AudioDuplexChannels channels, VoidCallback cb);
    void DoWriteClockSelect(AudioDuplexChannels channels, VoidCallback cb);
    void DoActiveClockCheck(AudioDuplexChannels channels, uint32_t accumulatedNotify, VoidCallback cb);
    void DoWaitClockAccepted(AudioDuplexChannels channels, uint32_t attempt, VoidCallback cb);
    void DoConfirmClockAccepted(AudioDuplexChannels channels, uint32_t observedNotify, VoidCallback cb);
    void DoReadGlobalAfterClockAccepted(AudioDuplexChannels channels, uint32_t observedNotify, IOReturn failureStatus, VoidCallback cb);
    void DoDiscoverStreams(AudioDuplexChannels channels, uint32_t step, VoidCallback cb);
    void DoProgramRx(AudioDuplexChannels channels, VoidCallback cb);
    void DoProgramTx(AudioDuplexChannels channels, VoidCallback cb);
    void DoFinishPrepare(VoidCallback cb);
    void DoRollback(IOReturn error, VoidCallback cb);

    // Async step chain for ConfirmDuplex48kStart
    void DoPollSourceLock(uint32_t attempt, uint32_t accumulatedNotify, VoidCallback cb);

    // Async stop / rollback sequencing
    void DoStopSequence(bool releaseOwner, VoidCallback cb);
    void DoStopDisableGlobal(bool releaseOwner, VoidCallback cb);
    void DoStopDisableTx(bool releaseOwner, VoidCallback cb);
    void DoStopReleaseTx(bool releaseOwner, VoidCallback cb);
    void DoStopDisableRx(bool releaseOwner, VoidCallback cb);
    void DoStopReleaseRx(bool releaseOwner, VoidCallback cb);
    void DoStopReleaseOwner(VoidCallback cb);

    [[nodiscard]] bool EnsureGenerationCurrent() const noexcept;
    [[nodiscard]] uint64_t OwnerValue() const noexcept;

    void ScheduleRetry(uint64_t delayMs, std::function<void()> work);

    DICETransaction& diceReader_;
    Protocols::Ports::ProtocolRegisterIO& io_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
    IODispatchQueue* workQueue_;   // NOT owned — borrowed from caller
    GeneralSections sections_;

    bool duplexPrepared_{false};
    bool duplexArmed_{false};
    bool duplexRunning_{false};
    bool ownerClaimed_{false};
    bool duplexRxProgrammed_{false};
    FW::Generation bringupGeneration_{0};
    uint8_t preparedTxIsoChannel_{0xFF};
    uint8_t preparedRxIsoChannel_{0xFF};
    IOReturn stopSequenceError_{kIOReturnSuccess};
};

} // namespace ASFW::Audio::DICE
