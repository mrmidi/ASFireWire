// IsochTxVerifier.cpp

#include "IsochTxVerifier.hpp"

namespace ASFW::Isoch {

using namespace ASFW::Async::HW;

namespace {
constexpr uint32_t kMaxAudioQuadlets =
    Encoding::kSamplesPerDataPacket * Config::kMaxAmdtpDbs;
constexpr uint32_t kMaxPacketsPerRun = 64;
static_assert(kMaxAudioQuadlets <= (Tx::Layout::kAudioWriteAhead * Config::kMaxAmdtpDbs),
              "TraceEntry audioHost buffer must be large enough");

[[nodiscard]] uint32_t CircularDistance(uint32_t a, uint32_t b) noexcept {
    constexpr uint32_t n = Tx::Layout::kNumPackets;
    const uint32_t d1 = (a + n - b) % n;
    const uint32_t d2 = (b + n - a) % n;
    return (d1 < d2) ? d1 : d2;
}
} // namespace

uint8_t IsochTxVerifier::ExpectedAM824Label(uint32_t slotInFrame,
                                            const PacketExpectations& expectations) noexcept {
    if (slotInFrame < expectations.pcmSlots) {
        return Encoding::kAM824LabelMBLA;
    }

    const uint32_t midiSlotIndex = slotInFrame - expectations.pcmSlots;
    return static_cast<uint8_t>(
        Encoding::kAM824LabelMIDIConformantBase + (midiSlotIndex & 0x03u));
}

void IsochTxVerifier::RecordInvalidLabel(AudioPayloadScan& scan, uint32_t q) noexcept {
    if (!scan.sawInvalidLabel) {
        scan.badLabel = ASFW::Isoch::TxVerify::AM824LabelByte(q);
        scan.badWord = q;
    }

    scan.sawInvalidLabel = true;
    if (q != 0) {
        scan.sawInvalidLabelNonZero = true;
    }
}

IsochTxVerifier::AudioPayloadScan IsochTxVerifier::ScanAudioPayload(
    const TraceEntry& entry,
    const PacketExpectations& expectations) noexcept {
    const uint32_t silenceHost = Encoding::AM824Encoder::encodeSilence();
    AudioPayloadScan scan{};

    for (uint32_t index = 0; index < entry.audioQuadletCount; ++index) {
        const uint32_t q = entry.audioHost[index];
        const uint32_t slotInFrame = index % expectations.slotsPerFrame;
        const bool isPcmSlot = slotInFrame < expectations.pcmSlots;
        const bool rawPcmMode =
            expectations.audioWireFormat == Encoding::AudioWireFormat::kRawPcm24In32;

        if (q == 0) {
            scan.sawAllZero = true;
        }
        if (rawPcmMode && isPcmSlot) {
            if (q != 0) {
                scan.allSilence = false;
            }
            continue;
        }
        if (!ASFW::Isoch::TxVerify::HasValidAM824Label(q, ExpectedAM824Label(slotInFrame, expectations))) {
            RecordInvalidLabel(scan, q);
        }
        if (isPcmSlot) {
            if (rawPcmMode) {
                if (q != 0) {
                    scan.allSilence = false;
                }
            } else if (q != silenceHost) {
                scan.allSilence = false;
            }
        }
    }

    return scan;
}

void IsochTxVerifier::ResetForStart(uint8_t blocksPerData) noexcept {
    shuttingDown_.store(false, std::memory_order_release);
    queued_.clear(std::memory_order_release);

    trace_.writeIndex.store(0, std::memory_order_relaxed);
    trace_.readIndex.store(0, std::memory_order_relaxed);
    trace_.dropped.store(0, std::memory_order_relaxed);

    state_ = State{};
    state_.blocksPerData = blocksPerData;

#ifndef ASFW_HOST_TEST
    if (!queue_) {
        IODispatchQueue* q = nullptr;
        auto kr = IODispatchQueue::Create("com.asfw.isoch.txverify", 0, 0, &q);
        if (kr != kIOReturnSuccess || !q) {
            ASFW_LOG(Isoch, "IT: Failed to create TX verify queue (kr=0x%08x)", kr);
        } else {
            queue_ = OSSharedPtr(q, OSNoRetain);
        }
    }
#endif
}

void IsochTxVerifier::Shutdown() noexcept {
    shuttingDown_.store(true, std::memory_order_release);

#ifndef ASFW_HOST_TEST
    if (queue_) {
        queue_->DispatchSync(^{
            // Barrier only.
        });
    }
#endif

    queued_.clear(std::memory_order_release);
}

void IsochTxVerifier::Kick(const Inputs& inputs) noexcept {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    if (!ASFW::LogConfig::Shared().IsIsochTxVerifierEnabled()) {
        return;
    }

    if (queued_.test_and_set(std::memory_order_acq_rel)) {
        return;
    }

    inputs_ = inputs;
    std::atomic_thread_fence(std::memory_order_release);

#ifdef ASFW_HOST_TEST
    RunWork();
#else
    if (!queue_) {
        RunWork();
        return;
    }

    IsochTxVerifier* self = this;
    queue_->DispatchAsync(^{
        self->RunWork();
    });
#endif
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void IsochTxVerifier::CaptureBeforeOverwrite(uint32_t packetIndex,
                                             uint32_t hwPacketIndexCmdPtr,
                                             uint32_t cmdPtr,
                                             const OHCIDescriptor* lastDesc,
                                             const uint32_t* payload32) noexcept {
    if (!lastDesc || !payload32) {
        return;
    }

    TraceEntry entry{};
    entry.packetIndex = packetIndex;
    entry.hwPacketIndexCmdPtr = hwPacketIndexCmdPtr;
    entry.cmdPtr = cmdPtr;
    entry.lastDescControl = lastDesc->control;
    entry.lastDescStatus = lastDesc->statusWord;
    entry.reqCount = static_cast<uint16_t>(lastDesc->control & 0xFFFFu);

    entry.cipQ0Host = payload32[0];
    entry.cipQ1Host = payload32[1];

    const uint32_t audioBytes =
        (entry.reqCount > Encoding::kCIPHeaderSize) ? (entry.reqCount - Encoding::kCIPHeaderSize) : 0;
    uint32_t audioQuadlets = audioBytes / 4;
    if (audioQuadlets > kMaxAudioQuadlets) {
        audioQuadlets = kMaxAudioQuadlets;
    }

    entry.audioQuadletCount = static_cast<uint16_t>(audioQuadlets);
    for (uint32_t i = 0; i < audioQuadlets; ++i) {
        entry.audioHost[i] = payload32[2 + i];
    }

    const uint32_t w = trace_.writeIndex.load(std::memory_order_relaxed);
    const uint32_t r = trace_.readIndex.load(std::memory_order_acquire);
    if ((w - r) >= kTraceCapacity) {
        trace_.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    trace_.entries[w & (kTraceCapacity - 1)] = entry;
    trace_.writeIndex.store(w + 1, std::memory_order_release);
}

bool IsochTxVerifier::Pop(TraceEntry& out) noexcept {
    const uint32_t r = trace_.readIndex.load(std::memory_order_relaxed);
    const uint32_t w = trace_.writeIndex.load(std::memory_order_acquire);
    if (r == w) {
        return false;
    }

    out = trace_.entries[r & (kTraceCapacity - 1)];
    trace_.readIndex.store(r + 1, std::memory_order_release);
    return true;
}

void IsochTxVerifier::DrainTrace() noexcept {
    TraceEntry tmp{};
    while (Pop(tmp)) {
    }
}

IsochTxVerifier::CounterDeltaSnapshot IsochTxVerifier::CaptureCounterDeltas() const noexcept {
    CounterDeltaSnapshot deltas{};
    deltas.curInjectResets = inputs_.audioInjectCursorResets;
    deltas.curInjectMissed = inputs_.audioInjectMissedPackets;
    deltas.curUnderrunSilenced = inputs_.underrunSilencedPackets;
    deltas.curCriticalGap = inputs_.criticalGapEvents;
    deltas.curDbcDisc = inputs_.dbcDiscontinuities;
    deltas.curDroppedTrace = trace_.dropped.load(std::memory_order_relaxed);

    deltas.deltaResets = deltas.curInjectResets - state_.lastInjectCursorResets;
    deltas.deltaMissed = deltas.curInjectMissed - state_.lastInjectMissedPackets;
    deltas.deltaUnderrunSilenced =
        deltas.curUnderrunSilenced - state_.lastUnderrunSilencedPackets;
    deltas.deltaCriticalGap = deltas.curCriticalGap - state_.lastCriticalGapEvents;
    deltas.deltaDbcDisc = deltas.curDbcDisc - state_.lastDbcDiscontinuities;
    deltas.deltaDropped = deltas.curDroppedTrace - state_.lastDroppedTrace;
    return deltas;
}

void IsochTxVerifier::LogCounterDeltas(const CounterDeltaSnapshot& deltas) const noexcept {
    if (deltas.deltaResets) {
        ASFW_LOG_RL(Isoch, "txverify/inject_resets", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: audioInjectCursorResets +=%llu (total=%llu)",
                    deltas.deltaResets, deltas.curInjectResets);
    }
    if (deltas.deltaMissed) {
        ASFW_LOG_RL(Isoch, "txverify/inject_miss", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: audioInjectMissedPackets +=%llu (total=%llu)",
                    deltas.deltaMissed, deltas.curInjectMissed);
    }
    if (deltas.deltaUnderrunSilenced) {
        ASFW_LOG_RL(Isoch, "txverify/underrun_silenced", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: underrunSilencedPackets +=%llu (total=%llu)",
                    deltas.deltaUnderrunSilenced, deltas.curUnderrunSilenced);
    }
    if (deltas.deltaCriticalGap) {
        ASFW_LOG_RL(Isoch, "txverify/critical_gap", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: criticalGapEvents +=%llu (total=%llu)",
                    deltas.deltaCriticalGap, deltas.curCriticalGap);
    }
    if (deltas.deltaDbcDisc) {
        ASFW_LOG_RL(Isoch, "txverify/dbc_disc_counter", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: producer DBC discontinuities +=%llu (total=%llu)",
                    deltas.deltaDbcDisc, deltas.curDbcDisc);
    }
    if (deltas.deltaDropped) {
        ASFW_LOG_RL(Isoch, "txverify/trace_drop", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: trace ring dropped +=%llu (total=%llu)",
                    deltas.deltaDropped, deltas.curDroppedTrace);
    }
}

uint32_t IsochTxVerifier::UpdateCounterState(const CounterDeltaSnapshot& deltas) noexcept {
    uint32_t restartReasons = 0;

    if (deltas.deltaMissed != 0) {
        if (state_.injectMissConsecutiveTicks < 0xFFFFFFFFu) {
            ++state_.injectMissConsecutiveTicks;
        }
    } else {
        state_.injectMissConsecutiveTicks = 0;
    }
    if (deltas.deltaMissed >= 8 || state_.injectMissConsecutiveTicks >= 2) {
        restartReasons |= IsochTxRecoveryController::kReasonInjectMiss;
    }
    if (deltas.deltaDbcDisc != 0) {
        restartReasons |= IsochTxRecoveryController::kReasonDbcDiscontinuity;
    }

    state_.lastInjectCursorResets = deltas.curInjectResets;
    state_.lastInjectMissedPackets = deltas.curInjectMissed;
    state_.lastUnderrunSilencedPackets = deltas.curUnderrunSilenced;
    state_.lastCriticalGapEvents = deltas.curCriticalGap;
    state_.lastDbcDiscontinuities = deltas.curDbcDisc;
    state_.lastDroppedTrace = deltas.curDroppedTrace;

    return restartReasons;
}

IsochTxVerifier::PacketExpectations IsochTxVerifier::BuildPacketExpectations() const noexcept {
    PacketExpectations expectations{};
    expectations.expectedNoDataReq = static_cast<uint16_t>(Encoding::kCIPHeaderSize);
    expectations.expectedAm824Slots =
        (inputs_.am824Slots != 0) ? inputs_.am824Slots : inputs_.pcmChannels;
    expectations.expectedDataReq = static_cast<uint16_t>(
        Encoding::kCIPHeaderSize +
        static_cast<size_t>(inputs_.framesPerPacket) * expectations.expectedAm824Slots *
            sizeof(uint32_t));
    expectations.slotsPerFrame =
        (expectations.expectedAm824Slots != 0) ? expectations.expectedAm824Slots : 1;
    expectations.pcmSlots =
        (inputs_.pcmChannels < expectations.slotsPerFrame) ? inputs_.pcmChannels
                                                           : expectations.slotsPerFrame;
    expectations.audioWireFormat = inputs_.audioWireFormat;
    return expectations;
}

void IsochTxVerifier::ProcessTraceEntries(const PacketExpectations& expectations,
                                          uint64_t deltaMissed,
                                          uint32_t& restartReasons) noexcept {
    uint32_t processed = 0;
    TraceEntry entry{};
    while (processed < kMaxPacketsPerRun && Pop(entry)) {
        ++processed;
        ProcessTraceEntry(entry, expectations, deltaMissed, restartReasons);
    }
}

void IsochTxVerifier::ProcessTraceEntry(const TraceEntry& entry,
                                        const PacketExpectations& expectations,
                                        uint64_t deltaMissed,
                                        uint32_t& restartReasons) noexcept {
    const bool isNoDataByReq = (entry.reqCount == expectations.expectedNoDataReq);
    const bool isDataByReq = (entry.reqCount > expectations.expectedNoDataReq);
    const ParsedCIP cip = ASFW::Isoch::TxVerify::ParseCIPFromHostWords(entry.cipQ0Host,
                                                                       entry.cipQ1Host);
    const bool isNoData = (cip.syt == Encoding::kSYTNoData) || isNoDataByReq;
    const bool isData = !isNoData && isDataByReq;

    CheckCompletionAndPacketShape(entry, expectations, cip, isNoData, isData, restartReasons);

    const uint32_t dist = CircularDistance(entry.hwPacketIndexCmdPtr, entry.packetIndex);
    if (dist > Tx::Layout::kGuardBandPackets) {
        ASFW_LOG_RL(Isoch, "txverify/cmdptr_mismatch", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: cmdPtr packet index diverges from completion pkt=%u hwPkt(cmdPtr)=%u dist=%u",
                    entry.packetIndex, entry.hwPacketIndexCmdPtr, dist);
    }

    CheckDbcContinuity(entry, cip, isData, restartReasons);
    if (isData && entry.audioQuadletCount > 0) {
        CheckAudioPayload(entry, expectations, deltaMissed, restartReasons);
    }
}

void IsochTxVerifier::CheckCompletionAndPacketShape(const TraceEntry& entry,
                                                    const PacketExpectations& expectations,
                                                    const ParsedCIP& cip,
                                                    bool isNoData,
                                                    bool isData,
                                                    uint32_t& restartReasons) const noexcept {
    if (entry.lastDescStatus == 0) {
        const uint32_t q0Wire = ASFW::Isoch::TxVerify::ByteSwap32(entry.cipQ0Host);
        const uint32_t q1Wire = ASFW::Isoch::TxVerify::ByteSwap32(entry.cipQ1Host);
        ASFW_LOG_RL(Isoch, "txverify/uncompleted_overwrite", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: overwriting slot without completion? pkt=%u hwPkt(cmdPtr)=%u req=%u st=0x%08x cip=[%08x %08x]",
                    entry.packetIndex, entry.hwPacketIndexCmdPtr, entry.reqCount,
                    entry.lastDescStatus, q0Wire, q1Wire);
        restartReasons |= IsochTxRecoveryController::kReasonUncompletedOverwrite;
    }

    if (isNoData && entry.reqCount != expectations.expectedNoDataReq) {
        ASFW_LOG_RL(Isoch, "txverify/reqcount", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: unexpected NO-DATA reqCount pkt=%u req=%u expected=%u",
                    entry.packetIndex, entry.reqCount, expectations.expectedNoDataReq);
        restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
    }
    if (isData && entry.reqCount != expectations.expectedDataReq) {
        ASFW_LOG_RL(Isoch, "txverify/reqcount", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: unexpected DATA reqCount pkt=%u req=%u expected=%u (framesPerData=%u pcm=%u dbs=%u)",
                    entry.packetIndex, entry.reqCount, expectations.expectedDataReq,
                    inputs_.framesPerPacket, inputs_.pcmChannels,
                    expectations.expectedAm824Slots);
        restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
    }
    if (cip.eoh0 != 0) {
        ASFW_LOG_RL(Isoch, "txverify/cip_eoh", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: CIP q0 EOH mismatch pkt=%u eoh0=%u",
                    entry.packetIndex, cip.eoh0);
        restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
    }
    if (cip.eoh1 != 2) {
        ASFW_LOG_RL(Isoch, "txverify/cip_eoh", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: CIP q1 EOH mismatch pkt=%u eoh1=%u",
                    entry.packetIndex, cip.eoh1);
        restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
    }
    if (cip.fmt != Encoding::kCIPFormatAM824) {
        ASFW_LOG_RL(Isoch, "txverify/cip_fmt", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: CIP FMT mismatch pkt=%u fmt=0x%02x expected=0x%02x",
                    entry.packetIndex, cip.fmt, Encoding::kCIPFormatAM824);
        restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
    }
    if (cip.fdf != Encoding::kSFC_48kHz) {
        ASFW_LOG_RL(Isoch, "txverify/cip_fdf", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: CIP FDF mismatch pkt=%u fdf=0x%02x expected=0x%02x",
                    entry.packetIndex, cip.fdf, Encoding::kSFC_48kHz);
        restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
    }
    if (cip.dbs != expectations.expectedAm824Slots) {
        ASFW_LOG_RL(Isoch, "txverify/cip_dbs", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: CIP DBS mismatch pkt=%u dbs=%u expected=%u",
                    entry.packetIndex, cip.dbs, expectations.expectedAm824Slots);
        restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
    }
    if (isData && cip.syt == Encoding::kSYTNoData) {
        ASFW_LOG_RL(Isoch, "txverify/cip_syt", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: DATA packet has SYT=NO-DATA pkt=%u dbc=0x%02x",
                    entry.packetIndex, cip.dbc);
        restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
    }
    if (isNoData && cip.syt != Encoding::kSYTNoData) {
        ASFW_LOG_RL(Isoch, "txverify/cip_syt", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: NO-DATA packet has SYT=0x%04x pkt=%u dbc=0x%02x",
                    cip.syt, entry.packetIndex, cip.dbc);
        restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
    }
}

void IsochTxVerifier::CheckDbcContinuity(const TraceEntry& entry,
                                         const ParsedCIP& cip,
                                         bool isData,
                                         uint32_t& restartReasons) noexcept {
    if (!isData) {
        return;
    }

    if (state_.haveLastDataDbc) {
        const uint8_t expected = static_cast<uint8_t>(state_.lastDataDbc + state_.blocksPerData);
        if (cip.dbc != expected) {
            const uint32_t q0Wire = ASFW::Isoch::TxVerify::ByteSwap32(entry.cipQ0Host);
            const uint32_t q1Wire = ASFW::Isoch::TxVerify::ByteSwap32(entry.cipQ1Host);
            ASFW_LOG_RL(Isoch, "txverify/dbc_disc", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: DBC discontinuity pkt=%u got=0x%02x expected=0x%02x blocksPerData=%u cip=[%08x %08x]",
                        entry.packetIndex, cip.dbc, expected, state_.blocksPerData,
                        q0Wire, q1Wire);
            restartReasons |= IsochTxRecoveryController::kReasonDbcDiscontinuity;
        }
    }

    state_.haveLastDataDbc = true;
    state_.lastDataDbc = cip.dbc;
}

void IsochTxVerifier::CheckAudioPayload(const TraceEntry& entry,
                                        const PacketExpectations& expectations,
                                        uint64_t deltaMissed,
                                        uint32_t& restartReasons) noexcept {
    const AudioPayloadScan scan = ScanAudioPayload(entry, expectations);

    const auto audWire = [&](uint32_t index) noexcept -> uint32_t {
        if (index >= entry.audioQuadletCount) {
            return 0;
        }
        return ASFW::Isoch::TxVerify::ByteSwap32(entry.audioHost[index]);
    };
    const uint32_t q0Wire = ASFW::Isoch::TxVerify::ByteSwap32(entry.cipQ0Host);
    const uint32_t q1Wire = ASFW::Isoch::TxVerify::ByteSwap32(entry.cipQ1Host);

    if (scan.sawAllZero) {
        ASFW_LOG_RL(Isoch, "txverify/all_zero", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: ALL-ZERO audio quadlet(s) pkt=%u req=%u st=0x%08x cip=[%08x %08x] audWire=[%08x %08x %08x %08x %08x %08x %08x %08x]",
                    entry.packetIndex, entry.reqCount, entry.lastDescStatus, q0Wire, q1Wire,
                    audWire(0), audWire(1), audWire(2), audWire(3), audWire(4), audWire(5),
                    audWire(6), audWire(7));
    }
    if (scan.sawInvalidLabel) {
        ASFW_LOG_RL(Isoch, "txverify/invalid_label", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: invalid %{public}s label pkt=%u label=0x%02x wordHost=0x%08x cip=[%08x %08x] audWire=[%08x %08x %08x %08x %08x %08x %08x %08x]",
                    expectations.audioWireFormat == Encoding::AudioWireFormat::kRawPcm24In32
                        ? "raw-midi"
                        : "AM824",
                    entry.packetIndex, scan.badLabel, scan.badWord, q0Wire, q1Wire, audWire(0),
                    audWire(1), audWire(2), audWire(3), audWire(4), audWire(5), audWire(6),
                    audWire(7));
        if (scan.sawInvalidLabelNonZero) {
            restartReasons |= IsochTxRecoveryController::kReasonInvalidLabel;
        }
    }

    if (scan.allSilence) {
        ++state_.silentDataRun;
    } else {
        state_.silentDataRun = 0;
    }

    if (state_.silentDataRun < 8) {
        return;
    }

    const bool shouldHaveAudio =
        (!inputs_.zeroCopyEnabled && inputs_.sharedTxQueueValid &&
         inputs_.sharedTxQueueFillFrames >= inputs_.framesPerPacket) &&
        (deltaMissed == 0);
    if (shouldHaveAudio) {
        ASFW_LOG_RL(Isoch, "txverify/silence_run", 10000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: SUSPICIOUS SILENCE RUN len=%u pkt=%u qFill=%u framesPerPkt=%u",
                    state_.silentDataRun, entry.packetIndex, inputs_.sharedTxQueueFillFrames,
                    inputs_.framesPerPacket);
    }
}

void IsochTxVerifier::RunWork() noexcept {
    struct FlagGuard {
        std::atomic_flag& flag;
        ~FlagGuard() { flag.clear(std::memory_order_release); }
    } guard{queued_};

    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }

    std::atomic_thread_fence(std::memory_order_acquire);

    if (!ASFW::LogConfig::Shared().IsIsochTxVerifierEnabled()) {
        DrainTrace();
        return;
    }

    const CounterDeltaSnapshot deltas = CaptureCounterDeltas();
    LogCounterDeltas(deltas);

    uint32_t restartReasons = UpdateCounterState(deltas);
    const PacketExpectations expectations = BuildPacketExpectations();
    ProcessTraceEntries(expectations, deltas.deltaMissed, restartReasons);

    if (restartReasons && recovery_) {
        recovery_->Request(restartReasons);
    }
}

} // namespace ASFW::Isoch
