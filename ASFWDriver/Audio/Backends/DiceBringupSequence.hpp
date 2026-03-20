// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DiceBringupSequence.hpp
// Shared sequencing helper for legacy-parity DICE startup ordering.

#pragma once

#include <DriverKit/IOReturn.h>
#include <utility>

namespace ASFW::Audio::Detail {

template <typename PrepareFn,
          typename ReservePlaybackFn,
          typename ProgramRxFn,
          typename ReserveCaptureFn,
          typename StartReceiveFn,
          typename ProgramTxEnableFn,
          typename StartTransmitFn,
          typename ConfirmFn,
          typename RollbackFn>
IOReturn RunDiceBringupSequence(PrepareFn&& prepare,
                                ReservePlaybackFn&& reservePlayback,
                                ProgramRxFn&& programRx,
                                ReserveCaptureFn&& reserveCapture,
                                StartReceiveFn&& startReceive,
                                ProgramTxEnableFn&& programTxEnable,
                                StartTransmitFn&& startTransmit,
                                ConfirmFn&& confirm,
                                RollbackFn&& rollback) {
    if (const IOReturn status = prepare(); status != kIOReturnSuccess) {
        rollback();
        return status;
    }

    if (const IOReturn status = reservePlayback(); status != kIOReturnSuccess) {
        rollback();
        return status;
    }

    if (const IOReturn status = programRx(); status != kIOReturnSuccess) {
        rollback();
        return status;
    }

    if (const IOReturn status = reserveCapture(); status != kIOReturnSuccess) {
        rollback();
        return status;
    }

    if (const IOReturn status = startReceive(); status != kIOReturnSuccess) {
        rollback();
        return status;
    }

    if (const IOReturn status = programTxEnable(); status != kIOReturnSuccess) {
        rollback();
        return status;
    }

    if (const IOReturn status = startTransmit(); status != kIOReturnSuccess) {
        rollback();
        return status;
    }

    if (const IOReturn status = confirm(); status != kIOReturnSuccess) {
        rollback();
        return status;
    }

    return kIOReturnSuccess;
}

} // namespace ASFW::Audio::Detail
