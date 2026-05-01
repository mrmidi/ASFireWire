// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DICETcatProtocol.cpp - Generic DICE/TCAT protocol state and duplex control

#include "DICETcatProtocol.hpp"

#include "../../../../Logging/Logging.hpp"

#include <memory>
#include <utility>

namespace ASFW::Audio::DICE::TCAT {

DICETcatProtocol::DICETcatProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                   Protocols::Ports::FireWireBusInfo& busInfo,
                                   uint16_t nodeId,
                                   ::ASFW::IRM::IRMClient* irmClient)
    : busInfo_(busInfo)
    , irmClient_(irmClient)
    , io_(busOps, busInfo, nodeId)
    , diceReader_(io_) {
}

IOReturn DICETcatProtocol::Initialize() {
    if (!duplexCtrl_) {
        duplexCtrl_.emplace(diceReader_, io_, busInfo_, nullptr /*workQueue*/, GeneralSections{});
    }

    initialized_ = true;
    ASFW_LOG(DICE, "DICETcatProtocol::Initialize defers generic discovery until runtime");
    return kIOReturnSuccess;
}

IOReturn DICETcatProtocol::Shutdown() {
    if (duplexCtrl_) {
        if (duplexCtrl_->IsPrepared() || duplexCtrl_->IsRunning()) {
            const IOReturn stopStatus = duplexCtrl_->StopDuplex();
            if (stopStatus != kIOReturnSuccess && stopStatus != kIOReturnUnsupported) {
                ASFW_LOG(DICE, "DICETcatProtocol::Shutdown duplex stop failed: 0x%x", stopStatus);
            }
        }

        duplexCtrl_->ReleaseOwner([](IOReturn status) {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(DICE, "DICETcatProtocol::Shutdown ReleaseOwner failed: 0x%x", status);
            }
        });
    }

    sections_ = {};
    sectionsLoaded_ = false;
    initialized_ = false;
    ResetRuntimeCaps();
    return kIOReturnSuccess;
}

bool DICETcatProtocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    if (!runtimeCapsValid_.load(std::memory_order_acquire)) {
        return false;
    }

    outCaps.sampleRateHz = runtimeSampleRateHz_.load(std::memory_order_relaxed);
    outCaps.hostInputPcmChannels = hostInputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.hostOutputPcmChannels = hostOutputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.deviceToHostAm824Slots = deviceToHostAm824Slots_.load(std::memory_order_relaxed);
    outCaps.hostToDeviceAm824Slots = hostToDeviceAm824Slots_.load(std::memory_order_relaxed);
    outCaps.deviceToHostActiveStreams = deviceToHostActiveStreams_.load(std::memory_order_relaxed);
    outCaps.hostToDeviceActiveStreams = hostToDeviceActiveStreams_.load(std::memory_order_relaxed);
    outCaps.deviceToHostIsoChannel =
        static_cast<uint8_t>(deviceToHostIsoChannel_.load(std::memory_order_relaxed));
    outCaps.hostToDeviceIsoChannel =
        static_cast<uint8_t>(hostToDeviceIsoChannel_.load(std::memory_order_relaxed));
    return true;
}

void DICETcatProtocol::RefreshRuntimeAudioStreamCaps(VoidCallback callback) {
    EnsureRuntimeCapsLoaded(std::move(callback));
}

void DICETcatProtocol::PrepareDuplex(const AudioDuplexChannels& channels,
                                     const DiceDesiredClockConfig& desiredClock,
                                     PrepareCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->PrepareDuplex(
        channels,
        desiredClock,
        [this, callback = std::move(callback)](IOReturn status, DiceDuplexPrepareResult result) mutable {
            if (status == kIOReturnSuccess) {
                CacheRuntimeCaps(result.runtimeCaps);
            }
            callback(status, result);
        });
}

void DICETcatProtocol::ProgramRx(StageCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->ProgramRx(std::move(callback));
}

void DICETcatProtocol::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->ProgramTxAndEnableDuplex(std::move(callback));
}

