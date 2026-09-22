// ARStreamFuzzTests.cpp
//
// Long-run randomized model of an OHCI AR bufferFill ring (OHCI §8.4.2) driving
// the real BufferRing + ProcessARStream. The "hardware" writes a contiguous
// packet stream across buffers (packets straddle boundaries whenever they do
// not fit), publishes resCount per completed packet, and only ever writes into
// a descriptor the software has recycled. The walker is run after random
// batches of packets and every dispatched packet must match the generated
// sequence exactly — a single lost or double-counted quadlet shows up as a
// framing divergence within a few packets.
//
// Regression coverage for the 2026-09-22 CoolScan wedge: the AR request parser
// ended up one quadlet behind the true packet boundary and never recovered.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "ASFWDriver/Async/Rx/ARStreamProcessor.hpp"
#include "ASFWDriver/Hardware/OHCIDescriptors.hpp"
#include "ASFWDriver/Shared/Rings/BufferRing.hpp"
#include "ASFWDriver/Testing/FakeDMAMemory.hpp"

namespace ASFW::Testing {
namespace {

using Async::ARPacketParser;
using Async::Rx::ProcessARStream;

struct RingContext {
    Shared::BufferRing& ring;
    std::optional<Shared::FilledBufferInfo> Dequeue() { return ring.Dequeue(); }
    kern_return_t Recycle(size_t index) { return ring.Recycle(index); }
    kern_return_t CommitConsumed(size_t index, size_t bytes) { return ring.CommitConsumed(index, bytes); }
    size_t CopyReadableBytes(std::span<uint8_t> d) { return ring.CopyReadableBytes(d); }
    kern_return_t ConsumeReadableBytes(size_t bytes) { return ring.ConsumeReadableBytes(bytes); }
};

void PutLE(std::vector<uint8_t>& b, uint32_t w) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(w >> (8 * i)));
}

struct GenPacket {
    uint8_t tCode;
    uint32_t seq;
    size_t dataLength;
    std::vector<uint8_t> bytes;
};

// Realistic SBP-2 inbound mix as seen on the AR request ring: ORB fetch (read
// block request, 20 B), status block write (28 B), data-phase block writes
// (16 + payload + 4), read quadlet request (16 B).
GenPacket MakePacket(std::mt19937& rng, uint32_t seq) {
    GenPacket p{};
    p.seq = seq;
    const uint8_t tLabel = static_cast<uint8_t>(seq & 0x3F);
    const int kind = static_cast<int>(rng() % 100);
    if (kind < 10) {
        p.tCode = 0x5; // read block request
    } else if (kind < 20) {
        p.tCode = 0x4; // read quadlet request
    } else if (kind < 35) {
        p.tCode = 0x1; p.dataLength = 8; // status block
    } else {
        p.tCode = 0x1;
        static constexpr size_t kSizes[] = {512, 256, 64, 32, 12, 4, 1024, 2048};
        p.dataLength = kSizes[rng() % (sizeof(kSizes) / sizeof(kSizes[0]))];
    }
    auto& b = p.bytes;
    PutLE(b, (0xFFC1u << 16) | (static_cast<uint32_t>(tLabel) << 10) | (static_cast<uint32_t>(p.tCode) << 4));
    PutLE(b, (0xFFC0u << 16) | 0xFFFFu);
    PutLE(b, seq); // destination offset low doubles as sequence tag
    if (p.tCode != 0x4) {
        PutLE(b, p.tCode == 0x5 ? (864u << 16) : (static_cast<uint32_t>(p.dataLength) << 16));
    }
    for (size_t i = 0; i < p.dataLength; i += 4) {
        PutLE(b, 0xA5000000u ^ seq ^ static_cast<uint32_t>(i)); // payload, not zero
    }
    PutLE(b, 0x84520000u | (seq & 0xFFFFu)); // trailer: xferStatus=run|active|ack_pending, timestamp
    return p;
}

