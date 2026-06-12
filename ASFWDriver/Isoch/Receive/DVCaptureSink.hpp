// DVCaptureSink.hpp
// ASFW - Minimal DV (IEC 61883-2) capture tap for the IR context.
//
// Filters isoch packets for CIP FMT=0x00 (DVCR), strips the 8-byte driver
// prefix, 8-byte CIP header and 4-byte source packet header, and writes raw
// 480-byte DIF chunks into a shared-memory SPSC ring consumed by the ASFW app
// (which concatenates them into a .dv file).
//
// Producer: IsochReceiveContext::Poll() packet callback (driver process)
// Consumer: ASFW app via CopyClientMemoryForType(type=1) + IOConnectMapMemory64
//
// NOTE: This is a pragmatic capture path, not a full DV pipeline. There is no
// frame reassembly here; the app gates output on the first DIF frame-header
// chunk (0x1F 0x07 0x00) and appends chunks in arrival order. Packet loss
// shows up as a visual glitch in the captured frame, which DV players tolerate.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../Audio/Wire/CIP/CIPHeader.hpp"

namespace ASFW::Isoch::Rx {

// Shared-memory layout. Offsets are fixed and mirrored in the Swift app
// (DriverConnector+DVCapture.swift) - do not reorder fields.
struct DVRingHeader {
    uint32_t magic;            // +0   'ASDV'
    uint16_t version;          // +4
    uint16_t reserved0;        // +6
    uint32_t numRecords;       // +8   capacity in 480-byte records
    uint32_t recordBytes;      // +12  always 480
    uint32_t dataOffsetBytes;  // +16  offset of record 0 from base
    uint32_t reserved1;        // +20

    // Diagnostics (single writer: driver Poll thread; app reads racily, fine)
    uint32_t packetsSeen;      // +24  all isoch packets observed
    uint32_t dvSourcePackets;  // +28  DV source packets extracted
    uint32_t nonDvPackets;     // +32  packets rejected (bad CIP / wrong FMT)
    uint32_t overruns;         // +36  records dropped because ring was full

    // Last rejected packet snapshot (raw bytes as read from the buffer, so the
    // app can show what the wire actually looks like when the filter misses).
    uint32_t lastRejectLen;    // +40
    uint32_t lastRejectQ0;     // +44  raw quadlet at payload+8 (no byteswap)
    uint32_t lastRejectQ1;     // +48  raw quadlet at payload+12 (no byteswap)
    uint32_t lastXferStatus;   // +52  descriptor xferStatus (event code in bits 4:0)

    uint8_t pad0[8];           // +56..63

    // Free-running SPSC indices in records (not bytes).
    std::atomic<uint32_t> writeIndex;  // +64  driver-owned
    uint8_t pad1[60];
    std::atomic<uint32_t> readIndex;   // +128 app-owned
    uint8_t pad2[60];                  // ..191
};

static_assert(sizeof(DVRingHeader) == 192, "DVRingHeader layout is ABI with the app");
static_assert(offsetof(DVRingHeader, writeIndex) == 64, "writeIndex offset is ABI");
static_assert(offsetof(DVRingHeader, readIndex) == 128, "readIndex offset is ABI");

class DVCaptureSink {
public:
    static constexpr uint32_t kMagic = 0x41534456;  // 'ASDV'
    static constexpr uint16_t kVersion = 1;
    static constexpr uint32_t kRecordBytes = 480;   // one DV source packet (6 DIF blocks)
    static constexpr size_t kDriverPrefixBytes = 8; // timestamp + isoch header quadlets
    static constexpr size_t kCipHeaderBytes = 8;
    static constexpr size_t kSphBytes = 4;
    static constexpr size_t kWireBlockBytes = kSphBytes + kRecordBytes; // 484

    [[nodiscard]] static uint64_t RequiredBytes(uint32_t numRecords) noexcept {
        return sizeof(DVRingHeader) + uint64_t(numRecords) * kRecordBytes;
    }

    // Creator-side: initialize the ring header in shared memory and attach.
    [[nodiscard]] bool InitializeAndAttach(void* base, uint64_t bytes, uint32_t numRecords) noexcept {
        Detach();
        if (!base || numRecords == 0 || bytes < RequiredBytes(numRecords)) {
            return false;
        }

        std::memset(base, 0, size_t(RequiredBytes(numRecords)));

        auto* hdr = reinterpret_cast<DVRingHeader*>(base);
        hdr->magic = kMagic;
        hdr->version = kVersion;
        hdr->numRecords = numRecords;
        hdr->recordBytes = kRecordBytes;
        hdr->dataOffsetBytes = sizeof(DVRingHeader);
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
    }

    [[nodiscard]] bool IsAttached() const noexcept { return hdr_ != nullptr; }

    // Called per completed isoch packet. payload includes the 8-byte driver
    // prefix ([0-3] timestamp quadlet, [4-7] isoch header) ahead of the CIP.
    void OnPacket(const uint8_t* payload, size_t length, uint32_t xferStatus) noexcept {
        if (!hdr_ || !payload) {
            return;
        }
        hdr_->packetsSeen++;
        hdr_->lastXferStatus = xferStatus;

        if (length < kDriverPrefixBytes + kCipHeaderBytes) {
            hdr_->nonDvPackets++;
            hdr_->lastRejectLen = static_cast<uint32_t>(length);
            return;  // runt / no CIP - not even an empty packet
        }

        uint32_t q0 = 0;
        uint32_t q1 = 0;
        std::memcpy(&q0, payload + kDriverPrefixBytes, sizeof(q0));
        std::memcpy(&q1, payload + kDriverPrefixBytes + 4, sizeof(q1));

        const auto cip = CIPHeader::Decode(q0, q1);
        if (!cip || cip->format != 0x00) {
            hdr_->nonDvPackets++;
            hdr_->lastRejectLen = static_cast<uint32_t>(length);
            hdr_->lastRejectQ0 = q0;
            hdr_->lastRejectQ1 = q1;
            return;
        }

        // Consumer DV camcorders (Linux dv1394-compatible) send SPH=0: the
        // payload after the CIP is plain 480-byte DIF blocks, timestamp lives
        // in CIP.SYT. Devices that set SPH=1 prefix each block with a 4-byte
        // source packet header instead.
        const size_t skip = cip->sourcePacketHeader ? kSphBytes : 0;
        const size_t blockBytes = kRecordBytes + skip;

        const size_t dataBytes = length - kDriverPrefixBytes - kCipHeaderBytes;
        const size_t blocks = dataBytes / blockBytes;  // 0 for empty (CIP-only) packets

        for (size_t i = 0; i < blocks; ++i) {
            const uint8_t* dif = payload + kDriverPrefixBytes + kCipHeaderBytes
                                 + (i * blockBytes) + skip;
            WriteRecord(dif);
        }
        hdr_->dvSourcePackets += static_cast<uint32_t>(blocks);
    }

private:
    void WriteRecord(const uint8_t* src) noexcept {
        const uint32_t w = hdr_->writeIndex.load(std::memory_order_relaxed);
        const uint32_t r = hdr_->readIndex.load(std::memory_order_acquire);
        if (uint32_t(w - r) >= numRecords_) {
            hdr_->overruns++;
            return;
        }
        std::memcpy(data_ + size_t(w % numRecords_) * kRecordBytes, src, kRecordBytes);
        hdr_->writeIndex.store(w + 1, std::memory_order_release);
    }

    DVRingHeader* hdr_{nullptr};
    uint8_t* data_{nullptr};
    uint32_t numRecords_{0};
};

} // namespace ASFW::Isoch::Rx
