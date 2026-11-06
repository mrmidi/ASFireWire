// DVCaptureSink.hpp
// ASFW - camcorder stream sink for payload-normalized isoch packets.
//
// Carries the two formats a tape camcorder sends on its output plug:
//   - DV (IEC 61883-2): 480-byte DIF chunks, one per data block.
//   - MPEG-2 TS (IEC 61883-4), as sent by HDV camcorders: 192-byte source
//     packets (4-byte source packet header + 188-byte TS packet), each split
//     into eight 24-byte data blocks (DBS=6, FN=3, SPH=1).
// The format is locked from the first packet that carries data; packets of the
// other format are rejected for the rest of the session.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
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

enum class StreamFormat : uint32_t {
    Unknown = 0,
    Dv = 1,      // IEC 61883-2, FMT 0x00
    MpegTs = 2,  // IEC 61883-4, FMT 0x20 (HDV)
};

[[nodiscard]] constexpr const char* ToString(StreamFormat format) noexcept {
    switch (format) {
    case StreamFormat::Unknown: return "unknown";
    case StreamFormat::Dv: return "DV";
    case StreamFormat::MpegTs: return "MPEG-2 TS";
    }
    return "unknown";
}

// Shared-memory layout. Offsets are fixed and mirrored by
// ASFW/DriverConnector+DVCapture.swift.
struct DVRingHeader {
    uint32_t magic;            // +0   'ASDV'
    uint16_t version;          // +4
    uint8_t channel;           // +6   negotiated receive channel
    uint8_t connectionMode;    // +7   ConnectionMode raw value
    uint32_t numRecords;       // +8   capacity in records
    uint32_t recordBytes;      // +12  record stride, always 480
    uint32_t dataOffsetBytes;  // +16  offset of record 0 from base
    std::atomic<uint32_t> state; // +20 CaptureState raw value

    std::atomic<uint32_t> packetsSeen;          // +24
    std::atomic<uint32_t> sourcePackets;        // +28 records published
    std::atomic<uint32_t> rejectedPackets;      // +32
    std::atomic<uint32_t> overruns;             // +36
    std::atomic<uint32_t> lastRejectLen;        // +40
    std::atomic<uint32_t> lastRejectQ0;         // +44 host order
    std::atomic<uint32_t> lastRejectQ1;         // +48 host order
    std::atomic<uint32_t> lastXferStatus;       // +52
    std::atomic<uint32_t> dbcDiscontinuities;   // +56
    std::atomic<uint32_t> invalidSourcePackets; // +60 bad DIF ID / TS sync byte

    std::atomic<uint32_t> writeIndex; // +64, driver-owned
    // Published once, before the first record, with release order: a reader
    // that has acquired writeIndex past a record also sees these.
    std::atomic<uint32_t> streamFormat;      // +68 StreamFormat raw value
    std::atomic<uint32_t> sourcePacketBytes; // +72 480 (DV) or 188 (TS)
    std::atomic<uint32_t> droppedFragments;  // +76 incomplete TS source packets
    uint8_t pad1[48];
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
static_assert(offsetof(DVRingHeader, streamFormat) == 68,
              "streamFormat offset is ABI");
static_assert(offsetof(DVRingHeader, droppedFragments) == 76,
              "droppedFragments offset is ABI");
static_assert(offsetof(DVRingHeader, readIndex) == 128,
              "readIndex offset is ABI");

class DVCaptureSink final : public ASFW::Isoch::IIsochReceiveConsumer {
public:
    static constexpr uint32_t kMagic = 0x41534456; // 'ASDV'
    static constexpr uint16_t kVersion = 4;
    static constexpr uint32_t kRecordBytes = 480;  // six 80-byte DIF blocks
    static constexpr size_t kTransportPrefixBytes = 8;
    static constexpr size_t kCipHeaderBytes = 8;
    static constexpr size_t kSphBytes = 4;

    static constexpr uint8_t kTsDataBlockQuadlets = 6;
    static constexpr uint8_t kTsFractionNumber = 3;
    static constexpr size_t kTsBlockBytes = kTsDataBlockQuadlets * 4u;
    static constexpr size_t kTsBlocksPerSourcePacket = 1u << kTsFractionNumber;
    static constexpr size_t kTsSourcePacketBytes =
        kTsBlockBytes * kTsBlocksPerSourcePacket;
    static constexpr uint32_t kTsPacketBytes = 188;
    static constexpr uint8_t kTsSyncByte = 0x47;

