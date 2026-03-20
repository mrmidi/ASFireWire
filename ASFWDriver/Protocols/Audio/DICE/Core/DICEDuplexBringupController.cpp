// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DICEDuplexBringupController.cpp - Raw-reference duplex startup for generic DICE devices

#include "DICEDuplexBringupController.hpp"

#include "DICENotificationMailbox.hpp"
#include "../../IDeviceProtocol.hpp"
#include "../../../../Common/WireFormat.hpp"
#include "../../../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <limits>
#include <memory>
#include <utility>

namespace ASFW::Audio::DICE {

namespace {

constexpr uint32_t kAsyncTimeoutMs = 5000;
constexpr uint32_t kPollIntervalMs = 10;
constexpr uint32_t kReadyTimeoutMs = 200;
constexpr uint32_t kStopSyncTimeoutMs = 5000;
constexpr uint32_t kStopSyncPollMs = 10;

constexpr uint32_t kClockSelect48kInternal =
    (ClockRateIndex::k48000 << ClockSelect::kRateShift) |
    static_cast<uint32_t>(ClockSource::Internal);
constexpr uint32_t kDisabledIsoChannel = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kRxSeqStartDefault = 0;
constexpr uint32_t kTxSpeedS400 = 2;

[[nodiscard]] IOReturn MapTransportStatus(Async::AsyncStatus status) noexcept {
    return Protocols::Ports::MapAsyncStatusToIOReturn(status);
}

void RecordFirstError(IOReturn& slot, IOReturn status) noexcept {
    if (slot == kIOReturnSuccess && status != kIOReturnSuccess) {
        slot = status;
    }
}

uint64_t DecodeOwnerOctlet(const uint8_t* data, size_t size) noexcept {
    if (data == nullptr || size < 8) {
        return 0;
    }
    return ASFW::FW::ReadBE64(data);
}

} // namespace

DICEDuplexBringupController::DICEDuplexBringupController(
    DICETransaction& diceReader,
    Protocols::Ports::ProtocolRegisterIO& io,
    Protocols::Ports::FireWireBusInfo& busInfo,
    IODispatchQueue* workQueue,
    GeneralSections sections)
    : diceReader_(diceReader)
    , io_(io)
    , busInfo_(busInfo)
    , workQueue_(workQueue)
    , sections_(sections) {
}

void DICEDuplexBringupController::ScheduleRetry(uint64_t delayMs, std::function<void()> work) {
    if (!work) {
        return;
    }

    if (!workQueue_) {
        if (delayMs > 0) {
            IOSleep(static_cast<unsigned int>(delayMs));
        }
        work();
        return;
    }

#ifdef ASFW_HOST_TEST
    if (workQueue_->UsesManualDispatchForTesting()) {
        workQueue_->DispatchAsyncAfter(delayMs * 1'000'000ULL, std::move(work));
        return;
    }

    workQueue_->DispatchAsync([delayMs, work = std::move(work)]() mutable {
        if (delayMs > 0) {
            IOSleep(static_cast<unsigned int>(delayMs));
        }
        work();
    });
#else
    auto sharedWork = std::make_shared<std::function<void()>>(std::move(work));
    workQueue_->DispatchAsync(^{
        if (delayMs > 0) {
            IOSleep(static_cast<unsigned int>(delayMs));
        }
        (*sharedWork)();
    });
#endif
}

bool DICEDuplexBringupController::EnsureGenerationCurrent() const noexcept {
    return busInfo_.GetGeneration() == bringupGeneration_;
}

uint64_t DICEDuplexBringupController::OwnerValue() const noexcept {
    const uint64_t localNodeId =
        0xFFC0ULL | static_cast<uint64_t>(busInfo_.GetLocalNodeID().value & 0x3FU);
    return (localNodeId << kOwnerNodeShift) | NotificationMailbox::kHandlerOffset;
}

void DICEDuplexBringupController::PrepareDuplex48k(
    const AudioDuplexChannels& channels,
    VoidCallback callback) {
    if (channels.deviceToHostIsoChannel > 63 || channels.hostToDeviceIsoChannel > 63) {
        callback(kIOReturnBadArgument);
        return;
    }
    if (!busInfo_.GetLocalNodeID().IsValid()) {
        callback(kIOReturnNotReady);
        return;
    }
    if (duplexPrepared_ || duplexArmed_ || duplexRunning_ || ownerClaimed_) {
        const IOReturn stopStatus = StopDuplex();
        if (stopStatus != kIOReturnSuccess) {
            callback(stopStatus);
            return;
        }
    }

    NotificationMailbox::Reset();
    duplexPrepared_ = false;
    duplexArmed_ = false;
    duplexRunning_ = false;
    ownerClaimed_ = false;
    duplexRxProgrammed_ = false;
    preparedTxIsoChannel_ = 0xFF;
    preparedRxIsoChannel_ = 0xFF;
    stopSequenceError_ = kIOReturnSuccess;
    bringupGeneration_ = busInfo_.GetGeneration();

    ASFW_LOG(DICE,
             "PrepareDuplex48k: raw parity start gen=%u localNode=0x%02x rxIso=%u txIso=%u",
             bringupGeneration_.value,
             busInfo_.GetLocalNodeID().value,
             channels.hostToDeviceIsoChannel,
             channels.deviceToHostIsoChannel);

    DoReadGlobalStatus(channels, std::move(callback));
}

void DICEDuplexBringupController::DoReadGlobalStatus(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    io_.ReadQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kStatus),
                   [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                        const IOReturn status = MapTransportStatus(transportStatus);
                        if (status != kIOReturnSuccess) {
                            DoRollback(status, std::move(cb));
                            return;
                        }
                        DoRefreshSectionLayout(channels, std::move(cb));
                    });
}

