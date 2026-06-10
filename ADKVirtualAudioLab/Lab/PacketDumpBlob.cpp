#include "PacketDumpBlob.hpp"

#include <cstring>

namespace ASFW::Lab {

using Protocols::Audio::AMDTP::AmdtpPacketTimeline;
using Protocols::Audio::AMDTP::AmdtpPayloadWriterCounters;
using Protocols::Audio::AMDTP::PacketSlotState;
using Protocols::Audio::AMDTP::PacketTimelineSlot;
using Protocols::Audio::AMDTP::PreparedTxPacket;

size_t BuildPacketDumpBlob(const FakeIsochTxSlotProvider& provider,
                           const AmdtpPacketTimeline& timeline,
                           const AmdtpPayloadWriterCounters& payloadCounters,
                           const PacketDumpContext& context,
                           uint32_t requestedCount,
                           uint64_t anchorPacketIndex,
                           uint8_t* out,
                           size_t outCapacity) noexcept {
    if (out == nullptr) {
        return 0;
    }
    if (requestedCount == 0) {
        requestedCount = kPacketDumpDefaultRecords;
    }
    if (requestedCount > kPacketDumpMaxRecords) {
        requestedCount = kPacketDumpMaxRecords;
    }

    // Resolve the window [first, last] of absolute packet indices. The fake
    // ring holds the last kSlotCount publications; older indices have been
    // evicted and are reported as such rather than silently skipped.
    uint64_t windowEnd = anchorPacketIndex;
    if (windowEnd == kPacketDumpAnchorLatest) {
        if (context.nextPacketIndex == 0) {
            windowEnd = 0; // nothing published yet → empty dump below
        } else {
            windowEnd = context.nextPacketIndex - 1;
        }
    }
    if (context.nextPacketIndex != 0 && windowEnd >= context.nextPacketIndex) {
        windowEnd = context.nextPacketIndex - 1;
    }

    uint32_t recordCount = requestedCount;
    if (context.nextPacketIndex == 0) {
        recordCount = 0;
    } else if (windowEnd + 1 < recordCount) {
        recordCount = static_cast<uint32_t>(windowEnd + 1);
    }

    const size_t needed = PacketDumpBlobSize(recordCount);
    if (outCapacity < needed) {
        return 0;
    }

    PacketDumpHeader header{};
    header.recordCount = recordCount;
    header.recordStride = sizeof(PacketDumpRecord);
    header.hostTimeTicks = context.hostTimeTicks;
    header.periodIndex = context.periodIndex;
    header.ztsPeriodFrames = context.ztsPeriodFrames;
    header.ioRunning = context.ioRunning ? 1u : 0u;
    header.exposedFrames = context.exposedFrames;
    header.nextPacketIndex = context.nextPacketIndex;
    header.prepareFailures = context.prepareFailures;
    header.writeEndCount = context.writeEndCount;
    header.framesVisited =
        payloadCounters.framesVisited.load(std::memory_order_relaxed);
    header.framesWritten =
        payloadCounters.framesWritten.load(std::memory_order_relaxed);
    header.framesWithoutPacket =
        payloadCounters.framesWithoutPacket.load(std::memory_order_relaxed);
    header.framesOutsidePacket =
        payloadCounters.framesOutsidePacket.load(std::memory_order_relaxed);
    header.framesRacedReuse =
        payloadCounters.framesRacedReuse.load(std::memory_order_relaxed);
    std::memcpy(out, &header, sizeof(header));

    uint8_t* cursor = out + sizeof(header);
    const uint64_t windowStart = windowEnd + 1 - recordCount;

    for (uint32_t i = 0; i < recordCount; ++i) {
        const uint64_t absoluteIndex = windowStart + i;
        const auto packetIndex = static_cast<uint32_t>(absoluteIndex);

        PacketDumpRecord record{};
        record.packetIndex = absoluteIndex;

        const PreparedTxPacket* published = provider.PublishedPacket(packetIndex);
        if (published != nullptr) {
            record.flags |= kDumpFlagPublished;
            if (published->isData) {
                record.flags |= kDumpFlagIsData;
            }
            record.byteCount = published->byteCount;
            record.firstAudioFrame = published->firstAudioFrame;
            record.framesInPacket = published->framesInPacket;
            record.dbs = published->dbs;
        }

        const PacketTimelineSlot* slot = timeline.SlotByIndex(packetIndex);
        if (slot != nullptr) {
            record.flags |= kDumpFlagTimelineLive;
            const PacketSlotState state =
                slot->state.load(std::memory_order_relaxed);
            record.slotState = static_cast<uint32_t>(state);
            record.generation = slot->generation.load(std::memory_order_relaxed);
            if (state == PacketSlotState::ExposedForAudio) {
                record.flags |= kDumpFlagLiveBytes;
            }
        }

        // The ring position's bytes are copied even when the publication has
        // been evicted — byteCount/flags tell the inspector how much (if
        // anything) is wire-valid for this index.
        if (published != nullptr) {
            std::memcpy(record.bytes, provider.SlotBytes(packetIndex),
                        sizeof(record.bytes));
        }

        std::memcpy(cursor, &record, sizeof(record));
        cursor += sizeof(record);
    }

    return needed;
}

} // namespace ASFW::Lab
