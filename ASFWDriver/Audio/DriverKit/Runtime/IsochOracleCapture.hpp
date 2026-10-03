#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

// Bounded, diagnostic-only capture of the first packets of a stream start, in
// the minimal dimension a passive bus capture recorded for the official
// driver: DATA/NO-DATA, wire length, DBC, first SPH and an OHCI cycle per
// packet, on both directions.  It exists to answer one question offline — does
// ASFW's start differ from the official driver's in that dimension — and deliberately
// carries no PCM.
//
// The capture never gates, delays or reorders anything: each direction appends
// one fixed-size record per packet until its limit, then freezes.  After the
// freeze the only remaining cost is one relaxed counter increment per packet.
inline constexpr uint32_t kIsochOracleCaptureRecords = 4096;

enum class IsochOracleDirection : uint32_t {
    kTx = 0, // host -> device, the oracle's ch33
    kRx = 1, // device -> host, the oracle's ch34
};

inline constexpr uint32_t kIsochOracleDirectionCount = 2;

// Presence flags.  Absence is meaningful — a record whose cycle never arrived
// must stay distinguishable from one whose cycle really was zero.
enum IsochOracleRecordFlags : uint8_t {
    kIsochOracleFlagData = 1u << 0,     // DATA packet (NO-DATA when clear)
    kIsochOracleFlagHasSph = 1u << 1,   // firstSph carries a wire value
    kIsochOracleFlagHasCycle = 1u << 2, // cycleTimestamp carries an OHCI value
    kIsochOracleFlagHasDbc = 1u << 3,   // dbc carries a wire value
    kIsochOracleFlagValidCip = 1u << 4, // decoder accepted the CIP header (RX)
};

// One packet as seen at the audio/protocol seam.  Fixed layout: it is copied
// verbatim into the export ABI, so field order and size are part of a
// contract with the offline comparison.
struct IsochOracleRecord final {
    uint64_t packetIndex{0};
    // TX: the OHCI OUTPUT_LAST completion cycle backfilled for this packet.
    // RX: the per-packet receive cycle timestamp decoded by the RX processor.
    uint32_t cycleTimestamp{0};
    uint32_t firstSph{0};
    uint16_t wireLengthBytes{0};
    uint16_t transferStatus{0}; // RX only; the oracle's xferStatus
    uint16_t residualCount{0};  // RX only; the oracle's resCount
    uint8_t dbc{0};
    uint8_t flags{0};
};

static_assert(sizeof(IsochOracleRecord) == 24,
              "IsochOracleRecord is copied verbatim into the export ABI");

// Single-writer (the direction's own packet path), multi-reader (UserClient
// export) bounded log.  `count_` is the publication point: a reader that
// observes N may read records [0, N).
//
// Backfill deliberately writes into an already-published record, so a reader
// racing the live capture can see a TX record before its completion cycle
// lands.  That is why the export is meant to run after the freeze, and why
// `Backfilled()` is reported: offline can verify completeness instead of
// assuming it.
class IsochOracleCaptureSection final {
public:
    void Reset() noexcept {
        count_.store(0, std::memory_order_relaxed);
        suppressed_.store(0, std::memory_order_relaxed);
        lostCycles_.store(0, std::memory_order_relaxed);
        backfilled_.store(0, std::memory_order_release);
    }