void DICEDuplexBringupController::DoRefreshSectionLayout(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    diceReader_.ReadGeneralSections([this, channels, cb = std::move(cb)](IOReturn status, GeneralSections sections) mutable {
        if (status != kIOReturnSuccess) {
            DoRollback(status, std::move(cb));
            return;
        }

        sections_ = sections;
        DoReadGlobalBeforeClaim(channels, std::move(cb));
    });
}

void DICEDuplexBringupController::DoReadGlobalBeforeClaim(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    diceReader_.ReadGlobalStateFull(sections_,
                                    [this, channels, cb = std::move(cb)](IOReturn status, GlobalState state) mutable {
                                if (status != kIOReturnSuccess) {
                                    DoRollback(status, std::move(cb));
                                    return;
                                }

                                ASFW_LOG(DICE,
                                         "PrepareDuplex48k: global pre-claim owner=0x%016llx enable=%u notify=0x%08x",
                                         state.owner,
                                         state.enabled ? 1U : 0U,
                                         state.notification);
                                DoReadOwnerBeforeClaim(channels, std::move(cb));
                            });
}

void DICEDuplexBringupController::DoReadOwnerBeforeClaim(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    io_.ReadBlock(MakeDICEAddress(sections_.global.offset + GlobalOffset::kOwnerHi),
                  8,
                  [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, std::span<const uint8_t> payload) mutable {
                      const IOReturn status = MapTransportStatus(transportStatus);
                      if (status != kIOReturnSuccess || payload.size() < 8) {
                          DoRollback((status == kIOReturnSuccess) ? kIOReturnUnderrun : status, std::move(cb));
                          return;
                      }

                      ASFW_LOG(DICE,
                               "PrepareDuplex48k: owner before claim=0x%016llx",
                               DecodeOwnerOctlet(payload.data(), payload.size()));
                      DoClaimOwner(channels, std::move(cb));
                  });
}

void DICEDuplexBringupController::DoClaimOwner(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    const uint64_t ownerValue = OwnerValue();
    io_.CompareSwap64BE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kOwnerHi),
                        kOwnerNoOwner,
                        ownerValue,
                        [this, channels, ownerValue, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint64_t previous) mutable {
                              const IOReturn status = MapTransportStatus(transportStatus);
                              if (status != kIOReturnSuccess) {
                                  DoRollback(status, std::move(cb));
                                  return;
                              }
                              if (previous != kOwnerNoOwner && previous != ownerValue) {
                                  DoRollback(kIOReturnExclusiveAccess, std::move(cb));
                                  return;
                              }

                              ownerClaimed_ = true;
                              DoReadOwnerAfterClaim(channels, std::move(cb));
                          });
}