    static_assert(kTsSourcePacketBytes == kSphBytes + kTsPacketBytes);
    static_assert(kTsPacketBytes <= kRecordBytes);

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
        format_ = StreamFormat::Unknown;
        tsPartialBlocks_ = 0;
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

    // One line per session, off the receive path (call after IR is stopped).
    void LogSessionSummary() const noexcept {
        if (!hdr_) return;
        ASFW_LOG(Isoch,
                 "[DV] session summary format=%{public}s packets=%u records=%u rejected=%u "
                 "dbcGaps=%u invalid=%u droppedFragments=%u overruns=%u "
                 "lastReject len=%u q0=0x%08x q1=0x%08x",
                 ToString(format_),
                 hdr_->packetsSeen.load(std::memory_order_relaxed),
                 hdr_->sourcePackets.load(std::memory_order_relaxed),
                 hdr_->rejectedPackets.load(std::memory_order_relaxed),
                 hdr_->dbcDiscontinuities.load(std::memory_order_relaxed),
                 hdr_->invalidSourcePackets.load(std::memory_order_relaxed),
                 hdr_->droppedFragments.load(std::memory_order_relaxed),
                 hdr_->overruns.load(std::memory_order_relaxed),
                 hdr_->lastRejectLen.load(std::memory_order_relaxed),
                 hdr_->lastRejectQ0.load(std::memory_order_relaxed),
                 hdr_->lastRejectQ1.load(std::memory_order_relaxed));
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
        const auto media = content.subspan(kCipHeaderBytes);
        const StreamFormat format =
            cip ? Classify(*cip, media.empty()) : StreamFormat::Unknown;
        if (format == StreamFormat::Unknown) {
            Reject(packet, q0, q1);
            return;
        }

        if (format_ == StreamFormat::Unknown) {
            // Lock on data, not on an empty (CIP-only) packet.
            if (media.empty()) return;
            LockFormat(format);
        } else if (format != format_) {
            Reject(packet, q0, q1);
            return;
        }

        if (format_ == StreamFormat::Dv) {
            ConsumeDv(*cip, media, packet, q0, q1);
        } else {
            ConsumeMpegTs(*cip, media, packet, q0, q1);
        }
    }

private:
    [[nodiscard]] static StreamFormat Classify(const ASFW::Isoch::CIPHeader& cip,
                                               bool empty) noexcept {
        if (cip.format == 0x00) {
            return empty || cip.dataBlockSize == kRecordBytes / 4
                       ? StreamFormat::Dv
                       : StreamFormat::Unknown;
        }
        if (cip.format == 0x20) {
            // IEC 61883-4 fixes DBS=6, FN=3 and SPH=1 for MPEG-2 TS; observed
            // as q0=0x0006C4B8 q1=0xA0800000 from a Sony HVR-Z1U playing HDV.
            const bool geometry = cip.dataBlockSize == kTsDataBlockQuadlets &&
                                  cip.fractionNumber == kTsFractionNumber &&
                                  cip.sourcePacketHeader;
            return empty || geometry ? StreamFormat::MpegTs
                                     : StreamFormat::Unknown;
        }
        return StreamFormat::Unknown;
    }

    void LockFormat(StreamFormat format) noexcept {
        format_ = format;
        hdr_->sourcePacketBytes.store(
            format == StreamFormat::Dv ? kRecordBytes : kTsPacketBytes,
            std::memory_order_relaxed);
        hdr_->streamFormat.store(static_cast<uint32_t>(format),
                                 std::memory_order_release);
        ASFW_LOG(Isoch, "[DV] stream format locked: %{public}s",
                 ToString(format));
    }

    void ConsumeDv(const ASFW::Isoch::CIPHeader& cip,
                   std::span<const uint8_t> media,
                   const ASFW::Isoch::IsochReceivePacket& packet,
                   uint32_t q0,
                   uint32_t q1) noexcept {
        const size_t sourceHeaderBytes =
            cip.sourcePacketHeader ? kSphBytes : 0;
        const size_t sourcePacketBytes = kRecordBytes + sourceHeaderBytes;
        if (media.size() % sourcePacketBytes != 0) {
            Reject(packet, q0, q1);
            return;
        }

        const size_t blocks = media.size() / sourcePacketBytes;
        if (blocks > 0) {
            if (dbcInitialized_ && cip.dataBlockCounter != expectedDbc_) {
                hdr_->dbcDiscontinuities.fetch_add(
                    1, std::memory_order_relaxed);
            }
            expectedDbc_ = static_cast<uint8_t>(
                cip.dataBlockCounter + static_cast<uint8_t>(blocks));
            dbcInitialized_ = true;
        }

        for (size_t i = 0; i < blocks; ++i) {
            const uint8_t* dif = media.data() + (i * sourcePacketBytes) +
                                 sourceHeaderBytes;
            if (!HasPlausibleDifIds(dif)) {
                hdr_->invalidSourcePackets.fetch_add(
                    1, std::memory_order_relaxed);
                continue;
            }
            WriteRecord(dif, kRecordBytes);
        }
    }

