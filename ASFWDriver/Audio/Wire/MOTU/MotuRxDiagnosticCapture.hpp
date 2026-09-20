// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "MotuTxTiming.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#ifdef ASFW_HOST_TEST
#include <functional>
#endif
#include <span>
#include <string>

namespace ASFW::Audio::Wire {

/// Snapshot of a single received packet at RX before decoding, normalization,
/// or replay caching.
struct MotuRxPacketSnapshot final {
    uint64_t epoch{0};
    uint16_t rxTimestamp{0};      // Full [sec:3][cycle:13] per-packet receive timestamp
    uint32_t cip0{0};
    uint32_t cip1{0};
    uint32_t payloadBytes{0};
    uint16_t sphCount{0};
    uint32_t strideQuadlets{0};
    uint32_t sampleRateHz{0};
    uint32_t trailingBytes{0};
    bool sphTruncated{false};
    bool timestampValid{false};

    static constexpr uint16_t kMaxSph = 32;
    uint32_t rawSph[kMaxSph]{};
    uint32_t decodedSphTick[kMaxSph]{};
};

/// Metadata for a diagnostic capture session, recorded once.
struct MotuRxStreamMetadata final {
    char model[64]{"MOTU"};
    uint32_t firmwareVersion{0};
    uint32_t sampleRateHz{0};
    char clockSource[32]{"Unknown"};
    uint64_t guid{0};
    uint32_t streamIndex{0};
};

/// Bounded diagnostic capture for raw hardware -> host SPHs at RX.
///
/// Preallocates a fixed 64-packet buffer. When armed via the control plane,
/// it captures consecutive packets (including empty or malformed packets) with
/// zero allocations and zero logging on the packet callback. Automatically disarms
/// when full. Draining and formatting happens outside the RX callback.
class MotuRxDiagnosticCapture final {
public:
    static constexpr size_t kCapacity = 64;

    MotuRxDiagnosticCapture() noexcept = default;