void DICEDuplexBringupController::DoReadOwnerAfterClaim(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    io_.ReadBlock(MakeDICEAddress(sections_.global.offset + GlobalOffset::kOwnerHi),
                  8,
                  [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, std::span<const uint8_t> payload) mutable {
                      const IOReturn status = MapTransportStatus(transportStatus);
                      if (status != kIOReturnSuccess || payload.size() < 8) {
                          DoRollback((status == kIOReturnSuccess) ? kIOReturnUnderrun : status, std::move(cb));
                          return;
                      }

                      const uint64_t ownerReadback = DecodeOwnerOctlet(payload.data(), payload.size());
                      if (ownerReadback != OwnerValue()) {
                          DoRollback(kIOReturnExclusiveAccess, std::move(cb));
                          return;
                      }

                      DoWriteClockSelect(channels, std::move(cb));
                  });
}

void DICEDuplexBringupController::DoWriteClockSelect(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    NotificationMailbox::Reset();
    io_.WriteQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kClockSelect),
                    kClockSelect48kInternal,
                    [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus) mutable {
                         const IOReturn status = MapTransportStatus(transportStatus);
                         if (status != kIOReturnSuccess) {
                             DoRollback(status, std::move(cb));
                             return;
                         }
                         DoWaitClockAccepted(channels, 0, std::move(cb));
                     });
}

void DICEDuplexBringupController::DoWaitClockAccepted(
    AudioDuplexChannels channels,
    uint32_t attempt,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    const uint32_t mailboxBits = NotificationMailbox::Consume();
    if ((mailboxBits & Notify::kClockAccepted) != 0) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: observed async CLOCK_ACCEPTED bits=0x%08x",
                 mailboxBits);
        DoReadGlobalAfterClockAccepted(
            channels, mailboxBits, kIOReturnNotReady, std::move(cb));
        return;
    }

    if (attempt * kPollIntervalMs >= kAsyncTimeoutMs) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: CLOCK_ACCEPTED wait reached %u ms; performing final confirmation",
                 kAsyncTimeoutMs);
        DoConfirmClockAccepted(channels, mailboxBits, std::move(cb));
        return;
    }

    ScheduleRetry(kPollIntervalMs, [this, channels, attempt, cb = std::move(cb)]() mutable {
        DoWaitClockAccepted(channels, attempt + 1, std::move(cb));
    });
}

void DICEDuplexBringupController::DoConfirmClockAccepted(
    AudioDuplexChannels channels,
    uint32_t observedNotify,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    const uint32_t lateMailboxBits = NotificationMailbox::Consume();
    if ((lateMailboxBits & Notify::kClockAccepted) != 0) {
        ASFW_LOG(DICE,
                 "PrepareDuplex48k: observed late async CLOCK_ACCEPTED bits=0x%08x",
                 lateMailboxBits);
    }

    DoReadGlobalAfterClockAccepted(
        channels, observedNotify | lateMailboxBits, kIOReturnTimeout, std::move(cb));
}

void DICEDuplexBringupController::DoReadGlobalAfterClockAccepted(
    AudioDuplexChannels channels,
    uint32_t observedNotify,
    IOReturn failureStatus,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    diceReader_.ReadGlobalStateFull(sections_,
                                    [this,
                                     channels,
                                     observedNotify,
                                     failureStatus,
                                     cb = std::move(cb)](IOReturn status, GlobalState state) mutable {
                                if (status != kIOReturnSuccess) {
                                    DoRollback(status, std::move(cb));
                                    return;
                                }

                                const uint32_t combinedNotify = observedNotify | state.notification;
                                const bool clockAccepted =
                                    (combinedNotify & Notify::kClockAccepted) != 0;
                                const bool sourceLocked48k =
                                    IsSourceLocked(state.status) &&
                                    NominalRateHz(state.status) == 48000U;
                                const bool sampleRate48k = state.sampleRate == 48000U;

                                if (state.clockSelect != kClockSelect48kInternal) {
                                    ASFW_LOG(DICE,
                                             "PrepareDuplex48k: clock confirm failed, clockSelect=0x%08x notify=0x%08x status=0x%08x sampleRate=%u",
                                             state.clockSelect,
                                             combinedNotify,
                                             state.status,
                                             state.sampleRate);
                                    DoRollback(failureStatus, std::move(cb));
                                    return;
                                }

                                if (!clockAccepted && !(sourceLocked48k && sampleRate48k)) {
                                    ASFW_LOG(DICE,
                                             "PrepareDuplex48k: CLOCK_ACCEPTED not confirmed, notify=0x%08x status=0x%08x sampleRate=%u",
                                             combinedNotify,
                                             state.status,
                                             state.sampleRate);
                                    DoRollback(failureStatus, std::move(cb));
                                    return;
                                }

                                if (!clockAccepted) {
                                    ASFW_LOG(DICE,
                                             "PrepareDuplex48k: confirmed clock via global state after timeout, status=0x%08x sampleRate=%u",
                                             state.status,
                                             state.sampleRate);
                                }

                                DoDiscoverStreams(channels, 0, std::move(cb));
                            });
}

