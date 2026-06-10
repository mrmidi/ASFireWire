#pragma once

#include "../Protocols/Audio/AMDTP/AmdtpPacketTimeline.hpp"
#include "../Protocols/Audio/AMDTP/AmdtpPayloadWriter.hpp"
#include "FakeIsochTxSlotProvider.hpp"

#include <cstddef>
#include <cstdint>

namespace ASFW::Lab {

// On-demand packet dump for the lab inspector (host app "Dump" feature).
//
// The builder runs on the dext work queue, so it is serialized with the
// packet pump by construction — slot metadata is consistent without locks.
// The only concurrent mutator is the RT payload writer, which fills PCM into
// ExposedForAudio slots; such records are flagged kDumpFlagLiveBytes instead
// of pretending the bytes are frozen.
//
// Wire format (native layout, little-endian arm64, same-host consumption
// only — the Swift inspector reads it back with fixed offsets):
//   PacketDumpHeader (128 B) followed by recordCount × PacketDumpRecord
//   (576 B each: 64 B metadata + 512 B raw packet image). Records are
//   ordered oldest → newest and end at the anchor packet. Keep the layout
//   constants in sync with Host/PacketDumpClient.swift.

constexpr uint32_t kPacketDumpMagic = 0x4C444D50;   // 'LDMP'
constexpr uint32_t kPacketDumpVersion = 1;
constexpr uint32_t kPacketDumpMaxRecords = 6;
constexpr uint32_t kPacketDumpDefaultRecords = 4;
constexpr uint64_t kPacketDumpAnchorLatest = ~0ull;
constexpr uint64_t kPacketDumpAnchorPayload = ~1ull;

// User-client plumbing shared with the host app.
constexpr uint32_t kLabDiagUserClientType = 0x4C444247; // 'LDBG'
constexpr uint64_t kLabDiagSelectorDumpPackets = 0;

constexpr uint32_t kDumpFlagIsData = 1u << 0;        // published as a data packet
constexpr uint32_t kDumpFlagPublished = 1u << 1;     // provider still holds this index
constexpr uint32_t kDumpFlagTimelineLive = 1u << 2;  // timeline slot still maps this index
constexpr uint32_t kDumpFlagLiveBytes = 1u << 3;     // ExposedForAudio: RT PCM may still land

struct PacketDumpHeader final {
    uint32_t magic{kPacketDumpMagic};
    uint32_t version{kPacketDumpVersion};
    uint32_t recordCount{0};
    uint32_t recordStride{0};
    uint64_t hostTimeTicks{0};
    uint64_t periodIndex{0};
    uint32_t ztsPeriodFrames{0};
    uint32_t ioRunning{0};
    uint64_t exposedFrames{0};
    uint64_t nextPacketIndex{0};
    uint64_t prepareFailures{0};
    uint64_t writeEndCount{0};
    uint64_t framesVisited{0};
    uint64_t framesWritten{0};
    uint64_t framesWithoutPacket{0};
    uint64_t framesOutsidePacket{0};
    uint64_t framesRacedReuse{0};
    uint64_t expectedNextSampleTime{0};
    uint32_t expectedSampleTimeValid{0};
    uint8_t reserved[4]{};
};
static_assert(sizeof(PacketDumpHeader) == 128, "header layout is the wire contract");

struct PacketDumpRecord final {
    uint64_t packetIndex{0};
    uint64_t firstAudioFrame{0};
    uint32_t flags{0};
    uint32_t byteCount{0};
    uint32_t framesInPacket{0};
    uint32_t dbs{0};
    uint32_t slotState{0xFFFFFFFFu}; // PacketSlotState, 0xFFFFFFFF = evicted
    uint32_t generation{0};
    uint8_t reserved[24]{};
    uint8_t bytes[FakeIsochTxSlotProvider::kSlotCapacityBytes]{};
};
static_assert(sizeof(PacketDumpRecord) == 64 + 512,
              "record layout is the wire contract");

// Snapshot of the device-side context taken on the work queue.
struct PacketDumpContext final {
    uint64_t hostTimeTicks{0};
    uint64_t periodIndex{0};
    uint32_t ztsPeriodFrames{0};
    bool ioRunning{false};
    uint64_t exposedFrames{0};
    uint64_t nextPacketIndex{0};
    uint64_t prepareFailures{0};
    uint64_t writeEndCount{0};
    uint64_t expectedNextSampleTime{0};
    bool expectedSampleTimeValid{false};
};

[[nodiscard]] constexpr size_t PacketDumpBlobSize(uint32_t recordCount) noexcept {
    return sizeof(PacketDumpHeader) +
           static_cast<size_t>(recordCount) * sizeof(PacketDumpRecord);
}

// Fills `out` with header + up to `requestedCount` records ending at
// `anchorPacketIndex` (kPacketDumpAnchorLatest = newest published packet).
// Returns the number of bytes written, 0 if the buffer is too small.
// Must be called on the queue that owns the provider/timeline (work queue).
[[nodiscard]] size_t BuildPacketDumpBlob(
    const FakeIsochTxSlotProvider& provider,
    const Protocols::Audio::AMDTP::AmdtpPacketTimeline& timeline,
    const Protocols::Audio::AMDTP::AmdtpPayloadWriterCounters& payloadCounters,
    const PacketDumpContext& context,
    uint32_t requestedCount,
    uint64_t anchorPacketIndex,
    uint8_t* out,
    size_t outCapacity) noexcept;

} // namespace ASFW::Lab