// How the model publishes resCount for the two descriptors a straddling
// packet touches, as observed by the CPU:
//   Atomic        — both at packet completion, before the walker runs.
//   EarlyFull     — the filled buffer publishes resCount=0 as soon as it fills
//                   (fragment visible before its tail arrives).
//   SuccessorFirst— the controller completes the straddling packet between
//                   Dequeue's read of the head descriptor and its read of the
//                   successor: the head still shows its pre-straddle count while
//                   the successor already shows the tail. Modeled by publishing
//                   the head's final resCount from the DMA fetch of the successor
//                   descriptor (see RacingDMA).
enum class PublishMode { Atomic, EarlyFull, SuccessorFirst };

// FakeDMAMemory whose descriptor fetch can run hardware-model work: lets the
// model publish a deferred descriptor exactly when the ring reads its neighbor.
class RacingDMA final : public FakeDMAMemory {
public:
    using FakeDMAMemory::FakeDMAMemory;
    using Shared::IDMAMemory::FetchFromDevice;
    std::function<void(const std::byte*)> onFetch;
    void FetchFromDevice(const std::byte* address, size_t) const noexcept override {
        if (onFetch) onFetch(address);
    }
};

class HwModel {
public:
    HwModel(Shared::BufferRing& ring, size_t bufCount, size_t bufSize, PublishMode mode)
        : ring_(ring), bufCount_(bufCount), bufSize_(bufSize), mode_(mode) {}

    // SuccessorFirst: a fetch of descriptor `address` publishes any deferred head
    // descriptor whose successor is being read.
    void OnDescriptorFetch(const std::byte* address) {
        if (deferred_.empty()) return;
        for (size_t i : deferred_) {
            const auto* succ = ring_.GetDescriptor((i + 1) % bufCount_);
            if (reinterpret_cast<const std::byte*>(succ) == address) {
                Flush();
                return;
            }
        }
    }

    // SuccessorFirst: publish the deferred head descriptor(s) of the last straddle.
    void Flush() {
        for (size_t i : deferred_) Publish(i);
        deferred_.clear();
    }

    // Returns false (nothing written) when the next buffer is not yet recycled.
    bool Write(std::span<const uint8_t> bytes) {
        Flush();
        // Pre-check space: a packet may need the successor buffer.
        if (off_ == 0 && !Available(idx_)) return false;
        if (bytes.size() > bufSize_ - off_) {
            const size_t next = (idx_ + 1) % bufCount_;
            if (!Available(next)) return false;
        }
        size_t written = 0;
        while (written < bytes.size()) {
            const size_t space = bufSize_ - off_;
            const size_t n = std::min(space, bytes.size() - written);
            std::memcpy(ring_.GetBufferAddress(idx_) + off_, bytes.data() + written, n);
            written += n;
            off_ += n;
            if (off_ == bufSize_) {
                if (mode_ == PublishMode::EarlyFull) Publish(idx_);
                else touched_.push_back(idx_);
                idx_ = (idx_ + 1) % bufCount_;
                off_ = 0;
            }
        }
        if (mode_ == PublishMode::SuccessorFirst) {
            deferred_ = touched_;
        } else {
            for (size_t i : touched_) Publish(i);
        }
        touched_.clear();
        if (off_ > 0) Publish(idx_);
        return true;
    }

private:
    bool Available(size_t i) const {
        const auto* d = ring_.GetDescriptor(i);
        // Recycled descriptors carry resCount == reqCount (nothing filled).
        return Async::HW::AR_resCount(*d) == bufSize_;
    }
    void Publish(size_t i) {
        auto* d = ring_.GetDescriptor(i);
        const size_t filled = (i == idx_) ? off_ : bufSize_;
        Async::HW::AR_init_status(*d, static_cast<uint16_t>(bufSize_ - filled));
    }

    Shared::BufferRing& ring_;
    size_t bufCount_, bufSize_;
    PublishMode mode_;
    size_t idx_ = 0, off_ = 0;
    std::vector<size_t> touched_;
    std::vector<size_t> deferred_;
};

class ARStreamFuzz : public ::testing::TestWithParam<PublishMode> {
protected:
    static constexpr size_t kNumBuffers = 8;
    static constexpr size_t kBufferSize = 4160;

