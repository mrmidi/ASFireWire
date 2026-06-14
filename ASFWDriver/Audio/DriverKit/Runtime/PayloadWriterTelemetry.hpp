#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

struct PayloadWriterTelemetryRecord final {
    uint64_t sampleTime{0};
    uint64_t completionCursor{0};
    uint64_t exposedFrameEnd{0};
    uint32_t frameCount{0};
    uint32_t frameCapacity{0};
    uint64_t visited{0};
    uint64_t written{0};
    uint64_t withoutPacket{0};
    uint64_t outsidePacket{0};
    uint64_t racedReuse{0};
    uint64_t wroteIntoTransmitted{0};
    uint64_t nonZeroFrames{0};
    float    maxAbsSample{0.0f};

    // Additional cursors and addresses for diagnosing buffer mapping / pointer alignment
    uint64_t playbackRingReadFrame{0};
    uint64_t playbackRingWriteFrame{0};
    uint64_t outputBaseAddr{0};
    uint64_t captureRingReadFrame{0};
    uint64_t captureRingWriteFrame{0};
    uint64_t inputBaseAddr{0};

    // CIP and payload bytes from the last packet retired (read) by hardware
    uint64_t lastReadPacketIndex{0};
    uint8_t  lastReadPacketBytes[16]{0};
};

class PayloadWriterTelemetryRing final {
public:
    static constexpr uint32_t kCapacity = 256;
    static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");

    void Reset() noexcept {
        writeSeq_.store(0, std::memory_order_relaxed);
        readSeq_ = 0;
    }

    void Record(const PayloadWriterTelemetryRecord& rec) noexcept {
        const uint64_t seq = writeSeq_.load(std::memory_order_relaxed);
        entries_[seq & (kCapacity - 1)] = rec;
        writeSeq_.store(seq + 1, std::memory_order_release);
    }

    [[nodiscard]] uint64_t PendingCount() const noexcept {
        const uint64_t writeSeq = writeSeq_.load(std::memory_order_acquire);
        return writeSeq >= readSeq_ ? writeSeq - readSeq_ : 0;
    }

    template <typename EmitFn>
    uint64_t Drain(uint32_t maxEmit, EmitFn&& emit) noexcept {
        const uint64_t writeSeq = writeSeq_.load(std::memory_order_acquire);
        uint64_t dropped = 0;
        if (writeSeq - readSeq_ > kCapacity) {
            dropped = (writeSeq - readSeq_) - kCapacity;
            readSeq_ = writeSeq - kCapacity;
        }

        const uint64_t available = writeSeq - readSeq_;
        if (available != 0 && maxEmit != 0) {
            const uint64_t stride = available > maxEmit ? (available / maxEmit) : 1;
            uint32_t emitted = 0;
            for (uint64_t seq = readSeq_; seq < writeSeq; ++seq) {
                const PayloadWriterTelemetryRecord& rec = entries_[seq & (kCapacity - 1)];
                const bool onStride =
                    emitted < maxEmit && ((seq - readSeq_) % stride) == 0;
                if (onStride) {
                    emit(rec);
                    ++emitted;
                }
            }
        }

        readSeq_ = writeSeq;
        return dropped;
    }

private:
    std::array<PayloadWriterTelemetryRecord, kCapacity> entries_{};
    std::atomic<uint64_t> writeSeq_{0};
    uint64_t readSeq_{0};
};

} // namespace ASFW::Audio::Runtime
