#include "VerifyingSlotProvider.hpp"

namespace ASFW::Lab {

// Wire-facts checked here (CIP layout per IEC 61883-1, cross-checked against
// CipHeader.cpp and the Linux/FFADO golden rules recorded in the README):
//
//   Q0: [31:30]=00  [29:24]=SID  [23:16]=DBS  [15:8]=FN/QPC/SPH/rsv (all 0
//       for AM824)  [7:0]=DBC
//   Q1: [31:30]=10 (EOH)  [29:24]=FMT=0x10  [23:16]=FDF  [15:0]=SYT
//
//   Data packet:    byteCount == 8 + framesInPacket * dbs * 4, FDF == 0x02
//                   (48 kHz), SYT field == packet.syt verbatim (0xFFFF is
//                   legal while the TX clock is invalid — Milestone 1).
//   No-data packet: CIP-header-only (byteCount == 8), FDF == 0xFF,
//                   SYT == 0xFFFF, DBC carried unchanged, zero frames.

namespace {

using Protocols::Audio::AMDTP::PreparedTxPacket;
using Protocols::Audio::AMDTP::TxPacketSlotView;

constexpr uint32_t kCipHeaderBytes = 8;
constexpr uint32_t kBytesPerSlot = 4;
constexpr uint8_t kFdfNoData = 0xFF;
constexpr uint16_t kSytNoInfo = 0xFFFF;
constexpr uint8_t kFmtAm824 = 0x10;

inline uint32_t ReadBE32(const uint8_t* src) noexcept {
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) | static_cast<uint32_t>(src[3]);
}

} // namespace

VerifyingSlotProvider::VerifyingSlotProvider(
    Protocols::Audio::AMDTP::IAmdtpTxSlotProvider& inner) noexcept
    : inner_(&inner) {}

VerifyingSlotProvider::VerifyingSlotProvider(
    Protocols::Audio::AMDTP::IAmdtpTxSlotProvider& inner,
    const Config& config) noexcept
    : inner_(&inner), config_(config) {}

void VerifyingSlotProvider::Configure(const Config& config) noexcept {
    config_ = config;
    Reset();
}

void VerifyingSlotProvider::Reset() noexcept {
    for (auto& tracked : trackedSlots_) {
        tracked = TrackedSlot{};
    }

    packetIndexValid_ = false;
    lastPacketIndex_ = 0;
    dbcValid_ = false;
    expectedDbc_ = 0;
    nextFrameValid_ = false;
    expectedNextFrame_ = 0;
    sidValid_ = false;
    learnedSid_ = 0;
    framesPerDataValid_ = false;
    learnedFramesPerData_ = 0;
    consecutiveData_ = 0;
    consecutiveNoData_ = 0;
    windowPackets_ = 0;
    windowDataPackets_ = 0;

    for (auto& counter : counters_) {
        counter.store(0, std::memory_order_relaxed);
    }
    firstViolationValid_.store(false, std::memory_order_relaxed);
    firstViolationId_.store(0, std::memory_order_relaxed);
    firstViolationPacketIndex_.store(0, std::memory_order_relaxed);
}

bool VerifyingSlotProvider::AcquireWritableSlot(uint32_t packetIndex,
                                                TxPacketSlotView& outSlot) noexcept {
    Count(VerifierCounterId::kAcquireCalls);

    if (inner_ == nullptr || !inner_->AcquireWritableSlot(packetIndex, outSlot)) {
        Count(VerifierCounterId::kAcquireFailures);
        return false;
    }

    TrackedSlot& tracked = trackedSlots_[packetIndex % kTrackedSlots];
    tracked.packetIndex = packetIndex;
    tracked.bytes = outSlot.bytes;
    tracked.capacityBytes = outSlot.capacityBytes;
    tracked.valid = true;
    tracked.published = false;
    return true;
}