void DICEDuplexBringupController::DoDiscoverStreams(
    AudioDuplexChannels channels,
    uint32_t step,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    const uint32_t txBase = sections_.txStreamFormat.offset;
    const uint32_t rxBase = sections_.rxStreamFormat.offset;

    switch (step) {
    case 0:
        io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kNumber),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 1, std::move(cb));
                        });
        return;
    case 1:
        io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kNumber),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 2, std::move(cb));
                        });
        return;
    case 2:
        io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kSize),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 3, std::move(cb));
                        });
        return;
    case 3:
        io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kIsochronous),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 4, std::move(cb));
                        });
        return;
    case 4:
        io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kNumberAudio),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 5, std::move(cb));
                        });
        return;
    case 5:
        io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kNumberMidi),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 6, std::move(cb));
                        });
        return;
    case 6:
        io_.ReadQuadBE(MakeDICEAddress(txBase + TxOffset::kSpeed),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 7, std::move(cb));
                        });
        return;
    case 7:
        io_.ReadBlock(MakeDICEAddress(txBase + TxOffset::kNames),
                      256,
                      [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, std::span<const uint8_t> payload) mutable {
                          const IOReturn status = MapTransportStatus(transportStatus);
                          if (status != kIOReturnSuccess || payload.size() < 256) {
                              DoRollback((status == kIOReturnSuccess) ? kIOReturnUnderrun : status, std::move(cb));
                              return;
                          }
                          DoDiscoverStreams(channels, 8, std::move(cb));
                      });
        return;
    case 8:
        io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kSize),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 9, std::move(cb));
                        });
        return;
    case 9:
        io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kIsochronous),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 10, std::move(cb));
                        });
        return;
    case 10:
        io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kNumberMidi),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 11, std::move(cb));
                        });
        return;
    case 11:
        io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kSeqStart),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 12, std::move(cb));
                        });
        return;
    case 12:
        io_.ReadQuadBE(MakeDICEAddress(rxBase + RxOffset::kNumberAudio),
                       [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                            const IOReturn status = MapTransportStatus(transportStatus);
                            if (status != kIOReturnSuccess) {
                                DoRollback(status, std::move(cb));
                                return;
                            }
                            DoDiscoverStreams(channels, 13, std::move(cb));
                        });
        return;
    case 13:
        io_.ReadBlock(MakeDICEAddress(rxBase + RxOffset::kNames),
                      256,
                      [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, std::span<const uint8_t> payload) mutable {
                          const IOReturn status = MapTransportStatus(transportStatus);
                          if (status != kIOReturnSuccess || payload.size() < 256) {
                              DoRollback((status == kIOReturnSuccess) ? kIOReturnUnderrun : status, std::move(cb));
                              return;
                          }
                          preparedRxIsoChannel_ = channels.hostToDeviceIsoChannel;
                          preparedTxIsoChannel_ = channels.deviceToHostIsoChannel;
                          DoFinishPrepare(std::move(cb));
                      });
        return;
    default:
        preparedRxIsoChannel_ = channels.hostToDeviceIsoChannel;
        preparedTxIsoChannel_ = channels.deviceToHostIsoChannel;
        DoFinishPrepare(std::move(cb));
        return;
    }
}

