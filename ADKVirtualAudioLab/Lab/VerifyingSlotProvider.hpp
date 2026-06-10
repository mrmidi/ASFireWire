#pragma once

#include "../Ports/IAmdtpTxSlotProvider.hpp"
#include "../Ports/IDiagSink.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Lab {

// Counter ids reported by VerifyingSlotProvider (totals + one sticky counter
// per invariant violation kind). Ids are sparse on purpose: the tens digit
// groups them by the README invariant they belong to (P1/P2/P3/P4), leaving
// room to add kinds without renumbering. They double as Ports::IDiagSink ids.
enum class VerifierCounterId : uint32_t {
    // Totals
    kPacketsPublished = 0,
    kDataPackets = 1,
    kNoDataPackets = 2,
    kAcquireCalls = 3,
    kAcquireFailures = 4,

    // P1 — cadence / stream shape
    kP1CadenceWindowViolation = 10, // tumbling 8000-packet window != 6000 data
    kP1CadenceRunViolation = 11,    // >3 consecutive data or >1 consecutive no-data
    kP1PacketIndexGapViolation = 12, // packetIndex != previous + 1

    // P2 — DBC continuity
    kP2DbcViolation = 20, // dbc != running expectation (no-data must carry unchanged)

    // P3 — CIP bit-exactness / packet structure
    kP3ByteCountViolation = 30,        // byteCount vs kind/frames/dbs (or > capacity)
    kP3CipQ0Violation = 31,            // sid/dbs/fn/qpc/sph/reserved/dbc fields
    kP3CipQ1Violation = 32,            // eoh/fmt/fdf/syt fields
    kP3UnacquiredPublishViolation = 33, // publish without a matching live acquire

    // P4 — frame tiling
    kP4FrameTilingViolation = 40, // firstAudioFrame != expected next frame
    kP4FrameCountViolation = 41,  // data frame count inconsistent / no-data frames != 0

    // P5 — SYT discipline (Milestone 2; checked only when Config::p5Enabled)
    kP5SytStepViolation = 50,  // data SYT not prev + step in the 16-cycle domain
    kP5SytGraftViolation = 51, // sub-cycle offset off the device graft lattice

    kIdLimit = 52,
};

struct VerifierSnapshot final {
    uint64_t counters[static_cast<uint32_t>(VerifierCounterId::kIdLimit)]{};

    bool firstViolationValid{false};
    uint32_t firstViolationId{0};
    uint64_t firstViolationPacketIndex{0};

    [[nodiscard]] uint64_t Value(VerifierCounterId id) const noexcept {
        return counters[static_cast<uint32_t>(id)];
    }

    [[nodiscard]] uint64_t TotalViolations() const noexcept {
        uint64_t sum = 0;
        for (uint32_t id = 10;
             id < static_cast<uint32_t>(VerifierCounterId::kIdLimit); ++id) {
            sum += counters[id];
        }
        return sum;
    }
};

