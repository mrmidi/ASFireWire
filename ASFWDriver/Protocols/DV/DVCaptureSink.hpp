// DVCaptureSink.hpp
// ASFW - IEC 61883-2 content sink for payload-normalized isoch packets.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "../../Audio/Wire/CIP/CIPHeader.hpp"
#include "../../Hardware/OHCIEventCodes.hpp"
#include "../../Isoch/Core/IsochTypes.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Protocols::DV {

enum class CaptureState : uint32_t {
    Initializing = 0,
    Active = 1,
    Stopped = 2,
    BusReset = 3,
    Failed = 4,
};

// Shared-memory layout. Offsets are fixed and mirrored by
// ASFW/DriverConnector+DVCapture.swift.
struct DVRingHeader {
    uint32_t magic;            // +0   'ASDV'
    uint16_t version;          // +4
    uint8_t channel;           // +6   negotiated receive channel
    uint8_t connectionMode;    // +7   ConnectionMode raw value
    uint32_t numRecords;       // +8   capacity in 480-byte records
    uint32_t recordBytes;      // +12  always 480
    uint32_t dataOffsetBytes;  // +16  offset of record 0 from base
    std::atomic<uint32_t> state; // +20 CaptureState raw value

    std::atomic<uint32_t> packetsSeen;      // +24
    std::atomic<uint32_t> dvSourcePackets;  // +28
    std::atomic<uint32_t> nonDvPackets;     // +32
    std::atomic<uint32_t> overruns;          // +36
    std::atomic<uint32_t> lastRejectLen;     // +40
    std::atomic<uint32_t> lastRejectQ0;      // +44
    std::atomic<uint32_t> lastRejectQ1;      // +48
    std::atomic<uint32_t> lastXferStatus;    // +52
    std::atomic<uint32_t> dbcDiscontinuities; // +56
    std::atomic<uint32_t> invalidDifBlocks;   // +60

    std::atomic<uint32_t> writeIndex; // +64, driver-owned
    uint8_t pad1[60];
    std::atomic<uint32_t> readIndex;  // +128, app-owned
    uint8_t pad2[60];
};

static_assert(sizeof(DVRingHeader) == 192,
              "DVRingHeader layout is ABI with the app");
static_assert(std::atomic<uint32_t>::is_always_lock_free &&
                  sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
              "DV ring ABI requires lock-free 32-bit shared atomics");
static_assert(offsetof(DVRingHeader, writeIndex) == 64,
              "writeIndex offset is ABI");
static_assert(offsetof(DVRingHeader, readIndex) == 128,
              "readIndex offset is ABI");

class DVCaptureSink final : public ASFW::Isoch::IIsochReceiveConsumer {
public:
    static constexpr uint32_t kMagic = 0x41534456; // 'ASDV'
    static constexpr uint16_t kVersion = 3;
    static constexpr uint32_t kRecordBytes = 480;  // six 80-byte DIF blocks
    static constexpr size_t kTransportPrefixBytes = 8;
    static constexpr size_t kCipHeaderBytes = 8;
    static constexpr size_t kSphBytes = 4;

    [[nodiscard]] static uint64_t RequiredBytes(uint32_t numRecords) noexcept {
        return sizeof(DVRingHeader) + uint64_t(numRecords) * kRecordBytes;
    }

    [[nodiscard]] bool InitializeAndAttach(void* base,
                                           uint64_t bytes,
                                           uint32_t numRecords) noexcept {
        Detach();
        if (!base || numRecords == 0 || bytes < RequiredBytes(numRecords)) {
            return false;
        }

        std::memset(base, 0, size_t(RequiredBytes(numRecords)));
        auto* hdr = ::new (base) DVRingHeader{};
        hdr->magic = kMagic;
        hdr->version = kVersion;
        hdr->channel = 0xFF;
        hdr->numRecords = numRecords;
        hdr->recordBytes = kRecordBytes;
        hdr->dataOffsetBytes = sizeof(DVRingHeader);
        hdr->state.store(static_cast<uint32_t>(CaptureState::Initializing),
                         std::memory_order_relaxed);
        hdr->writeIndex.store(0, std::memory_order_relaxed);
        hdr->readIndex.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        hdr_ = hdr;
        data_ = reinterpret_cast<uint8_t*>(base) + hdr->dataOffsetBytes;
        numRecords_ = numRecords;
        return true;
    }

    void Detach() noexcept {
        hdr_ = nullptr;
        data_ = nullptr;
        numRecords_ = 0;
        dbcInitialized_ = false;
        expectedDbc_ = 0;
    }

    [[nodiscard]] bool IsAttached() const noexcept { return hdr_ != nullptr; }

    void MarkActive(uint8_t channel, uint8_t connectionMode) noexcept {
        if (!hdr_) return;
        hdr_->channel = channel;
        hdr_->connectionMode = connectionMode;
        hdr_->state.store(static_cast<uint32_t>(CaptureState::Active),
                          std::memory_order_release);
    }

