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

inline uint32_t EncodeRawPcm24In32Word(int32_t pcmSample) noexcept {
    const uint32_t normalized =
        static_cast<uint32_t>(Encoding::NormalizeSigned24In32LowAligned(pcmSample));
    return ((normalized & 0xFF000000u) >> 24) |
           ((normalized & 0x00FF0000u) >> 8) |
           ((normalized & 0x0000FF00u) << 8) |
           ((normalized & 0x000000FFu) << 24);
}

// Positional arguments mirror PCM input then AM824 output layout.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline void EncodePcmFramesWithPlaceholders(const int32_t* pcmInterleaved,
                                            uint32_t frames, // NOLINT(bugprone-easily-swappable-parameters)
                                            uint32_t pcmChannels,
                                            uint32_t am824Slots,
                                            Encoding::AudioWireFormat wireFormat,
                                            uint32_t* outWireQuadlets) noexcept {
    const uint32_t midiSlots = (am824Slots > pcmChannels) ? (am824Slots - pcmChannels) : 0;
    for (uint32_t f = 0; f < frames; ++f) {
        const int32_t* frameIn = pcmInterleaved + (static_cast<size_t>(f) * pcmChannels);
        uint32_t* frameOut = outWireQuadlets + (static_cast<size_t>(f) * am824Slots);

        for (uint32_t ch = 0; ch < pcmChannels; ++ch) {
            frameOut[ch] = (wireFormat == Encoding::AudioWireFormat::kRawPcm24In32)
                ? EncodeRawPcm24In32Word(frameIn[ch])
                : Encoding::AM824Encoder::encode(frameIn[ch]);
        }
        for (uint32_t s = 0; s < midiSlots; ++s) {
            frameOut[pcmChannels + s] = EncodeMidiPlaceholderSlot(s);
        }
    }
}

inline uint32_t ClampTransferFrames(uint32_t requested,
                                    uint32_t queueFill,
                                    uint32_t ringSpace,
                                    uint32_t transferChunkFrames) noexcept {
    uint32_t clamped = requested;
    if (clamped > queueFill) clamped = queueFill;
    if (clamped > ringSpace) clamped = ringSpace;
    if (clamped > transferChunkFrames) clamped = transferChunkFrames;
    return clamped;
}

inline void UpdateLowWaterAlert(bool& lowAlert,
                                uint32_t fillLevel,
                                uint32_t lowThreshold,
                                uint32_t recoverThreshold,
                                std::atomic<uint64_t>& counter) noexcept {
    if (!lowAlert && fillLevel < lowThreshold) {
        lowAlert = true;
        counter.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (lowAlert && fillLevel >= recoverThreshold) {
        lowAlert = false;
    }
}

inline uint64_t ExternalSyncStaleThresholdTicks() noexcept {
    uint64_t staleThresholdTicks = ASFW::Timing::nanosToHostTicks(100'000'000ULL);
    if (staleThresholdTicks == 0 && ASFW::Timing::initializeHostTimebase()) {
        staleThresholdTicks = ASFW::Timing::nanosToHostTicks(100'000'000ULL);
    }
    return staleThresholdTicks;
}

} // namespace