void DICETcatProtocol::ConfirmDuplexStart(ConfirmCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->ConfirmDuplexStart(
        [this, callback = std::move(callback)](IOReturn status, DiceDuplexConfirmResult result) mutable {
            if (status == kIOReturnSuccess) {
                CacheRuntimeCaps(result.runtimeCaps);
            }
            callback(status, result);
        });
}

void DICETcatProtocol::ApplyClockConfig(const DiceDesiredClockConfig& desiredClock,
                                        ClockApplyCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->ApplyClockConfig(
        desiredClock,
        [this, callback = std::move(callback)](IOReturn status, DiceClockApplyResult result) mutable {
            if (status == kIOReturnSuccess) {
                CacheRuntimeCaps(result.runtimeCaps);
            }
            callback(status, result);
        });
}

void DICETcatProtocol::ReadDuplexHealth(HealthCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    EnsureSectionsLoaded([this, callback = std::move(callback)](IOReturn sectionStatus) mutable {
        if (sectionStatus != kIOReturnSuccess) {
            callback(sectionStatus, {});
            return;
        }

        diceReader_.ReadGlobalState(
            sections_,
            [this, callback = std::move(callback)](IOReturn status, GlobalState global) mutable {
                if (status != kIOReturnSuccess) {
                    callback(status, {});
                    return;
                }

                AudioStreamRuntimeCaps caps{};
                (void)GetRuntimeAudioStreamCaps(caps);

                callback(status,
                         DiceDuplexHealthResult{
                             .generation = busInfo_.GetGeneration(),
                             .appliedClock =
                                 DiceDesiredClockConfig{
                                     .sampleRateHz = global.sampleRate,
                                     .clockSelect = global.clockSelect,
                                 },
                             .runtimeCaps = caps,
                             .notification = global.notification,
                             .status = global.status,
                             .extStatus = global.extStatus,
                         });
            });
    });
}

void DICETcatProtocol::PrepareDuplex48k(const AudioDuplexChannels& channels, VoidCallback callback) {
    PrepareDuplex(channels,
                  DiceDesiredClockConfig{
                      .sampleRateHz = 48000U,
                      .clockSelect = kDiceClockSelect48kInternal,
                  },
                  [callback = std::move(callback)](IOReturn status, DiceDuplexPrepareResult) mutable {
                      callback(status);
                  });
}

void DICETcatProtocol::ProgramRxForDuplex48k(VoidCallback callback) {
    ProgramRx([callback = std::move(callback)](IOReturn status, DiceDuplexStageResult) mutable {
        callback(status);
    });
}

void DICETcatProtocol::ProgramTxAndEnableDuplex48k(VoidCallback callback) {
    ProgramTxAndEnableDuplex([callback = std::move(callback)](IOReturn status, DiceDuplexStageResult) mutable {
        callback(status);
    });
}

void DICETcatProtocol::ConfirmDuplex48kStart(VoidCallback callback) {
    ConfirmDuplexStart([callback = std::move(callback)](IOReturn status, DiceDuplexConfirmResult) mutable {
        callback(status);
    });
}

IOReturn DICETcatProtocol::StopDuplex() {
    if (!duplexCtrl_) {
        return kIOReturnSuccess;
    }
    return duplexCtrl_->StopDuplex();
}

void DICETcatProtocol::UpdateRuntimeContext(uint16_t nodeId,
                                            Protocols::AVC::FCPTransport* transport) {
    (void)transport;
    io_.SetNodeId(nodeId);
}

void DICETcatProtocol::EnsureSectionsLoaded(VoidCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady);
        return;
    }

    if (sectionsLoaded_) {
        callback(kIOReturnSuccess);
        return;
    }

    diceReader_.ReadGeneralSections([this, callback = std::move(callback)](IOReturn status, GeneralSections sections) mutable {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }

        sections_ = sections;
        sectionsLoaded_ = true;
        callback(kIOReturnSuccess);
    });
}

