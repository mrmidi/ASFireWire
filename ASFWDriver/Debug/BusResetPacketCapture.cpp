#include "BusResetPacketCapture.hpp"

#include <DriverKit/IOLib.h>
#include <os/log.h>

#include <cstdio>
#include <cstring>

namespace ASFW::Debug {

namespace {
/// Get current timestamp in nanoseconds
uint64_t GetCurrentTimestamp() {
    static mach_timebase_info_data_t timebase{};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    const uint64_t ticks = mach_absolute_time();
    return (ticks * timebase.numer) / timebase.denom;
}

/// Byte-swap from little-endian DMA to big-endian wire format
uint32_t LEtoBE(uint32_t le) {
    return OSSwapLittleToHostInt32(le);
}
} // anonymous namespace

BusResetPacketCapture::BusResetPacketCapture() = default;
BusResetPacketCapture::~BusResetPacketCapture() = default;

void BusResetPacketCapture::CapturePacket(const uint32_t* dmaQuadlets,
                                          uint8_t generation,
                                          const char* context)
{
    if (!dmaQuadlets) {
        return;
    }

    // Get next write slot
    const uint32_t index = writeIndex_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t slot = index % kBusResetPacketHistorySize;

    // Increment count (saturate at max)
    uint32_t oldCount = count_.load(std::memory_order_acquire);
    while (oldCount < kBusResetPacketHistorySize) {
        if (count_.compare_exchange_weak(oldCount, oldCount + 1,
                                        std::memory_order_release,
                                        std::memory_order_acquire)) {
            break;
        }
    }

    // Fill snapshot
    BusResetPacketSnapshot& snapshot = ring_[slot];

    snapshot.captureTimestamp = GetCurrentTimestamp();
    snapshot.generation = generation;

    // Copy raw quadlets (little-endian from DMA)
    std::memcpy(snapshot.rawQuadlets, dmaQuadlets, sizeof(snapshot.rawQuadlets));

    // Convert to wire format (big-endian)
    for (int i = 0; i < 4; ++i) {
        snapshot.wireQuadlets[i] = LEtoBE(dmaQuadlets[i]);
    }

    // Extract tCode from wire format Q0[31:28]
    snapshot.tCode = static_cast<uint8_t>(snapshot.wireQuadlets[0] >> 28);

    // Extract trailer from Q3 (already in wire format after swap)
    // Trailer format: xferStatus[31:16] | timeStamp[15:0]
    const uint32_t trailer = snapshot.wireQuadlets[3];
    const uint16_t xferStatus = static_cast<uint16_t>(trailer >> 16);
    snapshot.eventCode = static_cast<uint8_t>(xferStatus & 0x1F);
    snapshot.cycleTime = static_cast<uint16_t>(trailer & 0xFFFF);

    // Copy context string
    if (context) {
        std::strncpy(snapshot.contextInfo, context, sizeof(snapshot.contextInfo) - 1);
        snapshot.contextInfo[sizeof(snapshot.contextInfo) - 1] = '\0';
    } else {
        std::snprintf(snapshot.contextInfo, sizeof(snapshot.contextInfo),
                     "Gen %u @ slot %u", generation, slot);
    }
}

const BusResetPacketSnapshot* BusResetPacketCapture::GetSnapshot(size_t index) const
{
    const uint32_t count = count_.load(std::memory_order_acquire);
    if (index >= count) {
        return nullptr;
    }

    // Calculate actual ring buffer index
    // If count < kBusResetPacketHistorySize, oldest is at index 0
    // If count == kBusResetPacketHistorySize, oldest is at (writeIndex_ - count)
    const uint32_t writeIdx = writeIndex_.load(std::memory_order_acquire);
    uint32_t oldestIdx;

    if (count < kBusResetPacketHistorySize) {
        oldestIdx = 0;
    } else {
        oldestIdx = writeIdx % kBusResetPacketHistorySize;
    }

    const uint32_t slot = (oldestIdx + index) % kBusResetPacketHistorySize;
    return &ring_[slot];
}

size_t BusResetPacketCapture::GetCount() const
{
    return count_.load(std::memory_order_acquire);
}

void BusResetPacketCapture::Clear()
{
    writeIndex_.store(0, std::memory_order_release);
    count_.store(0, std::memory_order_release);

    // Zero out snapshots for clean state
    // Use assignment instead of memset for non-trivially copyable types
    for (auto& snapshot : ring_) {
        snapshot = BusResetPacketSnapshot{};
    }
}

const BusResetPacketSnapshot* BusResetPacketCapture::GetLatest() const
{
    const uint32_t count = count_.load(std::memory_order_acquire);
    if (count == 0) {
        return nullptr;
    }

    return GetSnapshot(count - 1);
}

} // namespace ASFW::Debug