    // Returns false once the section is frozen.  The caller must not treat
    // that as an error: freezing at the limit is the contract.
    bool Append(const IsochOracleRecord& record) noexcept {
        const uint32_t count = count_.load(std::memory_order_relaxed);
        if (count >= kIsochOracleCaptureRecords) {
            suppressed_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        records_[count] = record;
        count_.store(count + 1, std::memory_order_release);
        return true;
    }

    // Attaches an OHCI cycle to an already-appended record.  Packet indices
    // are contiguous and ascending within a capture, so the slot is computed
    // rather than searched; a mismatch means the completion belongs outside
    // the captured window and is counted, never guessed at.
    bool BackfillCycle(uint64_t packetIndex, uint32_t cycleTimestamp) noexcept {
        const uint32_t count = count_.load(std::memory_order_acquire);
        if (count == 0) {
            lostCycles_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const uint64_t base = records_[0].packetIndex;
        if (packetIndex < base) {
            lostCycles_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const uint64_t slot = packetIndex - base;
        if (slot >= count || records_[slot].packetIndex != packetIndex) {
            lostCycles_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto& record = records_[static_cast<uint32_t>(slot)];
        record.cycleTimestamp = cycleTimestamp;
        record.flags |= kIsochOracleFlagHasCycle;
        backfilled_.fetch_add(1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool ReadRecord(uint32_t index,
                                  IsochOracleRecord& out) const noexcept {
        if (index >= count_.load(std::memory_order_acquire)) {
            return false;
        }
        out = records_[index];
        return true;
    }

    [[nodiscard]] uint32_t Count() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

    // Packet index of the newest captured record, or 0 when empty.  The drain
    // uses it to know when every captured packet has been accounted for.
    [[nodiscard]] uint64_t LastPacketIndex() const noexcept {
        const uint32_t count = count_.load(std::memory_order_acquire);
        return count == 0 ? 0 : records_[count - 1].packetIndex;
    }

    [[nodiscard]] bool Frozen() const noexcept {
        return count_.load(std::memory_order_acquire) >=
               kIsochOracleCaptureRecords;
    }

    // Packets that arrived after the freeze.  Non-zero is expected on a
    // healthy stream and is not a drop.
    [[nodiscard]] uint64_t Suppressed() const noexcept {
        return suppressed_.load(std::memory_order_relaxed);
    }

    // Completion cycles that could not be attributed to a captured record.
    // Non-zero means the window is incomplete and the offset comparison must
    // say so rather than average over the gap.
    [[nodiscard]] uint64_t LostCycles() const noexcept {
        return lostCycles_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t Backfilled() const noexcept {
        return backfilled_.load(std::memory_order_acquire);
    }

    // The transport's completion-stamp ring is small and can wrap before the
    // drain runs.  A stamp lost that way never reaches BackfillCycle, so the
    // drain reports it here instead of leaving the gap invisible.
    void NoteLostCycle() noexcept {
        lostCycles_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    IsochOracleRecord records_[kIsochOracleCaptureRecords]{};
    std::atomic<uint32_t> count_{0};
    std::atomic<uint64_t> suppressed_{0};
    std::atomic<uint64_t> lostCycles_{0};
    std::atomic<uint64_t> backfilled_{0};
};

// Both directions of one stream start, armed together so their packet indices
// and cycles share a generation and can be compared for relative phase.
class IsochOracleCapture final {
public:
    void Reset(uint64_t generation) noexcept {
        tx_.Reset();
        rx_.Reset();
        txCompletionCursor_.store(0, std::memory_order_relaxed);
        txDrainFinished_.store(false, std::memory_order_relaxed);
        generation_.store(generation, std::memory_order_release);
    }

    // Called from TX packet preparation, once per published slot, with the
    // packet's own wire facts.  The completion cycle is not known yet and is
    // attached later by BackfillTxCompletionCycle.
    void RecordTxPrepared(uint64_t packetIndex,
                          uint32_t wireLengthBytes,
                          bool isData,
                          uint8_t dbc,
                          uint32_t firstSph,
                          bool hasSph) noexcept {
        IsochOracleRecord record{};
        record.packetIndex = packetIndex;
        record.wireLengthBytes = static_cast<uint16_t>(wireLengthBytes);
        record.dbc = dbc;
        record.firstSph = hasSph ? firstSph : 0;
        record.flags = static_cast<uint8_t>(kIsochOracleFlagHasDbc |
                                            (isData ? kIsochOracleFlagData : 0u) |
                                            (hasSph ? kIsochOracleFlagHasSph : 0u));
        (void)tx_.Append(record);
    }

    void BackfillTxCompletionCycle(uint64_t packetIndex,
                                   uint32_t cycleTimestamp) noexcept {
        (void)tx_.BackfillCycle(packetIndex, cycleTimestamp);
    }

    // Called from the RX consumer for every received packet, including
    // NO-DATA and packets the decoder rejected: the oracle counts those too,
    // and a capture that silently skipped them would compare the wrong
    // cadence.
    void RecordRxPacket(uint64_t packetIndex,
                        uint32_t wireLengthBytes,
                        uint16_t transferStatus,
                        uint16_t residualCount,
                        bool isData,
                        bool hasValidCip,
                        uint8_t dbc,
                        uint32_t firstSph,
                        bool hasSph,
                        uint32_t cycleTimestamp,
                        bool hasCycle) noexcept {
        IsochOracleRecord record{};
        record.packetIndex = packetIndex;
        record.cycleTimestamp = hasCycle ? cycleTimestamp : 0;
        record.firstSph = hasSph ? firstSph : 0;
        record.wireLengthBytes = static_cast<uint16_t>(wireLengthBytes);
        record.transferStatus = transferStatus;
        record.residualCount = residualCount;
        record.dbc = dbc;
        record.flags = static_cast<uint8_t>(
            (isData ? kIsochOracleFlagData : 0u) |
            (hasSph ? kIsochOracleFlagHasSph : 0u) |
            (hasCycle ? kIsochOracleFlagHasCycle : 0u) |
            (hasValidCip ? (kIsochOracleFlagValidCip | kIsochOracleFlagHasDbc)
                         : 0u));
        (void)rx_.Append(record);
    }

    // The TX completion drain is owned by the caller that can see the
    // transport's stamp ring; the capture only remembers how far it got, so
    // this header stays free of transport types and unit-testable.
    [[nodiscard]] uint64_t TxCompletionCursor() const noexcept {
        return txCompletionCursor_.load(std::memory_order_acquire);
    }

    void SetTxCompletionCursor(uint64_t cursor) noexcept {
        txCompletionCursor_.store(cursor, std::memory_order_release);
    }

    // Completions lag preparation by hundreds of packets, so the drain must
    // outlive the TX freeze — but only until the last captured packet has
    // been accounted for.  After that the capture is inert and the drain must
    // stop, or it would keep scanning the transport ring for the life of the
    // stream.
    [[nodiscard]] bool TxCompletionDrainFinished() const noexcept {
        return txDrainFinished_.load(std::memory_order_acquire);
    }

    void NoteTxCompletionDrained(uint64_t drainedPacketIndex) noexcept {
        if (tx_.Frozen() && drainedPacketIndex >= tx_.LastPacketIndex()) {
            txDrainFinished_.store(true, std::memory_order_release);
        }
    }

    void NoteLostTxCompletion() noexcept { tx_.NoteLostCycle(); }

    [[nodiscard]] const IsochOracleCaptureSection& Tx() const noexcept {
        return tx_;
    }

    [[nodiscard]] const IsochOracleCaptureSection& Rx() const noexcept {
        return rx_;
    }

    [[nodiscard]] const IsochOracleCaptureSection& Section(
        IsochOracleDirection direction) const noexcept {
        return direction == IsochOracleDirection::kTx ? tx_ : rx_;
    }

    [[nodiscard]] uint64_t Generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Complete() const noexcept {
        return tx_.Frozen() && rx_.Frozen();
    }

private:
    IsochOracleCaptureSection tx_{};
    IsochOracleCaptureSection rx_{};
    std::atomic<uint64_t> txCompletionCursor_{0};
    std::atomic<bool> txDrainFinished_{false};
    std::atomic<uint64_t> generation_{0};
};

} // namespace ASFW::Audio::Runtime
