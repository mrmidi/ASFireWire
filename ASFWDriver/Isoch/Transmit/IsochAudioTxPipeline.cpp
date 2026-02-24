// IsochAudioTxPipeline.cpp

#include "IsochAudioTxPipeline.hpp"

#include "../Encoding/TimingUtils.hpp"

namespace ASFW::Isoch {

namespace {

inline uint32_t EncodeMidiPlaceholderSlot(uint32_t midiSlotIndex) noexcept {
    const uint8_t label = static_cast<uint8_t>(
        Encoding::kAM824LabelMIDIConformantBase + (midiSlotIndex & 0x03u));
    return Encoding::AM824Encoder::encodeLabelOnly(label);
}

inline void EncodePcmFramesWithAm824Placeholders(const int32_t* pcmInterleaved,
                                                 uint32_t frames,
                                                 uint32_t pcmChannels,
                                                 uint32_t am824Slots,
                                                 uint32_t* outWireQuadlets) noexcept {
    const uint32_t midiSlots = (am824Slots > pcmChannels) ? (am824Slots - pcmChannels) : 0;
    for (uint32_t f = 0; f < frames; ++f) {
        const int32_t* frameIn = pcmInterleaved + (f * pcmChannels);
        uint32_t* frameOut = outWireQuadlets + (f * am824Slots);

        for (uint32_t ch = 0; ch < pcmChannels; ++ch) {
            frameOut[ch] = Encoding::AM824Encoder::encode(frameIn[ch]);
        }
        for (uint32_t s = 0; s < midiSlots; ++s) {
            frameOut[pcmChannels + s] = EncodeMidiPlaceholderSlot(s);
        }
    }
}

} // namespace

void IsochAudioTxPipeline::SetSharedTxQueue(void* base, uint64_t bytes) noexcept {
    if (!base || bytes == 0) {
        // Treat null/0 as an explicit detach so callers can safely tear down the
        // underlying mapping without leaving stale pointers behind.
        (void)sharedTxQueue_.Attach(nullptr, 0);
        ASFW_LOG(Isoch, "IT: Shared TX queue detached");
        return;
    }

    if (sharedTxQueue_.Attach(base, bytes)) {
        // Consumer-owned flush only: drop stale backlog on (re)attach.
        sharedTxQueue_.ConsumerDropQueuedFrames();
        ASFW_LOG(Isoch, "IT: Shared TX queue attached - capacity=%u frames",
                 sharedTxQueue_.CapacityFrames());
    } else {
        ASFW_LOG(Isoch, "IT: Failed to attach shared TX queue - invalid header?");
        (void)sharedTxQueue_.Attach(nullptr, 0);
    }
}

uint32_t IsochAudioTxPipeline::SharedTxFillLevelFrames() const noexcept {
    if (!sharedTxQueue_.IsValid()) return 0;
    return sharedTxQueue_.FillLevelFrames();
}

uint32_t IsochAudioTxPipeline::SharedTxCapacityFrames() const noexcept {
    if (!sharedTxQueue_.IsValid()) return 0;
    return sharedTxQueue_.CapacityFrames();
}

void IsochAudioTxPipeline::SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept {
    externalSyncBridge_ = bridge;
    externalSyncDiscipline_.Reset();
}

void IsochAudioTxPipeline::SetZeroCopyOutputBuffer(void* base, uint64_t bytes, uint32_t frameCapacity) noexcept {
    if (!base || bytes == 0 || frameCapacity == 0) {
        zeroCopyAudioBase_ = nullptr;
        zeroCopyAudioBytes_ = 0;
        zeroCopyFrameCapacity_ = 0;
        zeroCopyEnabled_ = false;
        assembler_.setZeroCopySource(nullptr, 0);

        if (base || bytes || frameCapacity) {
            ASFW_LOG(Isoch, "IT: SetZeroCopyOutputBuffer - invalid parameters");
        } else {
            ASFW_LOG(Isoch, "IT: ZERO-COPY disabled; using shared TX queue");
        }
        return;
    }

    zeroCopyAudioBase_ = base;
    zeroCopyAudioBytes_ = bytes;
    zeroCopyFrameCapacity_ = frameCapacity;
    zeroCopyEnabled_ = true;

    assembler_.setZeroCopySource(reinterpret_cast<const int32_t*>(base), frameCapacity);

    ASFW_LOG(Isoch, "IT: ✅ ZERO-COPY enabled! AudioBuffer base=%p bytes=%llu frames=%u assembler=%{public}s",
             base, bytes, frameCapacity,
             assembler_.isZeroCopyEnabled() ? "ENABLED" : "fallback");
}

kern_return_t IsochAudioTxPipeline::Configure(uint8_t sid,
                                              uint32_t streamModeRaw,
                                              uint32_t requestedChannels,
                                              uint32_t requestedAm824Slots) noexcept {
    if (!sharedTxQueue_.IsValid()) {
        ASFW_LOG(Isoch, "IT: Configure failed - shared TX queue missing");
        return kIOReturnNotReady;
    }

    const uint32_t queueChannels = sharedTxQueue_.Channels();
    if (queueChannels == 0 || queueChannels > Encoding::kMaxSupportedChannels) {
        ASFW_LOG(Isoch, "IT: Configure failed - invalid queueChannels=%u", queueChannels);
        return kIOReturnBadArgument;
    }
    if (requestedChannels != 0 && requestedChannels != queueChannels) {
        ASFW_LOG(Isoch, "IT: Configure failed - requestedChannels=%u queueChannels=%u mismatch",
                 requestedChannels, queueChannels);
        return kIOReturnBadArgument;
    }

    uint32_t am824Slots = queueChannels;
    if (requestedAm824Slots != 0) {
        if (requestedAm824Slots < queueChannels) {
            ASFW_LOG(Isoch,
                     "IT: Configure failed - requestedAm824Slots=%u < queuePcm=%u",
                     requestedAm824Slots,
                     queueChannels);
            return kIOReturnBadArgument;
        }
        if (requestedAm824Slots > Encoding::kMaxSupportedAm824Slots) {
            ASFW_LOG(Isoch,
                     "IT: Configure failed - requestedAm824Slots=%u exceed max supported=%u (pcm=%u)",
                     requestedAm824Slots,
                     Encoding::kMaxSupportedAm824Slots,
                     queueChannels);
            return kIOReturnUnsupported;
        }
        am824Slots = requestedAm824Slots;
    }

    assembler_.reconfigureAM824(queueChannels, am824Slots, sid);

    requestedStreamMode_ = (streamModeRaw == 1u)
        ? Encoding::StreamMode::kBlocking
        : Encoding::StreamMode::kNonBlocking;
    effectiveStreamMode_ = requestedStreamMode_;
    assembler_.setStreamMode(effectiveStreamMode_);

    ASFW_LOG(Isoch, "IT: Stream mode resolved requested=%{public}s effective=%{public}s",
             requestedStreamMode_ == Encoding::StreamMode::kBlocking ? "blocking" : "non-blocking",
             effectiveStreamMode_ == Encoding::StreamMode::kBlocking ? "blocking" : "non-blocking");

    const uint32_t framesPerDataPacket = assembler_.samplesPerDataPacket();
    const uint32_t payloadBytes = framesPerDataPacket * am824Slots * sizeof(uint32_t);
    const uint32_t packetBytes = Encoding::kCIPHeaderSize + payloadBytes;
    ASFW_LOG(Isoch,
             "IT: Channel geometry resolved pcm=%u dbs=%u midiSlots=%u framesPerData=%u payloadBytes=%u packetBytes=%u",
             queueChannels, am824Slots, (am824Slots > queueChannels) ? (am824Slots - queueChannels) : 0,
             framesPerDataPacket, payloadBytes, packetBytes);
    ASFW_LOG(Isoch,
             "IT: Cadence resolved mode=%{public}s dbs=%u framesPerData=%u dataBytes=%u noDataBytes=%u cadence=%{public}s",
             effectiveStreamMode_ == Encoding::StreamMode::kBlocking ? "blocking" : "non-blocking",
             am824Slots,
             framesPerDataPacket,
             packetBytes,
             Encoding::kCIPHeaderSize,
             effectiveStreamMode_ == Encoding::StreamMode::kBlocking ? "NDDD" : "DATA-every-cycle");

    return kIOReturnSuccess;
}

void IsochAudioTxPipeline::ResetForStart() noexcept {
    assembler_.reset();
    externalSyncDiscipline_.Reset();

    counters_.resyncApplied.store(0, std::memory_order_relaxed);
    counters_.staleFramesDropped.store(0, std::memory_order_relaxed);
    counters_.legacyPumpMovedFrames.store(0, std::memory_order_relaxed);
    counters_.legacyPumpSkipped.store(0, std::memory_order_relaxed);
    counters_.exitZeroRefill.store(0, std::memory_order_relaxed);
    counters_.underrunSilencedPackets.store(0, std::memory_order_relaxed);
    counters_.audioInjectCursorResets.store(0, std::memory_order_relaxed);
    counters_.audioInjectMissedPackets.store(0, std::memory_order_relaxed);
    counters_.rbLowEvents.store(0, std::memory_order_relaxed);
    counters_.txqLowEvents.store(0, std::memory_order_relaxed);

    fillLevelAlert_ = {};

    adaptiveFill_.baseTarget = Config::kTxBufferProfile.legacyRbTargetFrames;
    adaptiveFill_.currentTarget = adaptiveFill_.baseTarget;
    adaptiveFill_.maxTarget = adaptiveFill_.baseTarget * 4;
    adaptiveFill_.underrunsInWindow = 0;
    adaptiveFill_.windowTickCount = 0;
    adaptiveFill_.cleanWindows = 0;
    adaptiveFill_.lastCombinedUnderruns = 0;

    audioWriteIndex_ = 0;

    dbcTracker_.lastDbc = 0;
    dbcTracker_.lastDataBlockCount = 0;
    dbcTracker_.firstPacket = true;
    dbcTracker_.discontinuityCount.store(0, std::memory_order_relaxed);

    // SYT generator (cycle-based, Linux approach). TODO: derive rate from stream formats.
    sytGenerator_.initialize(48000.0);
    sytGenerator_.reset();
    cycleTrackingValid_ = false;
}

void IsochAudioTxPipeline::PrePrimeFromSharedQueue() noexcept {
    if (!sharedTxQueue_.IsValid() || zeroCopyEnabled_) {
        if (zeroCopyEnabled_) {
            ASFW_LOG(Isoch, "IT: Pre-prime skipped (ZERO-COPY mode)");
        }
        return;
    }

    const uint32_t fillBefore = sharedTxQueue_.FillLevelFrames();
    const uint32_t startupPrimeLimitFrames = Config::kTxBufferProfile.startupPrimeLimitFrames;
    uint32_t remainingPrimeFrames = startupPrimeLimitFrames;
    ASFW_LOG(Isoch, "IT: Pre-prime transfer - shared queue has %u frames (limit=%u)",
             fillBefore, startupPrimeLimitFrames);

    constexpr uint32_t kTransferChunk = Config::kTransferChunkFrames;
    int32_t transferBuf[kTransferChunk * Encoding::kMaxSupportedChannels];
    uint32_t totalTransferred = 0;
    uint32_t chunkCount = 0;
    bool primeLimitHit = false;

    while (sharedTxQueue_.FillLevelFrames() > 0) {
        if (startupPrimeLimitFrames != 0 && remainingPrimeFrames == 0) {
            primeLimitHit = true;
            break;
        }

        uint32_t toRead = sharedTxQueue_.FillLevelFrames();
        if (toRead > kTransferChunk) toRead = kTransferChunk;
        if (startupPrimeLimitFrames != 0 && toRead > remainingPrimeFrames) {
            toRead = remainingPrimeFrames;
        }

        const uint32_t read = sharedTxQueue_.Read(transferBuf, toRead);
        if (read == 0) break;

        if (chunkCount < 3) {
            ASFW_LOG(Isoch, "IT: SharedQ chunk[%u] read=%u samples=[%08x,%08x,%08x,%08x]",
                     chunkCount, read,
                     static_cast<uint32_t>(transferBuf[0]),
                     static_cast<uint32_t>(transferBuf[1]),
                     static_cast<uint32_t>(transferBuf[2]),
                     static_cast<uint32_t>(transferBuf[3]));
        }
        chunkCount++;

        const uint32_t written = assembler_.ringBuffer().write(transferBuf, read);
        totalTransferred += written;
        if (startupPrimeLimitFrames != 0) {
            remainingPrimeFrames = (written >= remainingPrimeFrames) ? 0 : (remainingPrimeFrames - written);
        }

        if (written < read) break;
    }

    ASFW_LOG(Isoch, "IT: Pre-prime transferred %u frames to assembler (fill=%u limit=%u hit=%{public}s)",
             totalTransferred,
             assembler_.bufferFillLevel(),
             startupPrimeLimitFrames,
             primeLimitHit ? "YES" : "NO");
}

void IsochAudioTxPipeline::OnRefillTickPreHW() noexcept {
    if (sharedTxQueue_.IsValid() && sharedTxQueue_.ConsumerApplyPendingResync()) {
        counters_.resyncApplied.fetch_add(1, std::memory_order_relaxed);
    }

    // Legacy (non-zero-copy) path: keep assembler ring near a target fill.
    if (!zeroCopyEnabled_ && sharedTxQueue_.IsValid()) {
        const uint32_t targetRbFillFrames = adaptiveFill_.currentTarget;
        constexpr uint32_t kMaxRbFillFrames = Config::kTxBufferProfile.legacyRbMaxFrames;
        constexpr uint32_t kTransferChunkFrames = Config::kTransferChunkFrames;
        constexpr uint32_t kMaxChunksPerRefill = Config::kTxBufferProfile.legacyMaxChunksPerRefill;

        const uint32_t rbFill = assembler_.bufferFillLevel();
        uint32_t pumpedFrames = 0;
        bool skipped = true;

        if (rbFill < targetRbFillFrames) {
            skipped = false;
            uint32_t want = targetRbFillFrames - rbFill;
            int32_t transferBuf[kTransferChunkFrames * Encoding::kMaxSupportedChannels];
            uint32_t chunks = 0;

            while (want > 0 && chunks < kMaxChunksPerRefill) {
                const uint32_t qFill = sharedTxQueue_.FillLevelFrames();
                if (qFill == 0) break;

                const uint32_t rbSpace = assembler_.ringBuffer().availableSpace();
                if (rbSpace == 0) break;

                uint32_t toRead = want;
                if (toRead > qFill) toRead = qFill;
                if (toRead > rbSpace) toRead = rbSpace;
                if (toRead > kTransferChunkFrames) toRead = kTransferChunkFrames;

                const uint32_t read = sharedTxQueue_.Read(transferBuf, toRead);
                if (read == 0) break;

                const uint32_t written = assembler_.ringBuffer().write(transferBuf, read);
                pumpedFrames += written;
                if (written < read) break;

                want -= written;
                ++chunks;

                if (assembler_.bufferFillLevel() >= kMaxRbFillFrames) break;
            }
        }

        if (skipped) {
            counters_.legacyPumpSkipped.fetch_add(1, std::memory_order_relaxed);
        } else {
            counters_.legacyPumpMovedFrames.fetch_add(pumpedFrames, std::memory_order_relaxed);
        }

        // Fill level threshold alerts (with hysteresis) - non-zero-copy only
        {
            const uint32_t rbCap = assembler_.ringBuffer().capacity();
            const uint32_t rbFillNow = assembler_.bufferFillLevel();
            const uint32_t rbLowThresh = rbCap / 20;     // 5%
            const uint32_t rbRecoverThresh = rbCap / 10;  // 10%

            if (!fillLevelAlert_.rbLow && rbFillNow < rbLowThresh) {
                fillLevelAlert_.rbLow = true;
                counters_.rbLowEvents.fetch_add(1, std::memory_order_relaxed);
            } else if (fillLevelAlert_.rbLow && rbFillNow >= rbRecoverThresh) {
                fillLevelAlert_.rbLow = false;
            }

            const uint32_t txqFill = sharedTxQueue_.FillLevelFrames();
            const uint32_t txqCap = sharedTxQueue_.CapacityFrames();
            const uint32_t txqLowThresh = txqCap / 20;     // 5%
            const uint32_t txqRecoverThresh = txqCap / 10;  // 10%

            if (!fillLevelAlert_.txqLow && txqFill < txqLowThresh) {
                fillLevelAlert_.txqLow = true;
                counters_.txqLowEvents.fetch_add(1, std::memory_order_relaxed);
            } else if (fillLevelAlert_.txqLow && txqFill >= txqRecoverThresh) {
                fillLevelAlert_.txqLow = false;
            }
        }
    }
}

void IsochAudioTxPipeline::OnPollTick1ms() noexcept {
    // Adaptive fill (1-second windows, non-zero-copy only)
    if (!zeroCopyEnabled_ && sharedTxQueue_.IsValid()) {
        ++adaptiveFill_.windowTickCount;

        const uint64_t curZeroRefills = counters_.exitZeroRefill.load(std::memory_order_relaxed);
        const uint64_t curAssemblerUnderruns = assembler_.underrunDiag().underrunCount.load(std::memory_order_relaxed);
        const uint64_t combinedUnderruns = curZeroRefills + curAssemblerUnderruns;
        if (combinedUnderruns > adaptiveFill_.lastCombinedUnderruns) {
            adaptiveFill_.underrunsInWindow += static_cast<uint32_t>(combinedUnderruns - adaptiveFill_.lastCombinedUnderruns);
            adaptiveFill_.lastCombinedUnderruns = combinedUnderruns;
        }

        if (adaptiveFill_.windowTickCount >= 1000) {
            if (adaptiveFill_.underrunsInWindow >= 3) {
                uint32_t newTarget = adaptiveFill_.currentTarget + 128;
                if (newTarget > adaptiveFill_.maxTarget) newTarget = adaptiveFill_.maxTarget;
                if (newTarget != adaptiveFill_.currentTarget) {
                    ASFW_LOG(Isoch, "IT: ADAPTIVE FILL ESCALATE %u -> %u (underruns=%u in window)",
                             adaptiveFill_.currentTarget, newTarget, adaptiveFill_.underrunsInWindow);
                    adaptiveFill_.currentTarget = newTarget;
                }
                adaptiveFill_.cleanWindows = 0;
            } else if (adaptiveFill_.underrunsInWindow == 0) {
                ++adaptiveFill_.cleanWindows;
                if (adaptiveFill_.cleanWindows >= 10 && adaptiveFill_.currentTarget > adaptiveFill_.baseTarget) {
                    uint32_t newTarget = adaptiveFill_.currentTarget;
                    newTarget = (newTarget > adaptiveFill_.baseTarget + 64)
                        ? newTarget - 64
                        : adaptiveFill_.baseTarget;
                    if (newTarget != adaptiveFill_.currentTarget) {
                        ASFW_LOG(Isoch, "IT: ADAPTIVE FILL DECAY %u -> %u (cleanWindows=%u)",
                                 adaptiveFill_.currentTarget, newTarget, adaptiveFill_.cleanWindows);
                        adaptiveFill_.currentTarget = newTarget;
                    }
                }
            } else {
                adaptiveFill_.cleanWindows = 0;
            }

            adaptiveFill_.windowTickCount = 0;
            adaptiveFill_.underrunsInWindow = 0;
        }
    }
}

uint16_t IsochAudioTxPipeline::ComputeDataSyt(uint32_t transmitCycle) noexcept {
    if (!sytGenerator_.isValid() || !cycleTrackingValid_) {
        return Encoding::SYTGenerator::kNoInfo;
    }

    const uint16_t txSyt = sytGenerator_.computeDataSYT(transmitCycle, assembler_.samplesPerDataPacket());
    MaybeApplyExternalSyncDiscipline(txSyt);
    return txSyt;
}

void IsochAudioTxPipeline::MaybeApplyExternalSyncDiscipline(uint16_t txSyt) noexcept {
    bool enabled = false;
    uint16_t rxSyt = Core::ExternalSyncBridge::kNoInfoSyt;

    if (externalSyncBridge_) {
        const bool active = externalSyncBridge_->active.load(std::memory_order_acquire);
        const bool established = externalSyncBridge_->clockEstablished.load(std::memory_order_acquire);
        const uint64_t lastUpdateTicks =
            externalSyncBridge_->lastUpdateHostTicks.load(std::memory_order_acquire);

        uint64_t staleThresholdTicks = ASFW::Timing::nanosToHostTicks(100'000'000ULL);
        if (staleThresholdTicks == 0 && ASFW::Timing::initializeHostTimebase()) {
            staleThresholdTicks = ASFW::Timing::nanosToHostTicks(100'000'000ULL);
        }

        if (active && established && staleThresholdTicks != 0 && lastUpdateTicks != 0) {
            const uint64_t nowTicks = mach_absolute_time();
            if (nowTicks >= lastUpdateTicks &&
                (nowTicks - lastUpdateTicks) <= staleThresholdTicks) {
                const uint32_t packed = externalSyncBridge_->lastPackedRx.load(std::memory_order_acquire);
                const uint16_t candidateSyt = Core::ExternalSyncBridge::UnpackSYT(packed);
                const uint8_t candidateFdf = Core::ExternalSyncBridge::UnpackFDF(packed);
                if (candidateSyt != Core::ExternalSyncBridge::kNoInfoSyt &&
                    candidateFdf == Core::ExternalSyncBridge::kFdf48k) {
                    enabled = true;
                    rxSyt = candidateSyt;
                }
            }
        }
    }

    const auto disciplineResult = externalSyncDiscipline_.Update(enabled, txSyt, rxSyt);
    if (enabled && disciplineResult.correctionTicks != 0) {
        sytGenerator_.nudgeOffsetTicks(disciplineResult.correctionTicks);
    }
}

Tx::IsochTxPacket IsochAudioTxPipeline::NextSilentPacket(uint32_t transmitCycle) noexcept {
    uint16_t syt = Encoding::SYTGenerator::kNoInfo;
    const bool willBeData = assembler_.nextIsData();
    if (willBeData) {
        syt = ComputeDataSyt(transmitCycle);
    }

    // silent=true: cadence/DBC/CIP advance, audio payload is valid AM824 silence.
    auto pkt = assembler_.assembleNext(syt, /*silent=*/true);

    // Producer-side DBC continuity validation (ignore NO-DATA).
    if (pkt.isData) {
        const uint8_t samplesInPkt = static_cast<uint8_t>(assembler_.samplesPerDataPacket());
        if (!dbcTracker_.firstPacket) {
            const uint8_t expectedDbc = static_cast<uint8_t>(dbcTracker_.lastDbc + dbcTracker_.lastDataBlockCount);
            if (pkt.dbc != expectedDbc) {
                dbcTracker_.discontinuityCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
        dbcTracker_.lastDbc = pkt.dbc;
        dbcTracker_.lastDataBlockCount = samplesInPkt;
        dbcTracker_.firstPacket = false;
    }

    Tx::IsochTxPacket out{};
    out.words = reinterpret_cast<const uint32_t*>(pkt.data);
    out.sizeBytes = pkt.size;
    out.isData = pkt.isData;
    out.dbc = pkt.dbc;
    return out;
}

void IsochAudioTxPipeline::InjectNearHw(uint32_t hwPacketIndex, Tx::IsochTxDescriptorSlab& slab) noexcept {
    constexpr uint32_t numPackets = Tx::Layout::kNumPackets;

    const bool zeroCopySync = zeroCopyEnabled_ && sharedTxQueue_.IsValid() && zeroCopyFrameCapacity_ > 0;

    // Target: write real audio up to kAudioWriteAhead packets ahead of HW
    const uint32_t audioTarget = (hwPacketIndex + Tx::Layout::kAudioWriteAhead) % numPackets;

    // If audio cursor fell behind HW (scheduling stall), reset to HW position.
    const uint32_t distBehind = (hwPacketIndex + numPackets - audioWriteIndex_) % numPackets;
    if (distBehind > 0 && distBehind < numPackets / 2) {
        counters_.audioInjectCursorResets.fetch_add(1, std::memory_order_relaxed);
        counters_.audioInjectMissedPackets.fetch_add(distBehind, std::memory_order_relaxed);
        audioWriteIndex_ = hwPacketIndex;
    }

    uint32_t toInject = (audioTarget + numPackets - audioWriteIndex_) % numPackets;
    if (toInject > Tx::Layout::kAudioWriteAhead) toInject = Tx::Layout::kAudioWriteAhead;

    if (toInject == 0) {
        return;
    }

    const uint32_t framesPerPacket = assembler_.samplesPerDataPacket();
    const uint32_t pcmChannels = assembler_.channelCount();
    const uint32_t am824Slots = assembler_.am824SlotCount();

    for (uint32_t i = 0; i < toInject; ++i) {
        const uint32_t idx = (audioWriteIndex_ + i) % numPackets;

        const uint32_t descBase = idx * Tx::Layout::kBlocksPerPacket;
        auto* lastDesc = slab.GetDescriptorPtr(descBase + 2);
        const uint16_t reqCount = static_cast<uint16_t>(lastDesc->control & 0xFFFF);
        const bool isData = (reqCount > Encoding::kCIPHeaderSize);
        if (!isData) continue;

        int32_t samples[Encoding::kSamplesPerDataPacket * Encoding::kMaxSupportedChannels];
        uint32_t framesRead = 0;

        if (zeroCopySync) {
            uint32_t zeroCopyFillBefore = sharedTxQueue_.FillLevelFrames();

            // Drop stale backlog if queue lag exceeds buffer capacity
            if (zeroCopyFillBefore > zeroCopyFrameCapacity_) {
                const uint32_t drop = zeroCopyFillBefore - zeroCopyFrameCapacity_;
                const uint32_t dropped = sharedTxQueue_.ConsumeFrames(drop);
                counters_.staleFramesDropped.fetch_add(dropped, std::memory_order_relaxed);
                zeroCopyFillBefore -= dropped;
            }

            const uint32_t readAbs = sharedTxQueue_.ReadIndexFrames();
            const uint32_t phase = sharedTxQueue_.ZeroCopyPhaseFrames() % zeroCopyFrameCapacity_;
            assembler_.setZeroCopyReadPosition((readAbs + phase) % zeroCopyFrameCapacity_);

            if (assembler_.isZeroCopyEnabled() && zeroCopyAudioBase_) {
                const int32_t* zcBase = reinterpret_cast<const int32_t*>(zeroCopyAudioBase_);
                const uint32_t zcPos = assembler_.zeroCopyReadPosition();
                for (uint32_t f = 0; f < framesPerPacket; ++f) {
                    const uint32_t frameIdx = (zcPos + f) % zeroCopyFrameCapacity_;
                    const uint32_t sampleIdx = frameIdx * pcmChannels;
                    for (uint32_t ch = 0; ch < pcmChannels; ++ch) {
                        samples[f * pcmChannels + ch] = zcBase[sampleIdx + ch];
                    }
                }
                assembler_.setZeroCopyReadPosition((zcPos + framesPerPacket) % zeroCopyFrameCapacity_);
                framesRead = framesPerPacket;
            } else {
                framesRead = assembler_.ringBuffer().read(samples, framesPerPacket);
            }

            const uint32_t consumed = sharedTxQueue_.ConsumeFrames(framesPerPacket);
            if (consumed < framesPerPacket || zeroCopyFillBefore < framesPerPacket) {
                counters_.exitZeroRefill.fetch_add(1, std::memory_order_relaxed);
                counters_.underrunSilencedPackets.fetch_add(1, std::memory_order_relaxed);
                assembler_.recordUnderrun(zeroCopyFillBefore, framesPerPacket,
                                          consumed, 0, 0);
                continue; // leave silence in place
            }
        } else {
            framesRead = assembler_.ringBuffer().read(samples, framesPerPacket);
        }

        if (framesRead < framesPerPacket) {
            const size_t samplesRead = framesRead * pcmChannels;
            const size_t totalSamples = framesPerPacket * pcmChannels;
            std::memset(&samples[samplesRead], 0,
                        (totalSamples - samplesRead) * sizeof(int32_t));
        }

        uint8_t* payloadVirt = slab.PayloadPtr(idx);
        if (!payloadVirt) {
            continue;
        }
        uint32_t* audioQuadlets = reinterpret_cast<uint32_t*>(payloadVirt + Encoding::kCIPHeaderSize);

        EncodePcmFramesWithAm824Placeholders(samples, framesPerPacket, pcmChannels, am824Slots, audioQuadlets);
    }

    audioWriteIndex_ = audioTarget;

    std::atomic_thread_fence(std::memory_order_release);
    ASFW::Driver::WriteBarrier();
}

} // namespace ASFW::Isoch
