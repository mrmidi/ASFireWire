#include "TestHarness.hpp"

#include "../Lab/FakeIsochTxSlotProvider.hpp"
#include "../Lab/StickyCounterSink.hpp"
#include "../Lab/VerifyingSlotProvider.hpp"
#include "../Protocols/Audio/IEC61883/CipHeader.hpp"

#include <functional>

// Self-tests for the Step 6 instrument: a verifier that cannot catch
// violations is worse than no verifier, so every violation kind gets a
// corruption case proving it fires — and, thanks to resync-after-violation,
// fires exactly once per injected fault, with no cross-talk into the other
// counters. Sequences are produced by a tiny hand-rolled golden driver
// (deliberately independent of AmdtpTxPacketizer: the instrument must not be
// validated against the code it will later judge).

namespace ASFW::LabTests {

using Lab::FakeIsochTxSlotProvider;
using Lab::StickyCounterSink;
using Lab::VerifierCounterId;
using Lab::VerifierSnapshot;
using Lab::VerifyingSlotProvider;
using Protocols::Audio::AMDTP::PreparedTxPacket;
using Protocols::Audio::AMDTP::TxPacketSlotView;
using Protocols::Audio::IEC61883::CipHeaderBuilder;
using Protocols::Audio::IEC61883::CipHeaderConfig;

namespace {

constexpr uint8_t kDbs = 2;
constexpr uint8_t kFramesPerData = 8;

inline void WriteBE32(uint8_t* dest, uint32_t value) noexcept {
    dest[0] = static_cast<uint8_t>(value >> 24);
    dest[1] = static_cast<uint8_t>(value >> 16);
    dest[2] = static_cast<uint8_t>(value >> 8);
    dest[3] = static_cast<uint8_t>(value);
}

// Mutator hook applied after the golden packet is built, before publish.
using Mutator = std::function<void(PreparedTxPacket&, uint8_t*)>;

// Hand-rolled producer of golden AMDTP blocking-48k sequences. EmitAuto()
// follows the N,D,D,D cadence (6 frames pending per cycle, emit 8 when >= 8);
// EmitExplicit() lets a test script an arbitrary data/no-data shape while the
// driver keeps DBC and frame tiling consistent.
struct GoldenDriver final {
    explicit GoldenDriver(VerifyingSlotProvider& verifier) noexcept
        : verifier_(verifier) {
        CipHeaderConfig config{};
        config.sid = 0;
        config.dbs = kDbs;
        config.fmt = 0x10;
        config.fdf = 0x02;
        cip_.Configure(config);
    }

    bool EmitExplicit(bool isData, const Mutator& mutate = nullptr) noexcept {
        TxPacketSlotView slot{};
        if (!verifier_.AcquireWritableSlot(index_, slot)) {
            return false;
        }

        const auto words = isData ? cip_.BuildData(dbc_, 0xFFFF)
                                  : cip_.BuildNoData(dbc_);
        WriteBE32(slot.bytes, words.q0);
        WriteBE32(slot.bytes + 4, words.q1);

        const uint32_t payloadBytes =
            isData ? kFramesPerData * kDbs * 4u : 0u;
        for (uint32_t i = 0; i < payloadBytes; ++i) {
            slot.bytes[8 + i] = 0;
        }

        PreparedTxPacket packet{};
        packet.packetIndex = index_;
        packet.byteCount = 8 + payloadBytes;
        packet.isData = isData;
        packet.dbc = dbc_;
        packet.syt = 0xFFFF;
        packet.firstAudioFrame = frame_;
        packet.framesInPacket = isData ? kFramesPerData : 0;
        packet.dbs = kDbs;

        if (mutate) {
            mutate(packet, slot.bytes);
        }

        verifier_.PublishSlot(packet);

        ++index_;
        if (isData) {
            dbc_ = static_cast<uint8_t>(dbc_ + kFramesPerData);
            frame_ += kFramesPerData;
        }
        return true;
    }

    bool EmitAuto(const Mutator& mutate = nullptr) noexcept {
        const bool isData = (pending_ + 6) >= 8;
        if (!EmitExplicit(isData, mutate)) {
            return false;
        }
        pending_ = static_cast<uint8_t>(pending_ + 6 - (isData ? 8 : 0));
        return true;
    }