    RacingDMA dma_{1024 * 1024};
    Shared::BufferRing ring_{};
    uint64_t heartbeat_ = 0;

    void SetUp() override {
        auto descRegion = dma_.AllocateRegion(kNumBuffers * sizeof(Async::HW::OHCIDescriptor));
        ASSERT_TRUE(descRegion.has_value());
        auto bufRegion = dma_.AllocateRegion(kNumBuffers * kBufferSize);
        ASSERT_TRUE(bufRegion.has_value());
        auto* descs = reinterpret_cast<Async::HW::OHCIDescriptor*>(descRegion->virtualBase);
        ASSERT_TRUE(ring_.Initialize({descs, kNumBuffers}, {bufRegion->virtualBase, kNumBuffers * kBufferSize},
                                     kNumBuffers, kBufferSize));
        ring_.BindDma(&dma_);
        ASSERT_TRUE(ring_.Finalize(descRegion->deviceBase, bufRegion->deviceBase));
    }
};

TEST_P(ARStreamFuzz, NeverLosesFraming) {
    const PublishMode mode = GetParam();
    HwModel hw(ring_, kNumBuffers, kBufferSize, mode);
    dma_.onFetch = [&hw](const std::byte* a) { hw.OnDescriptorFetch(a); };
    std::mt19937 rng(0xC0FFEEu + static_cast<uint32_t>(mode));

    std::vector<GenPacket> expected;
    size_t verified = 0;
    uint32_t seq = 1;
    RingContext ctx{ring_};
    bool diverged = false;

    auto dispatch = [&](const ARPacketParser::PacketInfo& info) {
        if (diverged) return;
        ASSERT_LT(verified, expected.size()) << "walker dispatched more packets than generated";
        const auto& e = expected[verified];
        // Extract seq tag from header quadlet 2 (destination offset low).
        uint32_t q2 = 0;
        std::memcpy(&q2, info.packetStart + 8, 4);
        if (info.tCode != e.tCode || info.dataLength != e.dataLength || q2 != e.seq ||
            info.totalLength != e.bytes.size()) {
            diverged = true;
            ADD_FAILURE() << "framing lost at packet #" << verified << " (seq " << e.seq << ")"
                          << ": got tCode=0x" << std::hex << int(info.tCode) << " dataLen=" << std::dec
                          << info.dataLength << " total=" << info.totalLength << " q2=0x" << std::hex << q2
                          << " expected tCode=0x" << int(e.tCode) << " dataLen=" << std::dec << e.dataLength
                          << " total=" << e.bytes.size();
            return;
        }
        ++verified;
    };

    constexpr size_t kPackets = 400000;
    size_t generated = 0;
    while (generated < kPackets && !diverged) {
        const size_t batch = 1 + rng() % 24;
        for (size_t i = 0; i < batch; ++i) {
            GenPacket p = MakePacket(rng, seq);
            if (!hw.Write(p.bytes)) break; // backpressure: software must drain first
            expected.push_back(std::move(p));
            ++seq;
            ++generated;
        }
        ProcessARStream(ctx, "Fuzz", &heartbeat_, [](uint32_t, const Shared::FilledBufferInfo&) {}, dispatch);
        if (verified > 4096) { // keep memory bounded
            expected.erase(expected.begin(), expected.begin() + static_cast<long>(verified));
            verified = 0;
        }
    }
    // Drain
    hw.Flush();
    for (int i = 0; i < 4 && !diverged; ++i) {
        ProcessARStream(ctx, "Fuzz", &heartbeat_, [](uint32_t, const Shared::FilledBufferInfo&) {}, dispatch);
    }
    EXPECT_FALSE(diverged);
    EXPECT_EQ(verified, expected.size()) << "packets left undispatched";
    EXPECT_GE(generated, kPackets) << "hardware model stalled (ring never drained)";
}

INSTANTIATE_TEST_SUITE_P(PublishModes, ARStreamFuzz,
                         ::testing::Values(PublishMode::Atomic, PublishMode::EarlyFull, PublishMode::SuccessorFirst));

} // namespace
} // namespace ASFW::Testing