void DICEDuplexBringupController::DoProgramRx(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    io_.ReadQuadBE(MakeDICEAddress(sections_.rxStreamFormat.offset + RxOffset::kSize),
                    [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t rxSize) mutable {
                        const IOReturn status = MapTransportStatus(transportStatus);
                        ASFW_LOG(DICE,
                                 "DoProgramRx: RX_SIZE transport status=%u value=0x%08x",
                                 static_cast<unsigned>(transportStatus),
                                 rxSize);
                        if (status != kIOReturnSuccess) {
                            DoRollback(status, std::move(cb));
                            return;
                        }

                        ASFW_LOG(DICE,
                                 "DoProgramRx: RX_SIZE complete, entering RX program lambda value=0x%08x",
                                 rxSize);
                        ASFW_LOG(DICE,
                                 "DoProgramRx: writing RX isoch channel %u",
                                 channels.hostToDeviceIsoChannel);
                        io_.WriteQuadBE(MakeDICEAddress(sections_.rxStreamFormat.offset + RxOffset::kIsochronous),
                                        channels.hostToDeviceIsoChannel,
                                        [this, channels, cb = std::move(cb)](Async::AsyncStatus isoTransportStatus) mutable {
                                             const IOReturn isoStatus = MapTransportStatus(isoTransportStatus);
                                             if (isoStatus != kIOReturnSuccess) {
                                                 DoRollback(isoStatus, std::move(cb));
                                                 return;
                                             }

                                             io_.WriteQuadBE(MakeDICEAddress(sections_.rxStreamFormat.offset + RxOffset::kSeqStart),
                                                             kRxSeqStartDefault,
                                                             [this, channels, cb = std::move(cb)](Async::AsyncStatus seqTransportStatus) mutable {
                                                                  const IOReturn seqStatus = MapTransportStatus(seqTransportStatus);
                                                                  if (seqStatus != kIOReturnSuccess) {
                                                                      DoRollback(seqStatus, std::move(cb));
                                                                      return;
                                                                  }
                                                                  duplexRxProgrammed_ = true;
                                                                  cb(kIOReturnSuccess);
                                                              });
                                         });
                    });
}

void DICEDuplexBringupController::DoProgramTx(
    AudioDuplexChannels channels,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        DoRollback(kIOReturnOffline, std::move(cb));
        return;
    }

    io_.ReadQuadBE(MakeDICEAddress(sections_.txStreamFormat.offset + TxOffset::kSize),
                   [this, channels, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint32_t) mutable {
                        const IOReturn status = MapTransportStatus(transportStatus);
                        if (status != kIOReturnSuccess) {
                            DoRollback(status, std::move(cb));
                            return;
                        }

                        io_.WriteQuadBE(MakeDICEAddress(sections_.txStreamFormat.offset + TxOffset::kIsochronous),
                                        channels.deviceToHostIsoChannel,
                                        [this, channels, cb = std::move(cb)](Async::AsyncStatus isoTransportStatus) mutable {
                                             const IOReturn isoStatus = MapTransportStatus(isoTransportStatus);
                                             if (isoStatus != kIOReturnSuccess) {
                                                 DoRollback(isoStatus, std::move(cb));
                                                 return;
                                             }

                                             io_.WriteQuadBE(MakeDICEAddress(sections_.txStreamFormat.offset + TxOffset::kSpeed),
                                                             kTxSpeedS400,
                                                             [this, cb = std::move(cb)](Async::AsyncStatus speedTransportStatus) mutable {
                                                                  const IOReturn speedStatus = MapTransportStatus(speedTransportStatus);
                                                                  if (speedStatus != kIOReturnSuccess) {
                                                                      DoRollback(speedStatus, std::move(cb));
                                                                      return;
                                                                  }
                                                                  io_.WriteQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kEnable),
                                                                                  1,
                                                                                  [this, cb = std::move(cb)](Async::AsyncStatus enableTransportStatus) mutable {
                                                                                       const IOReturn enableStatus = MapTransportStatus(enableTransportStatus);
                                                                                       if (enableStatus != kIOReturnSuccess) {
                                                                                           DoRollback(enableStatus, std::move(cb));
                                                                                           return;
                                                                                       }
                                                                                       duplexArmed_ = true;
                                                                                       cb(kIOReturnSuccess);
                                                                                   });
                                                              });
                                         });
                    });
}

void DICEDuplexBringupController::DoFinishPrepare(VoidCallback cb) {
    duplexPrepared_ = true;
    duplexArmed_ = false;
    duplexRunning_ = false;
    duplexRxProgrammed_ = false;
    cb(kIOReturnSuccess);
}

void DICEDuplexBringupController::DoRollback(IOReturn error, VoidCallback cb) {
    if (!ownerClaimed_) {
        duplexPrepared_ = false;
        duplexArmed_ = false;
        duplexRunning_ = false;
        duplexRxProgrammed_ = false;
        preparedTxIsoChannel_ = 0xFF;
        preparedRxIsoChannel_ = 0xFF;
        cb(error);
        return;
    }

    if (!EnsureGenerationCurrent()) {
        duplexPrepared_ = false;
        duplexArmed_ = false;
        duplexRunning_ = false;
        ownerClaimed_ = false;
        duplexRxProgrammed_ = false;
        preparedTxIsoChannel_ = 0xFF;
        preparedRxIsoChannel_ = 0xFF;
        cb(error);
        return;
    }

    DoStopSequence(ownerClaimed_, [this, error, cb = std::move(cb)](IOReturn stopStatus) mutable {
        if (stopStatus != kIOReturnSuccess) {
            ASFW_LOG(DICE, "DoRollback: cleanup reported 0x%x after start failure 0x%x", stopStatus, error);
        }
        cb(error);
    });
}

