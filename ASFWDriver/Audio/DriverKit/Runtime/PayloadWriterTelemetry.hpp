#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

struct PayloadWriterTelemetryRecord final {
    uint64_t sampleTime{0};
    uint64_t writeEndFrame{0};
    uint64_t completionCursor{0};
    uint64_t exposedFrameEnd{0};
    uint64_t exposureDeficitFrames{0};
    uint32_t frameCount{0};
    uint32_t frameCapacity{0};
    uint64_t visited{0};
    uint64_t written{0};
    uint64_t withoutPacket{0};
    uint64_t outsidePacket{0};
    uint64_t racedReuse{0};
    uint64_t wroteIntoTransmitted{0};
    uint64_t nonZeroFrames{0};
    uint64_t underExposureCalls{0};
    uint64_t underExposureFrames{0};
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

// Drain-side state only. The audio callback only copies records into
// PayloadWriterTelemetryRing; it must never inspect, aggregate, or log them.
struct PayloadWriterTelemetryAnomalySummary final {
    uint64_t maxExposureDeficitFrames{0};
    uint64_t visitedDelta{0};
    uint64_t writtenDelta{0};
    uint64_t withoutPacketDelta{0};
    uint64_t outsidePacketDelta{0};
    uint64_t racedReuseDelta{0};
    uint64_t wroteIntoTransmittedDelta{0};
    uint64_t underExposureCallsDelta{0};
    uint64_t underExposureFramesDelta{0};

    [[nodiscard]] bool HasAnomaly() const noexcept {
        return maxExposureDeficitFrames != 0 || withoutPacketDelta != 0 ||
               outsidePacketDelta != 0 || racedReuseDelta != 0 ||
               wroteIntoTransmittedDelta != 0 || underExposureCallsDelta != 0 ||
               underExposureFramesDelta != 0 || visitedDelta != writtenDelta;
    }
};

// Collapses monotonic callback counters into one watchdog-drain summary. This
// deliberately compares deltas, not absolute counters: a historical startup
// miss must not keep the steady state noisy forever.
class PayloadWriterTelemetryAnomalyAggregator final {
public:
    void BeginDrain() noexcept { summary_ = {}; }

    void Reset() noexcept {
        hasPrevious_ = false;
        summary_ = {};
    }

    void Observe(const PayloadWriterTelemetryRecord& current) noexcept {
        if (!hasPrevious_ || CountersMovedBackward(current)) {
            previous_ = current;
            hasPrevious_ = true;
            if (current.exposureDeficitFrames > summary_.maxExposureDeficitFrames) {
                summary_.maxExposureDeficitFrames = current.exposureDeficitFrames;
            }
            return;
        }

        summary_.maxExposureDeficitFrames =
            current.exposureDeficitFrames > summary_.maxExposureDeficitFrames
                ? current.exposureDeficitFrames
                : summary_.maxExposureDeficitFrames;
        summary_.visitedDelta += current.visited - previous_.visited;
        summary_.writtenDelta += current.written - previous_.written;
        summary_.withoutPacketDelta += current.withoutPacket - previous_.withoutPacket;
        summary_.outsidePacketDelta += current.outsidePacket - previous_.outsidePacket;
        summary_.racedReuseDelta += current.racedReuse - previous_.racedReuse;
        summary_.wroteIntoTransmittedDelta +=
            current.wroteIntoTransmitted - previous_.wroteIntoTransmitted;
        summary_.underExposureCallsDelta +=
            current.underExposureCalls - previous_.underExposureCalls;
        summary_.underExposureFramesDelta +=
            current.underExposureFrames - previous_.underExposureFrames;
        previous_ = current;
    }

    [[nodiscard]] const PayloadWriterTelemetryAnomalySummary& Summary() const noexcept {
        return summary_;
    }

private:
    [[nodiscard]] bool CountersMovedBackward(
        const PayloadWriterTelemetryRecord& current) const noexcept {
        return current.visited < previous_.visited || current.written < previous_.written ||
               current.withoutPacket < previous_.withoutPacket ||
               current.outsidePacket < previous_.outsidePacket ||
               current.racedReuse < previous_.racedReuse ||
               current.wroteIntoTransmitted < previous_.wroteIntoTransmitted ||
               current.underExposureCalls < previous_.underExposureCalls ||
               current.underExposureFrames < previous_.underExposureFrames;
    }

    PayloadWriterTelemetryRecord previous_{};
    PayloadWriterTelemetryAnomalySummary summary_{};
    bool hasPrevious_{false};
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

    template <typename VisitFn>
    uint64_t Drain(VisitFn&& visit) noexcept {
        const uint64_t writeSeq = writeSeq_.load(std::memory_order_acquire);
        uint64_t dropped = 0;
        if (writeSeq - readSeq_ > kCapacity) {
            dropped = (writeSeq - readSeq_) - kCapacity;
            readSeq_ = writeSeq - kCapacity;
        }

        for (uint64_t seq = readSeq_; seq < writeSeq; ++seq) {
            visit(entries_[seq & (kCapacity - 1)]);
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