    // A source packet may span bus packets, so blocks are reassembled by DBC.
    // A block whose DBC has its low FN bits clear starts a source packet
    // (IEC 61883-1 fractions; the HVR-Z1U capture above has DBC=0xB8, a
    // multiple of 8, at the start of its one source packet). A DBC gap drops
    // the partial source packet and resumes at the next source-packet start.
    void ConsumeMpegTs(const ASFW::Isoch::CIPHeader& cip,
                       std::span<const uint8_t> media,
                       const ASFW::Isoch::IsochReceivePacket& packet,
                       uint32_t q0,
                       uint32_t q1) noexcept {
        if (media.size() % kTsBlockBytes != 0) {
            Reject(packet, q0, q1);
            return;
        }

        const size_t blocks = media.size() / kTsBlockBytes;
        if (blocks == 0) {
            return;
        }

        if (dbcInitialized_ && cip.dataBlockCounter != expectedDbc_) {
            hdr_->dbcDiscontinuities.fetch_add(1, std::memory_order_relaxed);
            if (tsPartialBlocks_ != 0) {
                hdr_->droppedFragments.fetch_add(1, std::memory_order_relaxed);
                tsPartialBlocks_ = 0;
            }
        }
        expectedDbc_ = static_cast<uint8_t>(
            cip.dataBlockCounter + static_cast<uint8_t>(blocks));
        dbcInitialized_ = true;

        constexpr uint8_t kStartMask = kTsBlocksPerSourcePacket - 1;
        size_t i = 0;
        while (i < blocks) {
            const uint8_t* block = media.data() + i * kTsBlockBytes;
            if (tsPartialBlocks_ == 0) {
                const auto dbc = static_cast<uint8_t>(
                    cip.dataBlockCounter + static_cast<uint8_t>(i));
                if ((dbc & kStartMask) != 0) {
                    ++i;  // tail of a source packet whose start was not seen
                    continue;
                }
                if (blocks - i >= kTsBlocksPerSourcePacket) {
                    PublishTsSourcePacket(block);
                    i += kTsBlocksPerSourcePacket;
                    continue;
                }
            }
            std::memcpy(tsPartial_ + tsPartialBlocks_ * kTsBlockBytes, block,
                        kTsBlockBytes);
            ++tsPartialBlocks_;
            ++i;
            if (tsPartialBlocks_ == kTsBlocksPerSourcePacket) {
                PublishTsSourcePacket(tsPartial_);
                tsPartialBlocks_ = 0;
            }
        }
    }

    void PublishTsSourcePacket(const uint8_t* sourcePacket) noexcept {
        const uint8_t* ts = sourcePacket + kSphBytes;
        if (ts[0] != kTsSyncByte) {
            hdr_->invalidSourcePackets.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        WriteRecord(ts, kTsPacketBytes);
    }

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
        hdr_->rejectedPackets.fetch_add(1, std::memory_order_relaxed);
        hdr_->lastRejectLen.store(
            static_cast<uint32_t>(packet.payload.size()),
            std::memory_order_relaxed);
        hdr_->lastRejectQ0.store(OSSwapBigToHostInt32(q0),
                                 std::memory_order_relaxed);
        hdr_->lastRejectQ1.store(OSSwapBigToHostInt32(q1),
                                 std::memory_order_relaxed);
    }

    void WriteRecord(const uint8_t* src, size_t bytes) noexcept {
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
                    bytes);
        hdr_->writeIndex.store(write + 1, std::memory_order_release);
        hdr_->sourcePackets.fetch_add(1, std::memory_order_relaxed);
    }

    DVRingHeader* hdr_{nullptr};
    uint8_t* data_{nullptr};
    uint32_t numRecords_{0};
    uint8_t expectedDbc_{0};
    bool dbcInitialized_{false};
    StreamFormat format_{StreamFormat::Unknown};
    uint8_t tsPartial_[kTsSourcePacketBytes]{};
    size_t tsPartialBlocks_{0};
};

} // namespace ASFW::Protocols::DV