void DICETcatProtocol::EnsureRuntimeCapsLoaded(VoidCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady);
        return;
    }

    if (runtimeCapsValid_.load(std::memory_order_acquire)) {
        callback(kIOReturnSuccess);
        return;
    }

    EnsureSectionsLoaded([this, callback = std::move(callback)](IOReturn sectionStatus) mutable {
        if (sectionStatus != kIOReturnSuccess) {
            callback(sectionStatus);
            return;
        }

        struct RuntimeCapsState {
            GlobalState global;
            StreamConfig tx;
            StreamConfig rx;
        };

        auto state = std::make_shared<RuntimeCapsState>();
        diceReader_.ReadGlobalState(
            sections_,
            [this, state, callback = std::move(callback)](IOReturn globalStatus, GlobalState global) mutable {
                if (globalStatus != kIOReturnSuccess) {
                    callback(globalStatus);
                    return;
                }

                state->global = global;
                diceReader_.ReadTxStreamConfig(
                    sections_,
                    [this, state, callback = std::move(callback)](IOReturn txStatus, StreamConfig tx) mutable {
                        if (txStatus != kIOReturnSuccess) {
                            callback(txStatus);
                            return;
                        }

                        state->tx = tx;
                        diceReader_.ReadRxStreamConfig(
                            sections_,
                            [this, state, callback = std::move(callback)](IOReturn rxStatus, StreamConfig rx) mutable {
                                if (rxStatus != kIOReturnSuccess) {
                                    callback(rxStatus);
                                    return;
                                }

                                state->rx = rx;
                                CacheRuntimeCaps(state->global, state->tx, state->rx);
                                callback(kIOReturnSuccess);
                            });
                    });
            });
    });
}

void DICETcatProtocol::CacheRuntimeCaps(const GlobalState& global,
                                        const StreamConfig& tx,
                                        const StreamConfig& rx) noexcept {
    CacheRuntimeCaps(AudioStreamRuntimeCaps{
        .hostInputPcmChannels = tx.ActivePcmChannels(),
        .hostOutputPcmChannels = rx.ActivePcmChannels(),
        .deviceToHostAm824Slots = tx.ActiveAm824Slots(),
        .hostToDeviceAm824Slots = rx.ActiveAm824Slots(),
        .deviceToHostActiveStreams = tx.ActiveStreamCount(),
        .hostToDeviceActiveStreams = rx.ActiveStreamCount(),
        .sampleRateHz = global.sampleRate,
        .deviceToHostIsoChannel = tx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel),
        .hostToDeviceIsoChannel = rx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel),
    });
}

void DICETcatProtocol::CacheRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept {
    hostInputPcmChannels_.store(caps.hostInputPcmChannels, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(caps.deviceToHostAm824Slots, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(caps.hostOutputPcmChannels, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(caps.hostToDeviceAm824Slots, std::memory_order_relaxed);
    deviceToHostActiveStreams_.store(caps.deviceToHostActiveStreams, std::memory_order_relaxed);
    hostToDeviceActiveStreams_.store(caps.hostToDeviceActiveStreams, std::memory_order_relaxed);
    runtimeSampleRateHz_.store(caps.sampleRateHz, std::memory_order_relaxed);
    deviceToHostIsoChannel_.store(caps.deviceToHostIsoChannel, std::memory_order_relaxed);
    hostToDeviceIsoChannel_.store(caps.hostToDeviceIsoChannel, std::memory_order_relaxed);
    runtimeCapsValid_.store(true, std::memory_order_release);
}

void DICETcatProtocol::ResetRuntimeCaps() noexcept {
    runtimeCapsValid_.store(false, std::memory_order_release);
    runtimeSampleRateHz_.store(0, std::memory_order_relaxed);
    hostInputPcmChannels_.store(0, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(0, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(0, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(0, std::memory_order_relaxed);
    deviceToHostActiveStreams_.store(0, std::memory_order_relaxed);
    hostToDeviceActiveStreams_.store(0, std::memory_order_relaxed);
    deviceToHostIsoChannel_.store(AudioStreamRuntimeCaps::kInvalidIsoChannel, std::memory_order_relaxed);
    hostToDeviceIsoChannel_.store(AudioStreamRuntimeCaps::kInvalidIsoChannel, std::memory_order_relaxed);
}

} // namespace ASFW::Audio::DICE::TCAT