void DICEDuplexBringupController::DoPollSourceLock(
    uint32_t attempt,
    uint32_t accumulatedNotify,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        (void)StopDuplex();
        cb(kIOReturnOffline);
        return;
    }

    uint32_t notify = accumulatedNotify | NotificationMailbox::Consume();
    io_.ReadQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kStatus),
                   [this, attempt, notify, cb = std::move(cb)](Async::AsyncStatus statusTransport, uint32_t statusValue) mutable {
                        const IOReturn statusRead = MapTransportStatus(statusTransport);
                        if (statusRead != kIOReturnSuccess) {
                            (void)StopDuplex();
                            cb(statusRead);
                            return;
                        }

                        if (IsSourceLocked(statusValue)) {
                            io_.ReadQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kNotification),
                                           [this, notify, statusValue, cb = std::move(cb)](Async::AsyncStatus notifyTransport, uint32_t nv) mutable {
                                                const IOReturn ns = MapTransportStatus(notifyTransport);
                                                if (ns == kIOReturnSuccess) {
                                                    notify |= nv;
                                                }
                                                io_.ReadQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kExtStatus),
                                                               [this, notify, statusValue, cb = std::move(cb)](Async::AsyncStatus extTransport, uint32_t ev) mutable {
                                                                    const IOReturn es = MapTransportStatus(extTransport);
                                                                    const uint32_t extStatus =
                                                                        (es == kIOReturnSuccess) ? ev : 0;
                                                                    duplexRunning_ = true;
                                                                    ASFW_LOG(DICE,
                                                                             "ConfirmDuplex48kStart: source lock ok notify=0x%08x status=0x%08x ext=0x%08x",
                                                                             notify,
                                                                             statusValue,
                                                                             extStatus);
                                                                    cb(kIOReturnSuccess);
                                                                });
                                            });
                            return;
                        }

                        if (attempt * kPollIntervalMs >= kReadyTimeoutMs) {
                            (void)StopDuplex();
                            cb(kIOReturnTimeout);
                            return;
                        }

                        ScheduleRetry(kPollIntervalMs,
                                      [this, attempt, notify, cb = std::move(cb)]() mutable {
                                          DoPollSourceLock(attempt + 1, notify, std::move(cb));
                                      });
                    });
}

void DICEDuplexBringupController::ProgramRxForDuplex48k(VoidCallback callback) {
    if (!duplexPrepared_) {
        callback(kIOReturnNotReady);
        return;
    }
    if (!EnsureGenerationCurrent()) {
        callback(kIOReturnOffline);
        return;
    }

    if (preparedTxIsoChannel_ > 63 || preparedRxIsoChannel_ > 63) {
        callback(kIOReturnNotReady);
        return;
    }

    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = preparedTxIsoChannel_,
        .hostToDeviceIsoChannel = preparedRxIsoChannel_,
    };
    DoProgramRx(channels, std::move(callback));
}

void DICEDuplexBringupController::ProgramTxAndEnableDuplex48k(VoidCallback callback) {
    if (!duplexPrepared_ || !duplexRxProgrammed_) {
        callback(kIOReturnNotReady);
        return;
    }

    if (!EnsureGenerationCurrent()) {
        callback(kIOReturnOffline);
        return;
    }

    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = preparedTxIsoChannel_,
        .hostToDeviceIsoChannel = preparedRxIsoChannel_,
    };
    DoProgramTx(channels, std::move(callback));
}

void DICEDuplexBringupController::ConfirmDuplex48kStart(VoidCallback callback) {
    if (!duplexPrepared_ || !duplexArmed_) {
        callback(kIOReturnNotReady);
        return;
    }

    NotificationMailbox::Reset();
    DoPollSourceLock(0, 0, std::move(callback));
}