void VerifyingSlotProvider::PublishSlot(const PreparedTxPacket& packet) noexcept {
    Count(VerifierCounterId::kPacketsPublished);
    Count(packet.isData ? VerifierCounterId::kDataPackets
                        : VerifierCounterId::kNoDataPackets);

    // Packet-index contiguity (resync on violation so one gap counts once).
    if (packetIndexValid_ && packet.packetIndex != lastPacketIndex_ + 1) {
        Violation(VerifierCounterId::kP1PacketIndexGapViolation,
                  packet.packetIndex);
    }
    lastPacketIndex_ = packet.packetIndex;
    packetIndexValid_ = true;

    // Wire-image lookup from the acquire ring.
    TrackedSlot& tracked = trackedSlots_[packet.packetIndex % kTrackedSlots];
    const bool trackedMatches = tracked.valid &&
                                tracked.packetIndex == packet.packetIndex &&
                                tracked.bytes != nullptr && !tracked.published;
    if (!trackedMatches) {
        Violation(VerifierCounterId::kP3UnacquiredPublishViolation,
                  packet.packetIndex);
    } else {
        tracked.published = true;
    }

    CheckStructure(packet, trackedMatches ? &tracked : nullptr);
    CheckDbcContinuity(packet);
    CheckFrameTiling(packet);
    if (config_.blockingMode) {
        CheckCadence(packet);
    }

    if (inner_ != nullptr) {
        inner_->PublishSlot(packet);
    }
}

uint32_t VerifyingSlotProvider::SlotCount() const noexcept {
    return (inner_ != nullptr) ? inner_->SlotCount() : 0;
}

VerifierSnapshot VerifyingSlotProvider::Snapshot() const noexcept {
    VerifierSnapshot snapshot{};
    for (uint32_t id = 0; id < static_cast<uint32_t>(VerifierCounterId::kIdLimit);
         ++id) {
        snapshot.counters[id] = counters_[id].load(std::memory_order_relaxed);
    }
    snapshot.firstViolationValid =
        firstViolationValid_.load(std::memory_order_relaxed);
    snapshot.firstViolationId = firstViolationId_.load(std::memory_order_relaxed);
    snapshot.firstViolationPacketIndex =
        firstViolationPacketIndex_.load(std::memory_order_relaxed);
    return snapshot;
}

void VerifyingSlotProvider::Count(VerifierCounterId id, uint64_t delta) noexcept {
    counters_[static_cast<uint32_t>(id)].fetch_add(delta,
                                                   std::memory_order_relaxed);
    if (config_.diagSink != nullptr) {
        config_.diagSink->Increment(static_cast<uint32_t>(id), delta);
    }
}

void VerifyingSlotProvider::Violation(VerifierCounterId id,
                                      uint64_t packetIndex) noexcept {
    Count(id);
    if (!firstViolationValid_.load(std::memory_order_relaxed)) {
        firstViolationId_.store(static_cast<uint32_t>(id),
                                std::memory_order_relaxed);
        firstViolationPacketIndex_.store(packetIndex, std::memory_order_relaxed);
        firstViolationValid_.store(true, std::memory_order_relaxed);
    }
}

void VerifyingSlotProvider::CheckStructure(const PreparedTxPacket& packet,
                                           const TrackedSlot* tracked) noexcept {
    // Byte-count vs packet kind (and vs the acquired capacity when known).
    const uint32_t expectedBytes =
        packet.isData ? (kCipHeaderBytes +
                         packet.framesInPacket * packet.dbs * kBytesPerSlot)
                      : kCipHeaderBytes;
    bool byteCountOk = (packet.byteCount == expectedBytes);
    if (tracked != nullptr && packet.byteCount > tracked->capacityBytes) {
        byteCountOk = false;
    }
    if (!byteCountOk) {
        Violation(VerifierCounterId::kP3ByteCountViolation, packet.packetIndex);
    }

    if (tracked == nullptr || tracked->capacityBytes < kCipHeaderBytes) {
        return; // no wire image to parse; the unacquired violation already fired
    }

    const uint32_t q0 = ReadBE32(tracked->bytes);
    const uint32_t q1 = ReadBE32(tracked->bytes + 4);

    // Q0: [31:30]=00, SID stable, DBS == metadata, FN/QPC/SPH/rsv all zero,
    // DBC == metadata.
    const uint8_t q0Sid = static_cast<uint8_t>((q0 >> 24) & 0x3F);
    bool q0Ok = ((q0 >> 30) == 0u) &&
                (((q0 >> 16) & 0xFFu) == packet.dbs) &&
                (((q0 >> 8) & 0xFFu) == 0u) &&
                ((q0 & 0xFFu) == packet.dbc);
    if (q0Ok) {
        if (!sidValid_) {
            learnedSid_ = q0Sid;
            sidValid_ = true;
        } else if (q0Sid != learnedSid_) {
            q0Ok = false;
        }
    }
    if (!q0Ok) {
        Violation(VerifierCounterId::kP3CipQ0Violation, packet.packetIndex);
    }

    // Q1: EOH '10', FMT 0x10, FDF per kind, SYT field == metadata (and the
    // no-data rules: FDF 0xFF + SYT 0xFFFF).
    const uint8_t q1Fdf = static_cast<uint8_t>((q1 >> 16) & 0xFF);
    const uint16_t q1Syt = static_cast<uint16_t>(q1 & 0xFFFF);
    bool q1Ok = ((q1 >> 30) == 0b10u) &&
                (((q1 >> 24) & 0x3Fu) == kFmtAm824) &&
                (q1Syt == packet.syt);
    if (packet.isData) {
        q1Ok = q1Ok && (q1Fdf == config_.expectedDataFdf);
    } else {
        q1Ok = q1Ok && (q1Fdf == kFdfNoData) && (q1Syt == kSytNoInfo);
    }
    if (!q1Ok) {
        Violation(VerifierCounterId::kP3CipQ1Violation, packet.packetIndex);
    }
}