    void MarkTerminal(CaptureState state) noexcept {
        if (!hdr_) return;
        hdr_->state.store(static_cast<uint32_t>(state),
                          std::memory_order_release);
    }

    void BeginReceiveBatch(
        const ASFW::Isoch::IsochReceiveBatch&) noexcept override {}

    void ConsumePacket(
        const ASFW::Isoch::IsochReceiveBatch&,
        const ASFW::Isoch::IsochReceivePacket& packet) noexcept override {
        if (!hdr_) {
            return;
        }

        hdr_->packetsSeen.fetch_add(1, std::memory_order_relaxed);
        hdr_->lastXferStatus.store(packet.transferStatus,
                                   std::memory_order_relaxed);

        const auto successCode = static_cast<uint32_t>(
            ASFW::Async::OHCIEventCode::kAckComplete);
        if ((packet.transferStatus & 0x1Fu) != successCode ||
            packet.payload.size() < kTransportPrefixBytes + kCipHeaderBytes) {
            Reject(packet);
            return;
        }

        // With OHCI isoch-header mode enabled, the DMA buffer starts with the
        // receive timestamp and 1394 isoch header. Content begins after those
        // two quadlets; transport deliberately leaves them opaque.
        const auto content = packet.payload.subspan(kTransportPrefixBytes);

        uint32_t q0 = 0;
        uint32_t q1 = 0;
        std::memcpy(&q0, content.data(), sizeof(q0));
        std::memcpy(&q1, content.data() + 4, sizeof(q1));

        const auto cip = ASFW::Isoch::CIPHeader::Decode(q0, q1);
        if (!cip || cip->format != 0x00) {
            Reject(packet, q0, q1);
            return;
        }

        const auto media = content.subspan(kCipHeaderBytes);
        if (!media.empty() && cip->dataBlockSize != kRecordBytes / 4) {
            Reject(packet, q0, q1);
            return;
        }

        const size_t sourceHeaderBytes =
            cip->sourcePacketHeader ? kSphBytes : 0;
        const size_t sourcePacketBytes = kRecordBytes + sourceHeaderBytes;
        if (media.size() % sourcePacketBytes != 0) {
            Reject(packet, q0, q1);
            return;
        }

        const size_t blocks = media.size() / sourcePacketBytes;
        if (blocks > 0) {
            if (dbcInitialized_ && cip->dataBlockCounter != expectedDbc_) {
                hdr_->dbcDiscontinuities.fetch_add(
                    1, std::memory_order_relaxed);
            }
            expectedDbc_ = static_cast<uint8_t>(
                cip->dataBlockCounter + static_cast<uint8_t>(blocks));
            dbcInitialized_ = true;
        }

        for (size_t i = 0; i < blocks; ++i) {
            const uint8_t* dif = media.data() + (i * sourcePacketBytes) +
                                 sourceHeaderBytes;
            if (!HasPlausibleDifIds(dif)) {
                hdr_->invalidDifBlocks.fetch_add(
                    1, std::memory_order_relaxed);
                continue;
            }
            WriteRecord(dif);
            hdr_->dvSourcePackets.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    [[nodiscard]] static bool HasPlausibleDifIds(const uint8_t* chunk) noexcept {
        for (size_t offset = 0; offset < kRecordBytes; offset += 80) {
            const uint8_t section = chunk[offset] >> 5;
            const uint8_t sequence = chunk[offset + 1] >> 4;
            if (section > 4 || sequence >= 12) {
                return false;
            }
        }
        return true;
    }

    void Reject(const ASFW::Isoch::IsochReceivePacket& packet,
                uint32_t q0 = 0,
                uint32_t q1 = 0) noexcept {
        hdr_->nonDvPackets.fetch_add(1, std::memory_order_relaxed);
        hdr_->lastRejectLen.store(
            static_cast<uint32_t>(packet.payload.size()),
            std::memory_order_relaxed);
        hdr_->lastRejectQ0.store(q0, std::memory_order_relaxed);
        hdr_->lastRejectQ1.store(q1, std::memory_order_relaxed);
    }

    void WriteRecord(const uint8_t* src) noexcept {
        const uint32_t write =
            hdr_->writeIndex.load(std::memory_order_relaxed);
        const uint32_t read =
            hdr_->readIndex.load(std::memory_order_acquire);
        if (uint32_t(write - read) >= numRecords_) {
            hdr_->overruns.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::memcpy(data_ + size_t(write % numRecords_) * kRecordBytes,
                    src,
                    kRecordBytes);
        hdr_->writeIndex.store(write + 1, std::memory_order_release);
    }

    DVRingHeader* hdr_{nullptr};
    uint8_t* data_{nullptr};
    uint32_t numRecords_{0};
    uint8_t expectedDbc_{0};
    bool dbcInitialized_{false};
};

} // namespace ASFW::Protocols::DV