IOReturn DICEDuplexBringupController::StopDuplex() {
    if (!duplexPrepared_ && !duplexArmed_ && !duplexRunning_ && !ownerClaimed_) {
        return kIOReturnSuccess;
    }

    struct WaitState {
        std::atomic<bool> done{false};
        std::atomic<IOReturn> status{kIOReturnTimeout};
    };

    auto waitState = std::make_shared<WaitState>();
    DoStopSequence(true, [waitState](IOReturn status) {
        waitState->status.store(status, std::memory_order_relaxed);
        waitState->done.store(true, std::memory_order_release);
    });

    for (uint32_t waited = 0; waited < kStopSyncTimeoutMs; waited += kStopSyncPollMs) {
        if (waitState->done.load(std::memory_order_acquire)) {
            return waitState->status.load(std::memory_order_relaxed);
        }
        IOSleep(kStopSyncPollMs);
    }

    return waitState->done.load(std::memory_order_acquire)
        ? waitState->status.load(std::memory_order_relaxed)
        : kIOReturnTimeout;
}

void DICEDuplexBringupController::ReleaseOwner(VoidCallback callback) {
    if (!ownerClaimed_) {
        callback(kIOReturnSuccess);
        return;
    }

    if (!EnsureGenerationCurrent()) {
        ownerClaimed_ = false;
        callback(kIOReturnSuccess);
        return;
    }

    io_.CompareSwap64BE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kOwnerHi),
                        OwnerValue(),
                        kOwnerNoOwner,
                        [this, callback = std::move(callback)](Async::AsyncStatus transportStatus, uint64_t previous) mutable {
                              const IOReturn status = MapTransportStatus(transportStatus);
                              if (status != kIOReturnSuccess) {
                                  callback(status);
                                  return;
                              }
                              if (previous != OwnerValue() && previous != kOwnerNoOwner) {
                                  callback(kIOReturnExclusiveAccess);
                                  return;
                              }

                              ownerClaimed_ = false;
                              callback(kIOReturnSuccess);
                          });
}

void DICEDuplexBringupController::DoStopSequence(
    bool releaseOwner,
    VoidCallback cb) {
    stopSequenceError_ = kIOReturnSuccess;
    DoStopDisableGlobal(releaseOwner, std::move(cb));
}

void DICEDuplexBringupController::DoStopDisableGlobal(
    bool releaseOwner,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        RecordFirstError(stopSequenceError_, kIOReturnOffline);
        duplexPrepared_ = false;
        duplexArmed_ = false;
        duplexRunning_ = false;
        ownerClaimed_ = false;
        duplexRxProgrammed_ = false;
        preparedTxIsoChannel_ = 0xFF;
        preparedRxIsoChannel_ = 0xFF;
        cb(stopSequenceError_);
        return;
    }

    io_.WriteQuadBE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kEnable),
                    0,
                    [this, releaseOwner, cb = std::move(cb)](Async::AsyncStatus transportStatus) mutable {
                         const IOReturn status = MapTransportStatus(transportStatus);
                         RecordFirstError(stopSequenceError_, status);
                         DoStopDisableTx(releaseOwner, std::move(cb));
                     });
}

void DICEDuplexBringupController::DoStopDisableTx(
    bool releaseOwner,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        RecordFirstError(stopSequenceError_, kIOReturnOffline);
        cb(stopSequenceError_);
        return;
    }

    io_.ReadQuadBE(MakeDICEAddress(sections_.txStreamFormat.offset + TxOffset::kSize),
                   [this, releaseOwner, cb = std::move(cb)](Async::AsyncStatus readTransportStatus, uint32_t) mutable {
                        const IOReturn readStatus = MapTransportStatus(readTransportStatus);
                        RecordFirstError(stopSequenceError_, readStatus);
                        io_.WriteQuadBE(MakeDICEAddress(sections_.txStreamFormat.offset + TxOffset::kIsochronous),
                                        kDisabledIsoChannel,
                                        [this, releaseOwner, cb = std::move(cb)](Async::AsyncStatus isoTransportStatus) mutable {
                                             const IOReturn isoStatus = MapTransportStatus(isoTransportStatus);
                                             RecordFirstError(stopSequenceError_, isoStatus);
                                             io_.WriteQuadBE(MakeDICEAddress(sections_.txStreamFormat.offset + TxOffset::kSpeed),
                                                             kTxSpeedS400,
                                                             [this, releaseOwner, cb = std::move(cb)](Async::AsyncStatus speedTransportStatus) mutable {
                                                                  const IOReturn speedStatus = MapTransportStatus(speedTransportStatus);
                                                                  RecordFirstError(stopSequenceError_, speedStatus);
                                                                  DoStopReleaseTx(releaseOwner, std::move(cb));
                                                              });
                                         });
                    });
}