    VerifyingSlotProvider& verifier_;
    CipHeaderBuilder cip_{};
    uint32_t index_{0};
    uint8_t pending_{0};
    uint8_t dbc_{0};
    uint64_t frame_{0};
};

// Runs warmup, one corrupted emit, cooldown; checks the targeted counter
// fired exactly `expected` times and nothing else did.
void RunSingleFaultCase(TestContext& ctx, VerifierCounterId targetId,
                        uint64_t expected,
                        const std::function<void(GoldenDriver&)>& script) {
    FakeIsochTxSlotProvider fake{};
    VerifyingSlotProvider verifier{fake};
    GoldenDriver driver{verifier};

    for (int i = 0; i < 41; ++i) {
        CHECK(ctx, driver.EmitAuto());
    }

    script(driver);

    for (int i = 0; i < 40; ++i) {
        CHECK(ctx, driver.EmitAuto());
    }

    const VerifierSnapshot snapshot = verifier.Snapshot();
    CHECK_EQ_U64(ctx, snapshot.Value(targetId), expected);
    CHECK_EQ_U64(ctx, snapshot.TotalViolations(), expected);
    CHECK(ctx, snapshot.firstViolationValid);
    CHECK_EQ_U32(ctx, snapshot.firstViolationId,
                 static_cast<uint32_t>(targetId));
}

} // namespace

void RunVerifyingSlotProviderTests(TestContext& ctx) {
    // Clean 100k-cycle run: zero violations, exact N,D,D,D bookkeeping.
    {
        FakeIsochTxSlotProvider fake{};
        StickyCounterSink sink{};
        VerifyingSlotProvider verifier{
            fake, VerifyingSlotProvider::Config{true, 0x02, &sink}};
        GoldenDriver driver{verifier};

        bool emitted = true;
        for (int i = 0; i < 100000; ++i) {
            emitted = driver.EmitAuto() && emitted;
        }
        CHECK(ctx, emitted);

        const VerifierSnapshot snapshot = verifier.Snapshot();
        CHECK_EQ_U64(ctx, snapshot.TotalViolations(), 0);
        CHECK(ctx, !snapshot.firstViolationValid);
        CHECK_EQ_U64(ctx, snapshot.Value(VerifierCounterId::kPacketsPublished),
                     100000);
        CHECK_EQ_U64(ctx, snapshot.Value(VerifierCounterId::kDataPackets), 75000);
        CHECK_EQ_U64(ctx, snapshot.Value(VerifierCounterId::kNoDataPackets),
                     25000);
        CHECK_EQ_U64(ctx, snapshot.Value(VerifierCounterId::kAcquireCalls),
                     100000);
        CHECK_EQ_U64(ctx, snapshot.Value(VerifierCounterId::kAcquireFailures), 0);

        // The diag sink mirrors the counters one-for-one.
        CHECK_EQ_U64(ctx,
                     sink.Value(static_cast<uint32_t>(
                         VerifierCounterId::kPacketsPublished)),
                     100000);
        CHECK_EQ_U64(ctx, sink.OverflowedIds(), 0);
    }

    // P2 — a data packet jumps the DBC (persistently, as a real bug would).
    RunSingleFaultCase(ctx, VerifierCounterId::kP2DbcViolation, 1,
                       [](GoldenDriver& driver) {
                           driver.dbc_ = static_cast<uint8_t>(driver.dbc_ + 4);
                           driver.EmitAuto();
                       });

    // P2 — a no-data packet advances the DBC it must carry unchanged.
    RunSingleFaultCase(ctx, VerifierCounterId::kP2DbcViolation, 1,
                       [](GoldenDriver& driver) {
                           while ((driver.pending_ + 6) >= 8) {
                               driver.EmitAuto(); // reach the no-data cycle
                           }
                           driver.dbc_ = static_cast<uint8_t>(driver.dbc_ + 8);
                           driver.EmitAuto();
                       });

    // P3 — data byte count disagrees with frames * dbs.
    RunSingleFaultCase(ctx, VerifierCounterId::kP3ByteCountViolation, 1,
                       [](GoldenDriver& driver) {
                           while ((driver.pending_ + 6) < 8) {
                               driver.EmitAuto(); // reach a data cycle
                           }
                           driver.EmitAuto([](PreparedTxPacket& packet, uint8_t*) {
                               packet.byteCount -= 4;
                           });
                       });

    // P3 — no-data packet claims payload bytes (must be CIP-header-only).
    RunSingleFaultCase(ctx, VerifierCounterId::kP3ByteCountViolation, 1,
                       [](GoldenDriver& driver) {
                           while ((driver.pending_ + 6) >= 8) {
                               driver.EmitAuto();
                           }
                           driver.EmitAuto([](PreparedTxPacket& packet, uint8_t*) {
                               packet.byteCount = 16;
                           });
                       });

    // P3 — wrong FDF on the wire (Q1).
    RunSingleFaultCase(ctx, VerifierCounterId::kP3CipQ1Violation, 1,
                       [](GoldenDriver& driver) {
                           while ((driver.pending_ + 6) < 8) {
                               driver.EmitAuto();
                           }
                           driver.EmitAuto([](PreparedTxPacket&, uint8_t* bytes) {
                               bytes[5] = 0x03; // FDF byte of Q1
                           });
                       });

    // P3 — wrong DBS field on the wire (Q0).
    RunSingleFaultCase(ctx, VerifierCounterId::kP3CipQ0Violation, 1,
                       [](GoldenDriver& driver) {
                           driver.EmitAuto([](PreparedTxPacket&, uint8_t* bytes) {
                               bytes[1] = kDbs + 1; // DBS byte of Q0
                           });
                       });

    // P4 — frame tiling gap (producer skipped 8 frames).
    RunSingleFaultCase(ctx, VerifierCounterId::kP4FrameTilingViolation, 1,
                       [](GoldenDriver& driver) {
                           driver.frame_ += kFramesPerData;
                           driver.EmitAuto();
                       });

    // P4 — no-data packet claims frames (consistently-buggy producer).
    RunSingleFaultCase(ctx, VerifierCounterId::kP4FrameCountViolation, 1,
                       [](GoldenDriver& driver) {
                           while ((driver.pending_ + 6) >= 8) {
                               driver.EmitAuto();
                           }
                           driver.EmitAuto([](PreparedTxPacket& packet, uint8_t*) {
                               packet.framesInPacket = kFramesPerData;
                           });
                           driver.frame_ += kFramesPerData; // producer believes it
                       });

    // P1 — two consecutive no-data packets break the N,D,D,D run shape.
    RunSingleFaultCase(ctx, VerifierCounterId::kP1CadenceRunViolation, 1,
                       [](GoldenDriver& driver) {
                           while ((driver.pending_ + 6) >= 8) {
                               driver.EmitAuto();
                           }
                           driver.EmitAuto();           // the legitimate no-data
                           driver.EmitExplicit(false);   // and an illegal twin
                           driver.pending_ = 2;          // re-phase: next is D,D,D,N
                       });

    // P1 — packet index gap.
    RunSingleFaultCase(ctx, VerifierCounterId::kP1PacketIndexGapViolation, 1,
                       [](GoldenDriver& driver) {
                           driver.index_ += 1;
                           driver.EmitAuto();
                       });

    // P3 — publish without a matching acquire.
    RunSingleFaultCase(
        ctx, VerifierCounterId::kP3UnacquiredPublishViolation, 1,
        [](GoldenDriver& driver) {
            while ((driver.pending_ + 6) >= 8) {
                driver.EmitAuto(); // park on a no-data cycle boundary
            }
            PreparedTxPacket packet{};
            packet.packetIndex = driver.index_;
            packet.byteCount = 8;
            packet.isData = false;
            packet.dbc = driver.dbc_;
            packet.syt = 0xFFFF;
            packet.firstAudioFrame = driver.frame_;
            packet.framesInPacket = 0;
            packet.dbs = kDbs;
            driver.verifier_.PublishSlot(packet); // never acquired
            ++driver.index_;
            driver.pending_ =
                static_cast<uint8_t>(driver.pending_ + 6); // it was the N slot
        });

    // P1 — tumbling 8000-packet window with one data packet short (runs stay
    // legal: the tail is rescripted N,D,D,N instead of N,D,D,D).
    {
        FakeIsochTxSlotProvider fake{};
        VerifyingSlotProvider verifier{fake};
        GoldenDriver driver{verifier};

        for (int i = 0; i < 7996; ++i) {
            CHECK(ctx, driver.EmitAuto());
        }
        CHECK(ctx, driver.EmitExplicit(false));
        CHECK(ctx, driver.EmitExplicit(true));
        CHECK(ctx, driver.EmitExplicit(true));
        CHECK(ctx, driver.EmitExplicit(false));

        const VerifierSnapshot snapshot = verifier.Snapshot();
        CHECK_EQ_U64(ctx, snapshot.Value(VerifierCounterId::kPacketsPublished),
                     8000);
        CHECK_EQ_U64(ctx, snapshot.Value(VerifierCounterId::kDataPackets), 5999);
        CHECK_EQ_U64(
            ctx, snapshot.Value(VerifierCounterId::kP1CadenceWindowViolation), 1);
        CHECK_EQ_U64(ctx, snapshot.TotalViolations(), 1);
    }

    // Reset() clears counters and re-arms the learned state.
    {
        FakeIsochTxSlotProvider fake{};
        VerifyingSlotProvider verifier{fake};
        {
            GoldenDriver driver{verifier};
            driver.index_ += 5; // guarantees an index-gap-free fresh start later
            for (int i = 0; i < 20; ++i) {
                CHECK(ctx, driver.EmitAuto());
            }
        }
        verifier.Reset();
        {
            GoldenDriver driver{verifier};
            for (int i = 0; i < 20; ++i) {
                CHECK(ctx, driver.EmitAuto());
            }
            const VerifierSnapshot snapshot = verifier.Snapshot();
            CHECK_EQ_U64(ctx, snapshot.TotalViolations(), 0);
            CHECK_EQ_U64(ctx,
                         snapshot.Value(VerifierCounterId::kPacketsPublished), 20);
        }
    }
}

} // namespace ASFW::LabTests
