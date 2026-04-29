// IsochAudioRxPipeline.cpp

#include "IsochAudioRxPipeline.hpp"

#include <utility>

namespace ASFW::Isoch::Rx {

void IsochAudioRxPipeline::PublishTransportTimingAnchor(uint64_t hostTicks,
                                                        bool fromCycleCorrelation) noexcept {
    if (!externalSyncBridge_) {
        return;
    }

    externalSyncBridge_->PublishTransportTiming(transportSampleFrame_,
                                                hostTicks,
                                                transportHostNanosPerSampleQ8_);
    if (rxSharedQueue_.IsValid()) {
        rxSharedQueue_.SetTransportTiming(transportSampleFrame_,
                                          hostTicks,
                                          transportHostNanosPerSampleQ8_);
    }
    if (::ASFW::LogConfig::Shared().GetIsochVerbosity() >= 3 &&
        (++transportTimingPublishCount_ % 1000U) == 0U) {
        ASFW_LOG(Isoch,
                 "IR transport timing anchor sample=%llu host=%llu q8=%u source=%{public}s seq=%u",
                 transportSampleFrame_,
                 hostTicks,
                 transportHostNanosPerSampleQ8_,
                 fromCycleCorrelation ? "cycle" : "packet",
                 externalSyncBridge_->ReadTransportTiming().seq);
    }
}

void IsochAudioRxPipeline::ConfigureFor48k() noexcept {
    cycleCorr_ = {};
    cycleCorr_.sampleRate = 48000.0;
    transportHostNanosPerSampleQ8_ = 0;
    transportTimingPublishCount_ = 0;
    (void)ASFW::Timing::initializeHostTimebase();
}

void IsochAudioRxPipeline::OnStart() noexcept {
    streamProcessor_.Reset();
    transportSampleFrame_ = 0;
    transportHostNanosPerSampleQ8_ = 0;
    transportTimingPublishCount_ = 0;
    rxHealthPollCounter_ = 0;
    sytClockLostCount_.store(0, std::memory_order_release);
    rxSharedQueue_.ResetTransportTiming();
    rxSharedQueue_.ResetStartupAlignment();
    rxSharedQueue_.ResetConsumerReadDiagnostics();

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
    rxSharedQueue_.ResetTransportTiming();
    rxSharedQueue_.ResetStartupAlignment();
    externalSyncClockState_.Reset();
}

void IsochAudioRxPipeline::OnPacket(const uint8_t* payload, size_t length) noexcept {
    constexpr bool kNullProcessing = false;

    if constexpr (kNullProcessing) {
        streamProcessor_.RecordRawPacket(length);
        return;
    }

    const auto summary = streamProcessor_.ProcessPacket(payload, length);
    transportSampleFrame_ += summary.decodedFrames;

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
    PublishTransportTimingAnchor(nowTicks, false);
    if (establishTransition) {
        ASFW_LOG(Isoch, "IR SYT CLOCK ESTABLISHED syt=0x%04x fdf=0x%02x dbs=%u seq=%u",
                 summary.syt, summary.fdf, summary.dbs, updateSeq);
        externalSyncBridge_->startupQualified.store(true, std::memory_order_release);
        externalSyncBridge_->clockEstablished.store(true, std::memory_order_release);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void IsochAudioRxPipeline::OnPollEnd(Driver::HardwareInterface& hw,
                                     uint32_t packetsProcessed, // NOLINT(bugprone-easily-swappable-parameters)
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
                transportHostNanosPerSampleQ8_ = q8;
                rxSharedQueue_.SetCorrHostNanosPerSampleQ8(q8);
                PublishTransportTimingAnchor(up, true);
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
        const uint64_t nowTicks = mach_absolute_time();
        if (externalSyncClockState_.HandleStale(*externalSyncBridge_, nowTicks, staleTicks)) {
            sytClockLostCount_.fetch_add(1, std::memory_order_acq_rel);
            const uint64_t lastTicks =
                externalSyncBridge_->lastUpdateHostTicks.load(std::memory_order_acquire);
            const uint64_t staleForMs =
                (nowTicks >= lastTicks)
                    ? (ASFW::Timing::hostTicksToNanos(nowTicks - lastTicks) / 1'000'000ULL)
                    : 0;

            ASFW_LOG_WARNING(Isoch,
                             "IR SYT CLOCK LOST staleFor=%llums threshold=%llums",
                             staleForMs,
                             kExternalSyncStaleNanos / 1'000'000ULL);
            if (timingLossCallback_) {
                timingLossCallback_();
            }
        }
    }

    if (++rxHealthPollCounter_ >= 1000U) {
        rxHealthPollCounter_ = 0;
        const uint32_t queueFill = rxSharedQueue_.IsValid() ? rxSharedQueue_.FillLevelFrames() : 0;
        const uint32_t queueCap = rxSharedQueue_.IsValid() ? rxSharedQueue_.CapacityFrames() : 0;
        const uint64_t underreadEvents =
            rxSharedQueue_.IsValid() ? rxSharedQueue_.ConsumerUnderreadEvents() : 0;
        const uint64_t underreadFrames =
            rxSharedQueue_.IsValid() ? rxSharedQueue_.ConsumerUnderreadFrames() : 0;
        ASFW_LOG(Isoch,
                 "IR RX HEALTH pkts=%llu data=%llu empty=%llu decoded=%llu drops=%llu errors=%llu queue=%u/%u producerDrop=%llu/%llu consumerUnder=%llu/%llu lastPoll=%u/%uus cip(dbs=%u dbc=0x%02x syt=0x%04x fdf=0x%02x) q8=%u sytLost=%llu",
                 streamProcessor_.PacketCount(),
                 streamProcessor_.SamplePacketCount(),
                 streamProcessor_.EmptyPacketCount(),
                 streamProcessor_.DecodedFrameCount(),
                 streamProcessor_.DiscontinuityCount(),
                 streamProcessor_.ErrorCount(),
                 queueFill,
                 queueCap,
                 streamProcessor_.RxQueueProducerDropEvents(),
                 streamProcessor_.RxQueueProducerDropFrames(),
                 underreadEvents,
                 underreadFrames,
                 streamProcessor_.LastPollPackets(),
                 streamProcessor_.LastPollLatencyUs(),
                 streamProcessor_.LastCipDBS(),
                 streamProcessor_.LastDBC(),
                 streamProcessor_.LastSYT(),
                 streamProcessor_.LastCipFDF(),
                 rxSharedQueue_.IsValid() ? rxSharedQueue_.CorrHostNanosPerSampleQ8() : 0,
                 sytClockLostCount_.load(std::memory_order_acquire));
    }
}

void IsochAudioRxPipeline::SetSharedRxQueue(uint8_t* base, uint64_t bytes) noexcept {
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
        ASFW_LOG(Isoch, "[Isoch] IR: Failed to attach shared RX queue (base=%p bytes=%llu)",
                 static_cast<const void*>(base), bytes);
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

void IsochAudioRxPipeline::SetTimingLossCallback(TimingLossCallback callback) noexcept {
    timingLossCallback_ = std::move(callback);
}

IsochAudioRxPipeline::ClockHealthSnapshot IsochAudioRxPipeline::ReadClockHealth() const noexcept {
    ClockHealthSnapshot snapshot{};
    snapshot.lostCount = sytClockLostCount_.load(std::memory_order_acquire);
    if (!externalSyncBridge_) {
        return snapshot;
    }

    const uint32_t packed = externalSyncBridge_->lastPackedRx.load(std::memory_order_acquire);
    snapshot.active = externalSyncBridge_->active.load(std::memory_order_acquire);
    snapshot.clockEstablished =
        externalSyncBridge_->clockEstablished.load(std::memory_order_acquire);
    snapshot.startupQualified =
        externalSyncBridge_->startupQualified.load(std::memory_order_acquire);
    snapshot.updateSeq = externalSyncBridge_->updateSeq.load(std::memory_order_acquire);
    snapshot.lastSyt = Core::ExternalSyncBridge::UnpackSYT(packed);
    snapshot.lastFdf = Core::ExternalSyncBridge::UnpackFDF(packed);
    snapshot.lastDbs = Core::ExternalSyncBridge::UnpackDBS(packed);
    return snapshot;
}

} // namespace ASFW::Isoch::Rx