void VerifyingSlotProvider::CheckDbcContinuity(
    const PreparedTxPacket& packet) noexcept {
    // Golden rule: every packet carries the running DBC; only data packets
    // advance it (by their data-block count == framesInPacket for AM824).
    if (dbcValid_ && packet.dbc != expectedDbc_) {
        Violation(VerifierCounterId::kP2DbcViolation, packet.packetIndex);
    }
    expectedDbc_ = packet.isData
                       ? static_cast<uint8_t>(packet.dbc + packet.framesInPacket)
                       : packet.dbc;
    dbcValid_ = true;
}

void VerifyingSlotProvider::CheckFrameTiling(
    const PreparedTxPacket& packet) noexcept {
    if (packet.isData) {
        if (packet.framesInPacket == 0) {
            Violation(VerifierCounterId::kP4FrameCountViolation,
                      packet.packetIndex);
        } else if (!framesPerDataValid_) {
            learnedFramesPerData_ = packet.framesInPacket;
            framesPerDataValid_ = true;
        } else if (packet.framesInPacket != learnedFramesPerData_) {
            Violation(VerifierCounterId::kP4FrameCountViolation,
                      packet.packetIndex);
        }
    } else if (packet.framesInPacket != 0) {
        Violation(VerifierCounterId::kP4FrameCountViolation, packet.packetIndex);
    }

    // Both kinds must sit at the gapless frame cursor; only data advances it.
    if (nextFrameValid_ && packet.firstAudioFrame != expectedNextFrame_) {
        Violation(VerifierCounterId::kP4FrameTilingViolation, packet.packetIndex);
    }
    expectedNextFrame_ = packet.firstAudioFrame + packet.framesInPacket;
    nextFrameValid_ = true;
}

void VerifyingSlotProvider::CheckCadence(const PreparedTxPacket& packet) noexcept {
    // Run-length shape of the blocking 48 kHz N,D,D,D pattern: data runs of
    // at most 3, isolated no-data packets.
    if (packet.isData) {
        ++consecutiveData_;
        consecutiveNoData_ = 0;
        if (consecutiveData_ > kMaxConsecutiveData) {
            Violation(VerifierCounterId::kP1CadenceRunViolation,
                      packet.packetIndex);
        }
    } else {
        ++consecutiveNoData_;
        consecutiveData_ = 0;
        if (consecutiveNoData_ > kMaxConsecutiveNoData) {
            Violation(VerifierCounterId::kP1CadenceRunViolation,
                      packet.packetIndex);
        }
    }

    // Tumbling 8000-packet window: exactly 6000 data packets per window.
    ++windowPackets_;
    if (packet.isData) {
        ++windowDataPackets_;
    }
    if (windowPackets_ == kCadenceWindowPackets) {
        if (windowDataPackets_ != kCadenceWindowDataPackets) {
            Violation(VerifierCounterId::kP1CadenceWindowViolation,
                      packet.packetIndex);
        }
        windowPackets_ = 0;
        windowDataPackets_ = 0;
    }
}

} // namespace ASFW::Lab
