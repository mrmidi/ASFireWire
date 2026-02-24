// IsochAudioRxPipeline.cpp

#include "IsochAudioRxPipeline.hpp"

namespace ASFW::Isoch::Rx {

void IsochAudioRxPipeline::ConfigureFor48k() noexcept {
    cycleCorr_ = {};
    cycleCorr_.sampleRate = 48000.0;
    (void)ASFW::Timing::initializeHostTimebase();
}

void IsochAudioRxPipeline::OnStart() noexcept {
    streamProcessor_.Reset();

    if (externalSyncBridge_) {
        externalSyncBridge_->Reset();
        externalSyncBridge_->active.store(true, std::memory_order_release);
    }
    externalSyncClockState_.Reset();
}

void IsochAudioRxPipeline::OnStop() noexcept {
    if (::ASFW::LogConfig::Shared().GetIsochVerbosity() >= 3) {
        streamProcessor_.LogStatistics();
    }

    if (externalSyncBridge_) {
        externalSyncBridge_->Reset();
    }
    externalSyncClockState_.Reset();
}

void IsochAudioRxPipeline::OnPacket(const uint8_t* payload, size_t length) noexcept {
    constexpr bool kNullProcessing = false;

    if constexpr (kNullProcessing) {
        streamProcessor_.RecordRawPacket(length);
        return;
    }

    const auto summary = streamProcessor_.ProcessPacket(payload, length);

    if (!externalSyncBridge_) {
        return;
    }

    if (!summary.hasValidCip) {
        externalSyncClockState_.Reset();
        return;
    }

    uint32_t updateSeq = 0;
    const uint64_t nowTicks = mach_absolute_time();
    const bool establishTransition = externalSyncClockState_.ObserveSample(*externalSyncBridge_,
                                                                          nowTicks,
                                                                          summary.syt,
                                                                          summary.fdf,
                                                                          summary.dbs,
                                                                          &updateSeq);
    if (establishTransition) {
        ASFW_LOG(Isoch, "IR SYT CLOCK ESTABLISHED syt=0x%04x fdf=0x%02x dbs=%u seq=%u",
                 summary.syt, summary.fdf, summary.dbs, updateSeq);
        externalSyncBridge_->clockEstablished.store(true, std::memory_order_release);
    }
}

void IsochAudioRxPipeline::OnPollEnd(Driver::HardwareInterface& hw,
                                    uint32_t packetsProcessed,
                                    uint64_t pollStartMachTicks) noexcept {
    if (packetsProcessed > 0) {
        const uint64_t end = mach_absolute_time();
        const uint64_t deltaTicks = end - pollStartMachTicks;
        const uint64_t deltaUs = ASFW::Diagnostics::MachTicksToMicroseconds(deltaTicks);
        streamProcessor_.RecordPollLatency(deltaUs, packetsProcessed);
    }

    // Periodic cycle-time rate estimation (~1 second intervals, assuming 1kHz Poll cadence).
    cycleCorr_.pollsSinceLastUpdate++;
    if (cycleCorr_.pollsSinceLastUpdate >= 1000) {
        auto [ct, up] = hw.ReadCycleTimeAndUpTime();
        if (cycleCorr_.hasPrevious) {
            const int64_t dFW = ASFW::Timing::deltaFWTimeNanos(ct, cycleCorr_.prevCycleTimer);
            const int64_t dHost = static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(up))
                                - static_cast<int64_t>(ASFW::Timing::hostTicksToNanos(cycleCorr_.prevHostTicks));
            ASFW_LOG_V3(Isoch, "CycleCorr: ct=0x%08x prev=0x%08x dFW=%lld dHost=%lld",
                        ct, cycleCorr_.prevCycleTimer, dFW, dHost);
            if (dFW > 0 && dHost > 0) {
                const double ratio = static_cast<double>(dHost) / static_cast<double>(dFW);
                const double nanosPerSample = ratio * (1e9 / cycleCorr_.sampleRate);
                const uint32_t q8 = static_cast<uint32_t>(nanosPerSample * 256.0 + 0.5);
                rxSharedQueue_.SetCorrHostNanosPerSampleQ8(q8);
                ASFW_LOG_V3(Isoch, "CycleCorr: ratio=%.6f nanosPerSample=%.1f q8=%u",
                            ratio, nanosPerSample, q8);
            }
        } else {
            ASFW_LOG_V3(Isoch, "CycleCorr: baseline ct=0x%08x up=%llu", ct, up);
        }
        cycleCorr_.prevCycleTimer = ct;
        cycleCorr_.prevHostTicks = up;
        cycleCorr_.hasPrevious = true;
        cycleCorr_.pollsSinceLastUpdate = 0;
    }

    if (externalSyncBridge_) {
        uint64_t staleTicks = ASFW::Timing::nanosToHostTicks(kExternalSyncStaleNanos);
        if (staleTicks == 0 && ASFW::Timing::initializeHostTimebase()) {
            staleTicks = ASFW::Timing::nanosToHostTicks(kExternalSyncStaleNanos);
        }
        (void)externalSyncClockState_.HandleStale(*externalSyncBridge_,
                                                  mach_absolute_time(),
                                                  staleTicks);
    }
}

void IsochAudioRxPipeline::SetSharedRxQueue(void* base, uint64_t bytes) noexcept {
    if (!base || bytes == 0) {
        (void)rxSharedQueue_.Attach(nullptr, 0);
        streamProcessor_.SetOutputSharedQueue(nullptr);
        ASFW_LOG(Isoch, "[Isoch] IR: Shared RX queue detached");
        return;
    }

    if (rxSharedQueue_.Attach(base, bytes)) {
        streamProcessor_.SetOutputSharedQueue(&rxSharedQueue_);
        ASFW_LOG(Isoch, "[Isoch] IR: Shared RX queue attached (%llu bytes)", bytes);
    } else {
        ASFW_LOG(Isoch, "[Isoch] IR: Failed to attach shared RX queue (base=%p bytes=%llu)", base, bytes);
        (void)rxSharedQueue_.Attach(nullptr, 0);
        streamProcessor_.SetOutputSharedQueue(nullptr);
    }
}

void IsochAudioRxPipeline::SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept {
    externalSyncBridge_ = bridge;
    externalSyncClockState_.Reset();
    if (externalSyncBridge_) {
        externalSyncBridge_->Reset();
    }
}

} // namespace ASFW::Isoch::Rx
