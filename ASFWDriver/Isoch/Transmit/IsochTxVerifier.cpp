// IsochTxVerifier.cpp

#include "IsochTxVerifier.hpp"

namespace ASFW::Isoch {

using namespace ASFW::Async::HW;

namespace {
constexpr uint32_t kMaxAudioQuadlets =
    Encoding::kSamplesPerDataPacket * Config::kMaxAmdtpDbs;
static_assert(kMaxAudioQuadlets <= (Tx::Layout::kAudioWriteAhead * Config::kMaxAmdtpDbs),
              "TraceEntry audioHost buffer must be large enough");
} // namespace

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
        TraceEntry tmp{};
        while (Pop(tmp)) {
        }
        return;
    }

    uint32_t restartReasons = 0;

    const uint64_t curInjectResets = inputs_.audioInjectCursorResets;
    const uint64_t curInjectMissed = inputs_.audioInjectMissedPackets;
    const uint64_t curUnderrunSilenced = inputs_.underrunSilencedPackets;
    const uint64_t curCriticalGap = inputs_.criticalGapEvents;
    const uint64_t curDbcDisc = inputs_.dbcDiscontinuities;
    const uint64_t curDroppedTrace = trace_.dropped.load(std::memory_order_relaxed);

    const uint64_t deltaResets = curInjectResets - state_.lastInjectCursorResets;
    const uint64_t deltaMissed = curInjectMissed - state_.lastInjectMissedPackets;
    const uint64_t deltaUnderrunSilenced = curUnderrunSilenced - state_.lastUnderrunSilencedPackets;
    const uint64_t deltaCriticalGap = curCriticalGap - state_.lastCriticalGapEvents;
    const uint64_t deltaDbcDisc = curDbcDisc - state_.lastDbcDiscontinuities;
    const uint64_t deltaDropped = curDroppedTrace - state_.lastDroppedTrace;

    if (deltaResets) {
        ASFW_LOG_RL(Isoch, "txverify/inject_resets", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: audioInjectCursorResets +=%llu (total=%llu)",
                    deltaResets, curInjectResets);
    }
    if (deltaMissed) {
        ASFW_LOG_RL(Isoch, "txverify/inject_miss", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: audioInjectMissedPackets +=%llu (total=%llu)",
                    deltaMissed, curInjectMissed);
    }
    if (deltaUnderrunSilenced) {
        ASFW_LOG_RL(Isoch, "txverify/underrun_silenced", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: underrunSilencedPackets +=%llu (total=%llu)",
                    deltaUnderrunSilenced, curUnderrunSilenced);
    }
    if (deltaCriticalGap) {
        ASFW_LOG_RL(Isoch, "txverify/critical_gap", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: criticalGapEvents +=%llu (total=%llu)",
                    deltaCriticalGap, curCriticalGap);
    }
    if (deltaDbcDisc) {
        ASFW_LOG_RL(Isoch, "txverify/dbc_disc_counter", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: producer DBC discontinuities +=%llu (total=%llu)",
                    deltaDbcDisc, curDbcDisc);
    }
    if (deltaDropped) {
        ASFW_LOG_RL(Isoch, "txverify/trace_drop", 1000, OS_LOG_TYPE_DEFAULT,
                    "IT TX VERIFY: trace ring dropped +=%llu (total=%llu)",
                    deltaDropped, curDroppedTrace);
    }

    // Recovery trigger: injection misses should not sustain more than ~1-2ms.
    if (deltaMissed) {
        if (state_.injectMissConsecutiveTicks < 0xFFFFFFFFu) {
            ++state_.injectMissConsecutiveTicks;
        }
    } else {
        state_.injectMissConsecutiveTicks = 0;
    }
    if (deltaMissed >= 8 || state_.injectMissConsecutiveTicks >= 2) {
        restartReasons |= IsochTxRecoveryController::kReasonInjectMiss;
    }

    state_.lastInjectCursorResets = curInjectResets;
    state_.lastInjectMissedPackets = curInjectMissed;
    state_.lastUnderrunSilencedPackets = curUnderrunSilenced;
    state_.lastCriticalGapEvents = curCriticalGap;
    state_.lastDbcDiscontinuities = curDbcDisc;
    state_.lastDroppedTrace = curDroppedTrace;

    if (deltaDbcDisc) {
        restartReasons |= IsochTxRecoveryController::kReasonDbcDiscontinuity;
    }

    auto circularDistance = [](uint32_t a, uint32_t b) noexcept -> uint32_t {
        constexpr uint32_t n = Tx::Layout::kNumPackets;
        const uint32_t d1 = (a + n - b) % n;
        const uint32_t d2 = (b + n - a) % n;
        return (d1 < d2) ? d1 : d2;
    };

    constexpr uint32_t kMaxPacketsPerRun = 64;
    uint32_t processed = 0;
    TraceEntry e{};
    while (processed < kMaxPacketsPerRun && Pop(e)) {
        ++processed;

        const uint16_t expectedNoDataReq = static_cast<uint16_t>(Encoding::kCIPHeaderSize);
        const uint32_t expectedAm824Slots = (inputs_.am824Slots != 0)
            ? inputs_.am824Slots
            : inputs_.pcmChannels;
        const uint16_t expectedDataReq = static_cast<uint16_t>(
            Encoding::kCIPHeaderSize +
            inputs_.framesPerPacket * expectedAm824Slots * sizeof(uint32_t));

        const bool isNoDataByReq = (e.reqCount == expectedNoDataReq);
        const bool isDataByReq = (e.reqCount > expectedNoDataReq);

        const auto cip = ASFW::Isoch::TxVerify::ParseCIPFromHostWords(e.cipQ0Host, e.cipQ1Host);
        const bool isNoDataByCip = (cip.syt == Encoding::kSYTNoData);

        const bool isNoData = isNoDataByCip || isNoDataByReq;
        const bool isData = (!isNoData) && isDataByReq;

        const bool completed = (e.lastDescStatus != 0);
        if (!completed) {
            const uint32_t q0Wire = ASFW::Isoch::TxVerify::ByteSwap32(e.cipQ0Host);
            const uint32_t q1Wire = ASFW::Isoch::TxVerify::ByteSwap32(e.cipQ1Host);
            ASFW_LOG_RL(Isoch, "txverify/uncompleted_overwrite", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: overwriting slot without completion? pkt=%u hwPkt(cmdPtr)=%u req=%u st=0x%08x cip=[%08x %08x]",
                        e.packetIndex, e.hwPacketIndexCmdPtr, e.reqCount, e.lastDescStatus, q0Wire, q1Wire);
            restartReasons |= IsochTxRecoveryController::kReasonUncompletedOverwrite;
        }

        if (isNoData && e.reqCount != expectedNoDataReq) {
            ASFW_LOG_RL(Isoch, "txverify/reqcount", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: unexpected NO-DATA reqCount pkt=%u req=%u expected=%u",
                        e.packetIndex, e.reqCount, expectedNoDataReq);
            restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
        }
        if (isData && e.reqCount != expectedDataReq) {
            ASFW_LOG_RL(Isoch, "txverify/reqcount", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: unexpected DATA reqCount pkt=%u req=%u expected=%u (framesPerData=%u pcm=%u dbs=%u)",
                        e.packetIndex, e.reqCount, expectedDataReq,
                        inputs_.framesPerPacket, inputs_.pcmChannels, expectedAm824Slots);
            restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
        }

        if (cip.eoh0 != 0) {
            ASFW_LOG_RL(Isoch, "txverify/cip_eoh", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: CIP q0 EOH mismatch pkt=%u eoh0=%u",
                        e.packetIndex, cip.eoh0);
            restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
        }
        if (cip.eoh1 != 2) {
            ASFW_LOG_RL(Isoch, "txverify/cip_eoh", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: CIP q1 EOH mismatch pkt=%u eoh1=%u",
                        e.packetIndex, cip.eoh1);
            restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
        }
        if (cip.fmt != Encoding::kCIPFormatAM824) {
            ASFW_LOG_RL(Isoch, "txverify/cip_fmt", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: CIP FMT mismatch pkt=%u fmt=0x%02x expected=0x%02x",
                        e.packetIndex, cip.fmt, Encoding::kCIPFormatAM824);
            restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
        }
        if (cip.fdf != Encoding::kSFC_48kHz) {
            ASFW_LOG_RL(Isoch, "txverify/cip_fdf", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: CIP FDF mismatch pkt=%u fdf=0x%02x expected=0x%02x",
                        e.packetIndex, cip.fdf, Encoding::kSFC_48kHz);
            restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
        }
        if (cip.dbs != expectedAm824Slots) {
            ASFW_LOG_RL(Isoch, "txverify/cip_dbs", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: CIP DBS mismatch pkt=%u dbs=%u expected=%u",
                        e.packetIndex, cip.dbs, expectedAm824Slots);
            restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
        }
        if (isData && cip.syt == Encoding::kSYTNoData) {
            ASFW_LOG_RL(Isoch, "txverify/cip_syt", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: DATA packet has SYT=NO-DATA pkt=%u dbc=0x%02x",
                        e.packetIndex, cip.dbc);
            restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
        }
        if (isNoData && cip.syt != Encoding::kSYTNoData) {
            ASFW_LOG_RL(Isoch, "txverify/cip_syt", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: NO-DATA packet has SYT=0x%04x pkt=%u dbc=0x%02x",
                        cip.syt, e.packetIndex, cip.dbc);
            restartReasons |= IsochTxRecoveryController::kReasonCipAnomaly;
        }

        const uint32_t dist = circularDistance(e.hwPacketIndexCmdPtr, e.packetIndex);
        if (dist > Tx::Layout::kGuardBandPackets) {
            ASFW_LOG_RL(Isoch, "txverify/cmdptr_mismatch", 1000, OS_LOG_TYPE_DEFAULT,
                        "IT TX VERIFY: cmdPtr packet index diverges from completion pkt=%u hwPkt(cmdPtr)=%u dist=%u",
                        e.packetIndex, e.hwPacketIndexCmdPtr, dist);
        }

        // Verifier-side DBC continuity (ignore NO-DATA).
        if (isData) {
            if (state_.haveLastDataDbc) {
                const uint8_t expected = static_cast<uint8_t>(state_.lastDataDbc + state_.blocksPerData);
                if (cip.dbc != expected) {
                    const uint32_t q0Wire = ASFW::Isoch::TxVerify::ByteSwap32(e.cipQ0Host);
                    const uint32_t q1Wire = ASFW::Isoch::TxVerify::ByteSwap32(e.cipQ1Host);
                    ASFW_LOG_RL(Isoch, "txverify/dbc_disc", 1000, OS_LOG_TYPE_DEFAULT,
                                "IT TX VERIFY: DBC discontinuity pkt=%u got=0x%02x expected=0x%02x blocksPerData=%u cip=[%08x %08x]",
                                e.packetIndex, cip.dbc, expected, state_.blocksPerData, q0Wire, q1Wire);
                    restartReasons |= IsochTxRecoveryController::kReasonDbcDiscontinuity;
                }
            }
            state_.haveLastDataDbc = true;
            state_.lastDataDbc = cip.dbc;
        }

        if (isData && e.audioQuadletCount > 0) {
            const uint32_t silenceHost = Encoding::AM824Encoder::encodeSilence();
            const uint32_t slotsPerFrame = (expectedAm824Slots != 0) ? expectedAm824Slots : 1;
            const uint32_t pcmSlots = (inputs_.pcmChannels < slotsPerFrame)
                ? inputs_.pcmChannels
                : slotsPerFrame;
            bool allSilence = true;
            bool sawAllZero = false;
            bool sawInvalidLabel = false;
            bool sawInvalidLabelNonZero = false;
            uint8_t badLabel = 0;
            uint32_t badWord = 0;

            for (uint32_t i = 0; i < e.audioQuadletCount; ++i) {
                const uint32_t q = e.audioHost[i];
                const uint32_t slotInFrame = i % slotsPerFrame;
                const bool isPcmSlot = slotInFrame < pcmSlots;
                if (q == 0) {
                    sawAllZero = true;
                }
                uint8_t expectedLabel = Encoding::kAM824LabelMBLA;
                if (!isPcmSlot) {
                    const uint32_t midiSlotIndex = slotInFrame - pcmSlots;
                    expectedLabel = static_cast<uint8_t>(
                        Encoding::kAM824LabelMIDIConformantBase + (midiSlotIndex & 0x03u));
                }
                if (!ASFW::Isoch::TxVerify::HasValidAM824Label(q, expectedLabel)) {
                    if (!sawInvalidLabel) {
                        badLabel = ASFW::Isoch::TxVerify::AM824LabelByte(q);
                        badWord = q;
                    }
                    sawInvalidLabel = true;
                    if (q != 0) {
                        sawInvalidLabelNonZero = true;
                    }
                }
                if (isPcmSlot && q != silenceHost) {
                    allSilence = false;
                }
            }

            const auto audWire = [&](uint32_t i) noexcept -> uint32_t {
                if (i >= e.audioQuadletCount) return 0;
                return ASFW::Isoch::TxVerify::ByteSwap32(e.audioHost[i]);
            };
            const uint32_t q0Wire = ASFW::Isoch::TxVerify::ByteSwap32(e.cipQ0Host);
            const uint32_t q1Wire = ASFW::Isoch::TxVerify::ByteSwap32(e.cipQ1Host);

            if (sawAllZero) {
                ASFW_LOG_RL(Isoch, "txverify/all_zero", 1000, OS_LOG_TYPE_DEFAULT,
                            "IT TX VERIFY: ALL-ZERO audio quadlet(s) pkt=%u req=%u st=0x%08x cip=[%08x %08x] audWire=[%08x %08x %08x %08x %08x %08x %08x %08x]",
                            e.packetIndex, e.reqCount, e.lastDescStatus, q0Wire, q1Wire,
                            audWire(0), audWire(1), audWire(2), audWire(3),
                            audWire(4), audWire(5), audWire(6), audWire(7));
            }

            if (sawInvalidLabel) {
                ASFW_LOG_RL(Isoch, "txverify/invalid_label", 1000, OS_LOG_TYPE_DEFAULT,
                            "IT TX VERIFY: invalid AM824 label pkt=%u label=0x%02x wordHost=0x%08x cip=[%08x %08x] audWire=[%08x %08x %08x %08x %08x %08x %08x %08x]",
                            e.packetIndex, badLabel, badWord, q0Wire, q1Wire,
                            audWire(0), audWire(1), audWire(2), audWire(3),
                            audWire(4), audWire(5), audWire(6), audWire(7));
                if (sawInvalidLabelNonZero) {
                    restartReasons |= IsochTxRecoveryController::kReasonInvalidLabel;
                }
            }

            if (allSilence) {
                ++state_.silentDataRun;
            } else {
                state_.silentDataRun = 0;
            }

            if (state_.silentDataRun >= 8) {
                const bool shouldHaveAudio =
                    (!inputs_.zeroCopyEnabled && inputs_.sharedTxQueueValid &&
                     inputs_.sharedTxQueueFillFrames >= inputs_.framesPerPacket) &&
                    (deltaMissed == 0);
                if (shouldHaveAudio) {
                    ASFW_LOG_RL(Isoch, "txverify/silence_run", 10000, OS_LOG_TYPE_DEFAULT,
                                "IT TX VERIFY: SUSPICIOUS SILENCE RUN len=%u pkt=%u qFill=%u framesPerPkt=%u",
                                state_.silentDataRun, e.packetIndex, inputs_.sharedTxQueueFillFrames, inputs_.framesPerPacket);
                }
            }
        }
    }

    if (restartReasons && recovery_) {
        recovery_->Request(restartReasons);
    }
}

} // namespace ASFW::Isoch