void DICEDuplexBringupController::DoStopReleaseTx(
    bool releaseOwner,
    VoidCallback cb) {
    preparedTxIsoChannel_ = 0xFF;
    DoStopDisableRx(releaseOwner, std::move(cb));
}

void DICEDuplexBringupController::DoStopDisableRx(
    bool releaseOwner,
    VoidCallback cb) {
    if (!EnsureGenerationCurrent()) {
        RecordFirstError(stopSequenceError_, kIOReturnOffline);
        cb(stopSequenceError_);
        return;
    }

    io_.ReadQuadBE(MakeDICEAddress(sections_.rxStreamFormat.offset + RxOffset::kSize),
                   [this, releaseOwner, cb = std::move(cb)](Async::AsyncStatus readTransportStatus, uint32_t) mutable {
                        const IOReturn readStatus = MapTransportStatus(readTransportStatus);
                        RecordFirstError(stopSequenceError_, readStatus);
                        io_.WriteQuadBE(MakeDICEAddress(sections_.rxStreamFormat.offset + RxOffset::kIsochronous),
                                        kDisabledIsoChannel,
                                        [this, releaseOwner, cb = std::move(cb)](Async::AsyncStatus isoTransportStatus) mutable {
                                             const IOReturn isoStatus = MapTransportStatus(isoTransportStatus);
                                             RecordFirstError(stopSequenceError_, isoStatus);
                                             io_.WriteQuadBE(MakeDICEAddress(sections_.rxStreamFormat.offset + RxOffset::kSeqStart),
                                                             kRxSeqStartDefault,
                                                             [this, releaseOwner, cb = std::move(cb)](Async::AsyncStatus seqTransportStatus) mutable {
                                                                  const IOReturn seqStatus = MapTransportStatus(seqTransportStatus);
                                                                  RecordFirstError(stopSequenceError_, seqStatus);
                                                                  DoStopReleaseRx(releaseOwner, std::move(cb));
                                                              });
                                         });
                    });
}

void DICEDuplexBringupController::DoStopReleaseRx(
    bool releaseOwner,
    VoidCallback cb) {
    preparedRxIsoChannel_ = 0xFF;
    if (releaseOwner) {
        DoStopReleaseOwner(std::move(cb));
        return;
    }

    duplexPrepared_ = false;
    duplexArmed_ = false;
    duplexRunning_ = false;
    duplexRxProgrammed_ = false;
    cb(stopSequenceError_);
}

void DICEDuplexBringupController::DoStopReleaseOwner(VoidCallback cb) {
    if (!ownerClaimed_) {
        duplexPrepared_ = false;
        duplexArmed_ = false;
        duplexRunning_ = false;
        duplexRxProgrammed_ = false;
        cb(stopSequenceError_);
        return;
    }

    if (!EnsureGenerationCurrent()) {
        RecordFirstError(stopSequenceError_, kIOReturnOffline);
        duplexPrepared_ = false;
        duplexArmed_ = false;
        duplexRunning_ = false;
        ownerClaimed_ = false;
        duplexRxProgrammed_ = false;
        preparedTxIsoChannel_ = 0xFF;
        preparedRxIsoChannel_ = 0xFF;
        cb(stopSequenceError_);
        return;
    }

    io_.CompareSwap64BE(MakeDICEAddress(sections_.global.offset + GlobalOffset::kOwnerHi),
                        OwnerValue(),
                        kOwnerNoOwner,
                        [this, cb = std::move(cb)](Async::AsyncStatus transportStatus, uint64_t previous) mutable {
                              const IOReturn status = MapTransportStatus(transportStatus);
                              RecordFirstError(stopSequenceError_, status);
                              if (status == kIOReturnSuccess &&
                                  previous != OwnerValue() &&
                                  previous != kOwnerNoOwner) {
                                  RecordFirstError(stopSequenceError_, kIOReturnExclusiveAccess);
                              }

                              duplexPrepared_ = false;
                              duplexArmed_ = false;
                              duplexRunning_ = false;
                              ownerClaimed_ = false;
                              duplexRxProgrammed_ = false;
                              preparedTxIsoChannel_ = 0xFF;
                              preparedRxIsoChannel_ = 0xFF;
                              cb(stopSequenceError_);
                          });
}

} // namespace ASFW::Audio::DICE