void IsochAudioTxPipeline::SetSharedTxQueue(uint8_t* base, uint64_t bytes) noexcept {
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

void IsochAudioTxPipeline::SetZeroCopyOutputBuffer(const int32_t* base, uint64_t bytes,
                                                   uint32_t frameCapacity) noexcept {
    if (!base || bytes == 0 || frameCapacity == 0) {
        zeroCopyAudioBase_ = nullptr;
        zeroCopyAudioBytes_ = 0;
        zeroCopyFrameCapacity_ = 0;
        zeroCopyEnabled_ = false;
        assembler_.setZeroCopySource(nullptr, 0);

        if (base || bytes || frameCapacity) { // NOSONAR(cpp:S3923): branches log different diagnostic messages
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

    assembler_.setZeroCopySource(base, frameCapacity);

    ASFW_LOG(Isoch, "IT: ✅ ZERO-COPY enabled! AudioBuffer base=%p bytes=%llu frames=%u assembler=%{public}s",
             static_cast<const void*>(base), bytes, frameCapacity,
             assembler_.isZeroCopyEnabled() ? "ENABLED" : "fallback");
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t IsochAudioTxPipeline::Configure(uint8_t sid,
                                              uint32_t streamModeRaw,
                                              uint32_t requestedChannels,
                                              uint32_t requestedAm824Slots,
                                              Encoding::AudioWireFormat wireFormat) noexcept {
    if (!sharedTxQueue_.IsValid()) {
        ASFW_LOG(Isoch, "IT: Configure failed - shared TX queue missing");
        return kIOReturnNotReady;
    }

    const uint32_t queueChannels = sharedTxQueue_.Channels();
    if (queueChannels == 0 || queueChannels > Config::kMaxPcmChannels) {
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
        if (requestedAm824Slots > Config::kMaxAmdtpDbs) {
            ASFW_LOG(Isoch,
                     "IT: Configure failed - requestedAm824Slots=%u exceed max supported=%u (pcm=%u)",
                     requestedAm824Slots,
                     Config::kMaxAmdtpDbs,
                     queueChannels);
            return kIOReturnUnsupported;
        }
        am824Slots = requestedAm824Slots;
    }

    assembler_.reconfigureAM824(queueChannels, am824Slots, sid);
    assembler_.setAudioWireFormat(wireFormat);

    requestedStreamMode_ = (streamModeRaw == 1u)
        ? Encoding::StreamMode::kBlocking
        : Encoding::StreamMode::kNonBlocking;
    effectiveStreamMode_ = requestedStreamMode_;
    assembler_.setStreamMode(effectiveStreamMode_);

    ASFW_LOG(Isoch, "IT: Stream mode resolved requested=%{public}s effective=%{public}s",
             requestedStreamMode_ == Encoding::StreamMode::kBlocking ? "blocking" : "non-blocking",
             effectiveStreamMode_ == Encoding::StreamMode::kBlocking ? "blocking" : "non-blocking");

    const uint32_t framesPerDataPacket = assembler_.samplesPerDataPacket();
    const uint32_t payloadBytes = static_cast<uint32_t>(
        static_cast<size_t>(framesPerDataPacket) * am824Slots * sizeof(uint32_t));
    const uint32_t packetBytes = Encoding::kCIPHeaderSize + payloadBytes;
    ASFW_LOG(Isoch,
             "IT: Channel geometry resolved pcm=%u dbs=%u midiSlots=%u framesPerData=%u payloadBytes=%u packetBytes=%u wire=%{public}s",
             queueChannels, am824Slots, (am824Slots > queueChannels) ? (am824Slots - queueChannels) : 0,
             framesPerDataPacket, payloadBytes, packetBytes,
             wireFormat == Encoding::AudioWireFormat::kRawPcm24In32 ? "raw24in32" : "am824");
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

    // 48 kHz milestone: RX-seeded sample timeline for DATA-packet SYT emission.
    sytGenerator_.initialize(48000.0);
    sytGenerator_.reset();
    cycleTrackingValid_ = false;
}

bool IsochAudioTxPipeline::PrimeSyncFromExternalBridge() noexcept {
    const auto syncState = ReadExternalSyncState();
    if (!syncState.enabled) {
        ASFW_LOG(Isoch, "IT: SYT seed unavailable - missing fresh established RX SYT");
        return false;
    }

    sytGenerator_.seedFromRxSyt(syncState.rxSyt);
    ASFW_LOG(Isoch, "IT: SYT seeded from RX bridge syt=0x%04x", syncState.rxSyt);
    return true;
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
    int32_t transferBuf[kTransferChunk * Config::kMaxPcmChannels];
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

void IsochAudioTxPipeline::ApplyPendingSharedQueueResync() noexcept {
    if (sharedTxQueue_.IsValid() && sharedTxQueue_.ConsumerApplyPendingResync()) {
        counters_.resyncApplied.fetch_add(1, std::memory_order_relaxed);
    }
}

IsochAudioTxPipeline::LegacyPumpResult
IsochAudioTxPipeline::PumpLegacyAssemblerRing(uint32_t targetRbFillFrames) noexcept {
    LegacyPumpResult result{};

    const uint32_t rbFill = assembler_.bufferFillLevel();
    if (rbFill >= targetRbFillFrames) {
        return result;
    }

    result.skipped = false;

    constexpr uint32_t kMaxRbFillFrames = Config::kTxBufferProfile.legacyRbMaxFrames;
    constexpr uint32_t kTransferChunkFrames = Config::kTransferChunkFrames;
    constexpr uint32_t kMaxChunksPerRefill = Config::kTxBufferProfile.legacyMaxChunksPerRefill;

    uint32_t want = targetRbFillFrames - rbFill;
    int32_t transferBuf[kTransferChunkFrames * Config::kMaxPcmChannels];
    uint32_t chunks = 0;

    while (want > 0 && chunks < kMaxChunksPerRefill) {
        const uint32_t qFill = sharedTxQueue_.FillLevelFrames();
        const uint32_t rbSpace = assembler_.ringBuffer().availableSpace();
        if (qFill == 0 || rbSpace == 0) {
            break;
        }

        const uint32_t toRead = ClampTransferFrames(want, qFill, rbSpace, kTransferChunkFrames);
        const uint32_t read = sharedTxQueue_.Read(transferBuf, toRead);
        if (read == 0) {
            break;
        }

        const uint32_t written = assembler_.ringBuffer().write(transferBuf, read);
        result.pumpedFrames += written;
        want -= written;
        ++chunks;

        if (written < read || assembler_.bufferFillLevel() >= kMaxRbFillFrames) {
            break;
        }
    }

    return result;
}

void IsochAudioTxPipeline::RecordLegacyPumpResult(const LegacyPumpResult& result) noexcept {
    if (result.skipped) {
        counters_.legacyPumpSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    counters_.legacyPumpMovedFrames.fetch_add(result.pumpedFrames, std::memory_order_relaxed);
}

void IsochAudioTxPipeline::UpdateLegacyFillLevelAlerts() noexcept {
    const uint32_t rbCap = assembler_.ringBuffer().capacity();
    const uint32_t rbFill = assembler_.bufferFillLevel();
    UpdateLowWaterAlert(fillLevelAlert_.rbLow,
                        rbFill,
                        rbCap / 20,
                        rbCap / 10,
                        counters_.rbLowEvents);

    const uint32_t txqCap = sharedTxQueue_.CapacityFrames();
    const uint32_t txqFill = sharedTxQueue_.FillLevelFrames();
    UpdateLowWaterAlert(fillLevelAlert_.txqLow,
                        txqFill,
                        txqCap / 20,
                        txqCap / 10,
                        counters_.txqLowEvents);
}

void IsochAudioTxPipeline::OnRefillTickPreHW() noexcept {
    ApplyPendingSharedQueueResync();

    if (zeroCopyEnabled_ || !sharedTxQueue_.IsValid()) {
        return;
    }

    const auto pumpResult = PumpLegacyAssemblerRing(adaptiveFill_.currentTarget);
    RecordLegacyPumpResult(pumpResult);
    UpdateLegacyFillLevelAlerts();
}

void IsochAudioTxPipeline::AdvanceAdaptiveFillWindow() noexcept {
    ++adaptiveFill_.windowTickCount;

    const uint64_t curZeroRefills = counters_.exitZeroRefill.load(std::memory_order_relaxed);
    const uint64_t curAssemblerUnderruns =
        assembler_.underrunDiag().underrunCount.load(std::memory_order_relaxed);
    const uint64_t combinedUnderruns = curZeroRefills + curAssemblerUnderruns;
    if (combinedUnderruns > adaptiveFill_.lastCombinedUnderruns) {
        adaptiveFill_.underrunsInWindow +=
            static_cast<uint32_t>(combinedUnderruns - adaptiveFill_.lastCombinedUnderruns);
        adaptiveFill_.lastCombinedUnderruns = combinedUnderruns;
    }
}

void IsochAudioTxPipeline::ApplyAdaptiveFillPolicy() noexcept {
    if (adaptiveFill_.underrunsInWindow >= 3) {
        uint32_t newTarget = adaptiveFill_.currentTarget + 128;
        if (newTarget > adaptiveFill_.maxTarget) {
            newTarget = adaptiveFill_.maxTarget;
        }

        if (newTarget != adaptiveFill_.currentTarget) {
            ASFW_LOG(Isoch, "IT: ADAPTIVE FILL ESCALATE %u -> %u (underruns=%u in window)",
                     adaptiveFill_.currentTarget, newTarget, adaptiveFill_.underrunsInWindow);
            adaptiveFill_.currentTarget = newTarget;
        }

        adaptiveFill_.cleanWindows = 0;
        return;
    }

    if (adaptiveFill_.underrunsInWindow == 0) {
        ++adaptiveFill_.cleanWindows;
        if (adaptiveFill_.cleanWindows >= 10 &&
            adaptiveFill_.currentTarget > adaptiveFill_.baseTarget) {
            uint32_t newTarget = adaptiveFill_.currentTarget;
            newTarget = (newTarget > adaptiveFill_.baseTarget + 64)
                ? (newTarget - 64)
                : adaptiveFill_.baseTarget;
            if (newTarget != adaptiveFill_.currentTarget) {
                ASFW_LOG(Isoch, "IT: ADAPTIVE FILL DECAY %u -> %u (cleanWindows=%u)",
                         adaptiveFill_.currentTarget, newTarget, adaptiveFill_.cleanWindows);
                adaptiveFill_.currentTarget = newTarget;
            }
        }
        return;
    }

    adaptiveFill_.cleanWindows = 0;
}

void IsochAudioTxPipeline::OnPollTick1ms() noexcept {
    if (zeroCopyEnabled_ || !sharedTxQueue_.IsValid()) {
        return;
    }

    AdvanceAdaptiveFillWindow();
    if (adaptiveFill_.windowTickCount < 1000) {
        return;
    }

    ApplyAdaptiveFillPolicy();
    adaptiveFill_.windowTickCount = 0;
    adaptiveFill_.underrunsInWindow = 0;
}

uint16_t IsochAudioTxPipeline::ComputeDataSyt(uint32_t transmitCycle) noexcept {
    if (!sytGenerator_.isValid() || !cycleTrackingValid_) {
        return Encoding::SYTGenerator::kNoInfo;
    }

    const uint16_t txSyt = sytGenerator_.computeDataSYT(transmitCycle, assembler_.samplesPerDataPacket());
    const bool safetyOk = MaybeApplyExternalSyncDiscipline(txSyt);
    if (!safetyOk) {
        return Encoding::SYTGenerator::kNoInfo;
    }
    return txSyt;
}

IsochAudioTxPipeline::ExternalSyncState
IsochAudioTxPipeline::ReadExternalSyncState() noexcept {
    if (!externalSyncBridge_ || !HasFreshExternalSyncUpdate(*externalSyncBridge_)) {
        return {};
    }

    const uint32_t packed = externalSyncBridge_->lastPackedRx.load(std::memory_order_acquire);
    const uint16_t candidateSyt = Core::ExternalSyncBridge::UnpackSYT(packed);
    const uint8_t candidateFdf = Core::ExternalSyncBridge::UnpackFDF(packed);
    if (candidateSyt == Core::ExternalSyncBridge::kNoInfoSyt ||
        candidateFdf != Core::ExternalSyncBridge::kFdf48k) {
        return {};
    }

    return {.enabled = true, .rxSyt = candidateSyt};
}

bool IsochAudioTxPipeline::HasFreshExternalSyncUpdate(const Core::ExternalSyncBridge& bridge) noexcept {
    if (!bridge.active.load(std::memory_order_acquire) ||
        !bridge.clockEstablished.load(std::memory_order_acquire)) {
        return false;
    }

    const uint64_t staleThresholdTicks = ExternalSyncStaleThresholdTicks();
    const uint64_t lastUpdateTicks = bridge.lastUpdateHostTicks.load(std::memory_order_acquire);
    if (staleThresholdTicks == 0 || lastUpdateTicks == 0) {
        return false;
    }

    const uint64_t nowTicks = mach_absolute_time();
    return nowTicks >= lastUpdateTicks &&
        (nowTicks - lastUpdateTicks) <= staleThresholdTicks;
}

bool IsochAudioTxPipeline::MaybeApplyExternalSyncDiscipline(uint16_t txSyt) noexcept {
    const auto syncState = ReadExternalSyncState();
    const auto result = externalSyncDiscipline_.Update(syncState.enabled, txSyt, syncState.rxSyt);

    if (result.correctionTicks != 0) {
        sytGenerator_.nudgeOffsetTicks(result.correctionTicks);
        if (result.firstPassSnap) {
            ASFW_LOG(Isoch, "IT: SYT discipline first-pass snap: error=%d correction=%d ticks",
                     result.phaseErrorTicks, result.correctionTicks);
        }
    }

    return result.safetyGateOpen;
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
    std::memcpy(silentPacketStorage_.data(), pkt.data, pkt.size);
    out.words = reinterpret_cast<const uint32_t*>(silentPacketStorage_.data());
    out.sizeBytes = pkt.size;
    out.isData = pkt.isData;
    out.dbc = pkt.dbc;
    return out;
}

IsochAudioTxPipeline::AudioInjectionPlan
IsochAudioTxPipeline::BuildAudioInjectionPlan(uint32_t hwPacketIndex) noexcept {
    constexpr uint32_t kNumPackets = Tx::Layout::kNumPackets;

    AudioInjectionPlan plan{};
    plan.zeroCopySync = zeroCopyEnabled_ && sharedTxQueue_.IsValid() && zeroCopyFrameCapacity_ > 0;
    plan.audioTarget = (hwPacketIndex + Tx::Layout::kAudioWriteAhead) % kNumPackets;

    const uint32_t distBehind = (hwPacketIndex + kNumPackets - audioWriteIndex_) % kNumPackets;
    if (distBehind > 0 && distBehind < kNumPackets / 2) {
        counters_.audioInjectCursorResets.fetch_add(1, std::memory_order_relaxed);
        counters_.audioInjectMissedPackets.fetch_add(distBehind, std::memory_order_relaxed);
        audioWriteIndex_ = hwPacketIndex;
    }

    plan.packetsToInject = (plan.audioTarget + kNumPackets - audioWriteIndex_) % kNumPackets;
    if (plan.packetsToInject > Tx::Layout::kAudioWriteAhead) {
        plan.packetsToInject = Tx::Layout::kAudioWriteAhead;
    }
    if (plan.packetsToInject == 0) {
        return plan;
    }

    plan.framesPerPacket = assembler_.samplesPerDataPacket();
    plan.pcmChannels = assembler_.channelCount();
    plan.am824Slots = assembler_.am824SlotCount();
    return plan;
}

bool IsochAudioTxPipeline::PacketCarriesAudio(uint32_t packetIndex,
                                              Tx::IsochTxDescriptorSlab& slab) noexcept {
    const uint32_t descBase = packetIndex * Tx::Layout::kBlocksPerPacket;
    auto* lastDesc = slab.GetDescriptorPtr(descBase + 2);
    if (!lastDesc) {
        return false;
    }

    const uint16_t reqCount = static_cast<uint16_t>(lastDesc->control & 0xFFFF);
    return reqCount > Encoding::kCIPHeaderSize;
}

IsochAudioTxPipeline::PacketReadResult
IsochAudioTxPipeline::ReadPacketSamples(const AudioInjectionPlan& plan, int32_t* samples) noexcept {
    if (!plan.zeroCopySync) {
        return {.framesRead = assembler_.ringBuffer().read(samples, plan.framesPerPacket)};
    }

    return ReadZeroCopyPacketSamples(plan, samples);
}

IsochAudioTxPipeline::PacketReadResult
IsochAudioTxPipeline::ReadZeroCopyPacketSamples(const AudioInjectionPlan& plan,
                                                int32_t* samples) noexcept {
    PacketReadResult result{};

    uint32_t zeroCopyFillBefore = sharedTxQueue_.FillLevelFrames();
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
        CopyZeroCopyFrames(plan, samples);
        result.framesRead = plan.framesPerPacket;
    } else {
        result.framesRead = assembler_.ringBuffer().read(samples, plan.framesPerPacket);
    }

    const uint32_t consumed = sharedTxQueue_.ConsumeFrames(plan.framesPerPacket);
    if (consumed < plan.framesPerPacket || zeroCopyFillBefore < plan.framesPerPacket) {
        counters_.exitZeroRefill.fetch_add(1, std::memory_order_relaxed);
        counters_.underrunSilencedPackets.fetch_add(1, std::memory_order_relaxed);
        assembler_.recordUnderrun(zeroCopyFillBefore, plan.framesPerPacket, consumed, 0, 0);
        result.leaveSilence = true;
    }

    return result;
}

void IsochAudioTxPipeline::CopyZeroCopyFrames(const AudioInjectionPlan& plan, int32_t* samples) noexcept {
    const int32_t* zcBase = zeroCopyAudioBase_;
    const uint32_t zcPos = assembler_.zeroCopyReadPosition();
    for (uint32_t f = 0; f < plan.framesPerPacket; ++f) {
        const uint32_t frameIdx = (zcPos + f) % zeroCopyFrameCapacity_;
        const uint32_t sampleIdx = frameIdx * plan.pcmChannels;
        for (uint32_t ch = 0; ch < plan.pcmChannels; ++ch) {
            samples[f * plan.pcmChannels + ch] = zcBase[sampleIdx + ch];
        }
    }
    assembler_.setZeroCopyReadPosition((zcPos + plan.framesPerPacket) % zeroCopyFrameCapacity_);
}

void IsochAudioTxPipeline::PadPacketSamples(const AudioInjectionPlan& plan,
                                            uint32_t framesRead,
                                            int32_t* samples) noexcept {
    if (framesRead >= plan.framesPerPacket) {
        return;
    }

    const size_t samplesRead = static_cast<size_t>(framesRead) * plan.pcmChannels;
    const size_t totalSamples = static_cast<size_t>(plan.framesPerPacket) * plan.pcmChannels;
    std::memset(&samples[samplesRead], 0, (totalSamples - samplesRead) * sizeof(int32_t));
}

void IsochAudioTxPipeline::EncodeInjectedPacket(uint32_t packetIndex,
                                                Tx::IsochTxDescriptorSlab& slab,
                                                const AudioInjectionPlan& plan,
                                                const int32_t* samples) noexcept {
    uint8_t* payloadVirt = slab.PayloadPtr(packetIndex);
    if (!payloadVirt) {
        return;
    }

    uint32_t* audioQuadlets =
        reinterpret_cast<uint32_t*>(payloadVirt + Encoding::kCIPHeaderSize);
    EncodePcmFramesWithPlaceholders(samples,
                                    plan.framesPerPacket,
                                    plan.pcmChannels,
                                    plan.am824Slots,
                                    assembler_.audioWireFormat(),
                                    audioQuadlets);
}

void IsochAudioTxPipeline::InjectNearHw(uint32_t hwPacketIndex, Tx::IsochTxDescriptorSlab& slab) noexcept {
    auto plan = BuildAudioInjectionPlan(hwPacketIndex);
    if (plan.packetsToInject == 0) {
        return;
    }

    constexpr uint32_t kNumPackets = Tx::Layout::kNumPackets;
    for (uint32_t i = 0; i < plan.packetsToInject; ++i) {
        const uint32_t packetIndex = (audioWriteIndex_ + i) % kNumPackets;
        if (!PacketCarriesAudio(packetIndex, slab)) {
            continue;
        }

        std::array<int32_t, Encoding::kSamplesPerDataPacket * Config::kMaxPcmChannels> samples{};
        const auto readResult = ReadPacketSamples(plan, samples.data());
        if (readResult.leaveSilence) {
            continue;
        }

        PadPacketSamples(plan, readResult.framesRead, samples.data());
        EncodeInjectedPacket(packetIndex, slab, plan, samples.data());
    }

    audioWriteIndex_ = plan.audioTarget;

    std::atomic_thread_fence(std::memory_order_release);
    ASFW::Driver::WriteBarrier();
}

} // namespace ASFW::Isoch