// The Step 6 instrument (see README, "Key design decisions"): a decorator
// wrapping ANY IAmdtpTxSlotProvider that asserts the P1–P4 invariants on
// every PublishSlot and records violations as sticky counters — never
// logging, never blocking, never altering the wrapped provider's behavior.
// The same object is meant to run in three contexts: host tests
// (Verifying(Fake)), the lab dext under real HAL pacing, and later ASFW
// bring-up (Verifying(RealDmaRing)).
//
// Design decisions:
//
// 1. Observer, not gate: packets are forwarded to the inner provider even
//    when they violate an invariant. The lab needs the downstream effects of
//    a bad packet to stay observable; rejecting would also make the
//    decorator change behavior, which it must never do.
// 2. Resync-after-violation: every continuity check (index, DBC, frame
//    tiling) re-anchors its expectation on the observed value after a
//    violation, so one corruption increments exactly one counter instead of
//    cascading — that keeps counter signatures readable as diagnostics.
// 3. Structural constants are checked, stream constants are learned: FMT,
//    the 48 kHz data FDF, the no-data rules, and the metadata↔bytes
//    agreement are asserted outright; SID and frames-per-data-packet are
//    learned from the first packet and then held constant. The verifier
//    deliberately re-derives nothing from the packetizer's internals — it
//    checks observable wire facts, not a second copy of the implementation.
// 4. Acquire tracking mirrors publish-at-prepare: AcquireWritableSlot
//    records (index, bytes, capacity) in a small ring keyed by
//    packetIndex % kTrackedSlots; PublishSlot consumes the entry to parse
//    the wire image. kTrackedSlots mirrors the IT ring geometry (256).
// 5. All counters are relaxed atomics and every increment is mirrored to an
//    optional Ports::IDiagSink — RT-safe by construction.
class VerifyingSlotProvider final
    : public Protocols::Audio::AMDTP::IAmdtpTxSlotProvider {
public:
    static constexpr uint32_t kTrackedSlots = 256;
    static constexpr uint32_t kCadenceWindowPackets = 8000;
    static constexpr uint32_t kCadenceWindowDataPackets = 6000;
    static constexpr uint32_t kMaxConsecutiveData = 3;
    static constexpr uint32_t kMaxConsecutiveNoData = 1;

    struct Config final {
        bool blockingMode{true};      // enables P1 run-length + 8000-window checks
        uint8_t expectedDataFdf{0x02}; // AM824 | 48 kHz (the lab is 48 k-only)
        Ports::IDiagSink* diagSink{nullptr};

        // P5 (Milestone 2): only meaningful for timing-valid runs — data
        // packets must then carry real SYTs stepping by p5StepTicks in the
        // 16-cycle domain, on the device's constant sub-cycle graft lattice
        // (offset ≡ graft mod 1024 at 48 kHz blocking: 4096 % 3072 = 1024).
        bool p5Enabled{false};
        uint16_t p5GraftOffsetTicks{0x0B0};
        uint32_t p5StepTicks{4096};
    };

    explicit VerifyingSlotProvider(
        Protocols::Audio::AMDTP::IAmdtpTxSlotProvider& inner) noexcept;

    VerifyingSlotProvider(Protocols::Audio::AMDTP::IAmdtpTxSlotProvider& inner,
                          const Config& config) noexcept;

    void Configure(const Config& config) noexcept;

    // Clears counters and all learned/continuity state. Call alongside the
    // engine's ResetForStart when a scenario restarts the stream.
    void Reset() noexcept;

    bool AcquireWritableSlot(
        uint32_t packetIndex,
        Protocols::Audio::AMDTP::TxPacketSlotView& outSlot) noexcept override;

    void PublishSlot(
        const Protocols::Audio::AMDTP::PreparedTxPacket& packet) noexcept override;

    uint32_t SlotCount() const noexcept override;

    [[nodiscard]] VerifierSnapshot Snapshot() const noexcept;

private:
    struct TrackedSlot final {
        uint64_t packetIndex{0};
        const uint8_t* bytes{nullptr};
        uint32_t capacityBytes{0};
        bool valid{false};
        bool published{false};
    };

    void Count(VerifierCounterId id, uint64_t delta = 1) noexcept;
    void Violation(VerifierCounterId id, uint64_t packetIndex) noexcept;

    void CheckStructure(const Protocols::Audio::AMDTP::PreparedTxPacket& packet,
                        const TrackedSlot* tracked) noexcept;
    void CheckDbcContinuity(
        const Protocols::Audio::AMDTP::PreparedTxPacket& packet) noexcept;
    void CheckFrameTiling(
        const Protocols::Audio::AMDTP::PreparedTxPacket& packet) noexcept;
    void CheckCadence(
        const Protocols::Audio::AMDTP::PreparedTxPacket& packet) noexcept;
    void CheckSytDiscipline(
        const Protocols::Audio::AMDTP::PreparedTxPacket& packet) noexcept;

    Protocols::Audio::AMDTP::IAmdtpTxSlotProvider* inner_{nullptr};
    Config config_{};

    TrackedSlot trackedSlots_[kTrackedSlots]{};

    // Continuity state (single writer: the IO/pump thread).
    bool packetIndexValid_{false};
    uint32_t lastPacketIndex_{0};

    bool dbcValid_{false};
    uint8_t expectedDbc_{0};

    bool nextFrameValid_{false};
    uint64_t expectedNextFrame_{0};

    bool sidValid_{false};
    uint8_t learnedSid_{0};

    bool framesPerDataValid_{false};
    uint32_t learnedFramesPerData_{0};

    uint32_t consecutiveData_{0};
    uint32_t consecutiveNoData_{0};

    uint32_t windowPackets_{0};
    uint32_t windowDataPackets_{0};

    bool p5PrevValid_{false};
    uint32_t p5PrevDomainTick_{0};

    std::atomic<uint64_t>
        counters_[static_cast<uint32_t>(VerifierCounterId::kIdLimit)]{};
    std::atomic<bool> firstViolationValid_{false};
    std::atomic<uint32_t> firstViolationId_{0};
    std::atomic<uint64_t> firstViolationPacketIndex_{0};
};

} // namespace ASFW::Lab