    // All control operations use the same nonblocking ownership gate as RX.
    // A busy result is retryable; there is no spin or wait on the packet path.
    [[nodiscard]] bool Arm(const MotuRxStreamMetadata& metadata) noexcept {
        if (busy_.test_and_set(std::memory_order_acquire)) return false;
        if (armed_.load(std::memory_order_relaxed)) {
            busy_.clear(std::memory_order_release);
            return false;
        }
        metadata_ = metadata;
        metadata_.model[sizeof(metadata_.model) - 1] = 0;
        metadata_.clockSource[sizeof(metadata_.clockSource) - 1] = 0;
        writeIndex_.store(0, std::memory_order_relaxed);
        completedIndex_.store(0, std::memory_order_relaxed);
        overflowCount_.store(0, std::memory_order_relaxed);
        full_.store(false, std::memory_order_relaxed);
        armed_.store(true, std::memory_order_release);
        busy_.clear(std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool Disarm() noexcept {
        if (busy_.test_and_set(std::memory_order_acquire)) return false;
        armed_.store(false, std::memory_order_release);
        busy_.clear(std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool IsArmed() const noexcept {
        return armed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool IsFull() const noexcept {
        return full_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t Count() const noexcept {
        const uint32_t idx = completedIndex_.load(std::memory_order_acquire);
        return (idx > kCapacity) ? static_cast<uint32_t>(kCapacity) : idx;
    }

    [[nodiscard]] uint32_t OverflowCount() const noexcept {
        return overflowCount_.load(std::memory_order_relaxed);
    }

    /// Record a packet before decoding or replay caching.
    /// Safe to call from the realtime packet callback.
    void RecordPacket(
        uint64_t epoch,
        uint16_t rxTimestamp,
        std::span<const uint8_t> payload,
        uint32_t dbs = 0,
        uint32_t cipHeaderBytes = 8U,
        uint64_t sourceGuid = 0, uint32_t sampleRateHz = 0) noexcept {
        if (busy_.test_and_set(std::memory_order_acquire)) {
            overflowCount_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        struct Release final {
            std::atomic_flag& gate;
            ~Release() { gate.clear(std::memory_order_release); }
        } release{busy_};
        if (!armed_.load(std::memory_order_relaxed)) {
            if (full_.load(std::memory_order_relaxed)) {
                overflowCount_.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }

        if (sourceGuid != 0 && sourceGuid != metadata_.guid) return;

#ifdef ASFW_HOST_TEST
        if (onRecordAdmitted_) onRecordAdmitted_();
#endif
        const uint32_t slot = writeIndex_.fetch_add(1, std::memory_order_relaxed);
        if (slot >= kCapacity) {
            overflowCount_.fetch_add(1, std::memory_order_relaxed);
            if (slot == kCapacity) {
                full_.store(true, std::memory_order_release);
                armed_.store(false, std::memory_order_release);
            }
            return;
        }

        auto& snap = packets_[slot];
        snap = MotuRxPacketSnapshot{};
        snap.epoch = epoch;
        snap.sampleRateHz = sampleRateHz != 0 ? sampleRateHz : metadata_.sampleRateHz;
        if (slot == 0 && sampleRateHz != 0) metadata_.sampleRateHz = sampleRateHz;
        snap.rxTimestamp = rxTimestamp;
        snap.timestampValid = payload.size() >= 8 && (rxTimestamp & 0x1FFFu) < 8000;
        snap.payloadBytes = static_cast<uint32_t>(payload.size());

        const size_t cipOffset = (cipHeaderBytes >= 8) ? (cipHeaderBytes - 8) : 0;
        if (payload.size() >= cipOffset + 4) {
            snap.cip0 = (static_cast<uint32_t>(payload[cipOffset]) << 24) |
                        (static_cast<uint32_t>(payload[cipOffset + 1]) << 16) |
                        (static_cast<uint32_t>(payload[cipOffset + 2]) << 8) |
                        static_cast<uint32_t>(payload[cipOffset + 3]);
        }
        if (payload.size() >= cipOffset + 8) {
            snap.cip1 = (static_cast<uint32_t>(payload[cipOffset + 4]) << 24) |
                        (static_cast<uint32_t>(payload[cipOffset + 5]) << 16) |
                        (static_cast<uint32_t>(payload[cipOffset + 6]) << 8) |
                        static_cast<uint32_t>(payload[cipOffset + 7]);
        }

        uint32_t effectiveDbs = dbs;
        if (effectiveDbs == 0 && payload.size() >= cipOffset + 4) {
            effectiveDbs = (snap.cip0 >> 16) & 0xFFU;
        }
        snap.strideQuadlets = effectiveDbs;
        const uint16_t rxCycle = rxTimestamp & 0x1FFFu;
        if (effectiveDbs > 0 && payload.size() >= cipHeaderBytes) {
            const size_t availableBytes = payload.size() - cipHeaderBytes;
            const size_t blockBytes = static_cast<size_t>(effectiveDbs) * 4U;
            if (blockBytes > 0) {
                const size_t blocks = availableBytes / blockBytes;
                snap.trailingBytes = static_cast<uint32_t>(availableBytes % blockBytes);
                snap.sphTruncated = blocks > MotuRxPacketSnapshot::kMaxSph;
                const uint16_t count = static_cast<uint16_t>(
                    blocks < MotuRxPacketSnapshot::kMaxSph ? blocks : MotuRxPacketSnapshot::kMaxSph);
                snap.sphCount = count;
                const uint32_t baseTick = ::ASFW::Encoding::Motu::BaseTickForCycle(rxCycle);
                for (uint16_t i = 0; i < count; ++i) {
                    const size_t offset = cipHeaderBytes + static_cast<size_t>(i) * blockBytes;
                    if (offset + 4 <= payload.size()) {
                        const uint32_t sph = (static_cast<uint32_t>(payload[offset]) << 24) |
                                             (static_cast<uint32_t>(payload[offset + 1]) << 16) |
                                             (static_cast<uint32_t>(payload[offset + 2]) << 8) |
                                             static_cast<uint32_t>(payload[offset + 3]);
                        snap.rawSph[i] = sph;
                        snap.decodedSphTick[i] = ::ASFW::Encoding::Motu::TickOffsetFromBase(sph, baseTick);
                    }
                }
            }
        }

        // Mark this slot as fully written. completedIndex_ is bumped only after
        // the snapshot is complete, so Packets() never exposes a partial entry.
        completedIndex_.fetch_add(1, std::memory_order_release);

        if (slot + 1 == kCapacity) {
            full_.store(true, std::memory_order_release);
            armed_.store(false, std::memory_order_release);
        }
    }

    struct Snapshot final {
        MotuRxStreamMetadata metadata{};
        std::array<MotuRxPacketSnapshot, kCapacity> packets{};
        uint32_t count{0};
        uint32_t dropped{0};
    };

    [[nodiscard]] bool CopySnapshot(Snapshot& out) const noexcept {
        if (busy_.test_and_set(std::memory_order_acquire)) return false;
#ifdef ASFW_HOST_TEST
        if (onSnapshotLocked_) onSnapshotLocked_();
#endif
        out.metadata = metadata_;
        out.count = Count();
        out.dropped = OverflowCount();
        for (uint32_t i = 0; i < out.count; ++i) out.packets[i] = packets_[i];
        busy_.clear(std::memory_order_release);
        return true;
    }

    // Owning copy: rearming cannot invalidate a diagnostic reader's data.
    [[nodiscard]] std::vector<MotuRxPacketSnapshot> Packets() const {
        Snapshot snapshot{};
        if (!CopySnapshot(snapshot)) return {};
        return {snapshot.packets.begin(), snapshot.packets.begin() + snapshot.count};
    }

    /// Format captured packets into representative text lines.
    /// Safe for non-realtime diagnostic consumption.
    [[nodiscard]] std::string FormatCaptureSummary() const {
        Snapshot snapshot{};
        if (!CopySnapshot(snapshot)) return {};
        std::string out;
        out.reserve(4096);
        out += "MOTU RX Diagnostic Capture (Model: ";
        out += snapshot.metadata.model;
        out += ", Rate: " + std::to_string(snapshot.metadata.sampleRateHz) + " Hz)\n";
        char metadataLine[256];
        snprintf(metadataLine, sizeof(metadataLine),
                 "guid=0x%016llx stream=%u firmware=0x%08x (0=unknown) clock=%s dropped=%u\n",
                 static_cast<unsigned long long>(snapshot.metadata.guid), snapshot.metadata.streamIndex,
                 snapshot.metadata.firmwareVersion, snapshot.metadata.clockSource, snapshot.dropped);
        out += metadataLine;
        out += "epoch       rx_ts    CIP0       CIP1       payload_bytes SPH_count SPH[0..N]\n";

        const auto captured = std::span(snapshot.packets.data(), snapshot.count);
        for (size_t i = 0; i < captured.size(); ++i) {
            const auto& p = captured[i];
            char buf[256];
            snprintf(buf, sizeof(buf), "%-11llu 0x%04x   0x%08x 0x%08x %-13u %-9u",
                     static_cast<unsigned long long>(p.epoch),
                     static_cast<unsigned>(p.rxTimestamp),
                     p.cip0, p.cip1, p.payloadBytes, p.sphCount);
            out += buf;
            snprintf(buf, sizeof(buf), " stride=%u rate=%u trailing=%u truncated=%u tsValid=%u",
                     p.strideQuadlets, p.sampleRateHz, p.trailingBytes, p.sphTruncated, p.timestampValid);
            out += buf;
            for (uint16_t s = 0; s < p.sphCount; ++s) {
                snprintf(buf, sizeof(buf), " [0x%08x:off=%u]", p.rawSph[s], p.decodedSphTick[s]);
                out += buf;
            }
            out += "\n";
        }
        return out;
    }

#ifdef ASFW_HOST_TEST
    void SetOnRecordAdmittedForTesting(std::function<void()> hook) { onRecordAdmitted_ = std::move(hook); }
    void SetOnSnapshotLockedForTesting(std::function<void()> hook) { onSnapshotLocked_ = std::move(hook); }
#endif
private:
#ifdef ASFW_HOST_TEST
    std::function<void()> onRecordAdmitted_{};
    std::function<void()> onSnapshotLocked_{};
#endif
    mutable std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    std::array<MotuRxPacketSnapshot, kCapacity> packets_{};
    MotuRxStreamMetadata metadata_{};
    std::atomic<uint32_t> writeIndex_{0};
    std::atomic<uint32_t> completedIndex_{0};   // Bumped after snapshot is fully written
    std::atomic<uint32_t> overflowCount_{0};
    std::atomic<bool> armed_{false};
    std::atomic<bool> full_{false};
};

} // namespace ASFW::Audio::Wire
