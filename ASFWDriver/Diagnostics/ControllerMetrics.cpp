#include "ControllerMetrics.hpp"

#include <DriverKit/IOLib.h>
#include <cstring>

namespace ASFW::Driver {

namespace {
uint64_t GetCurrentTimeNanos() {
    static mach_timebase_info_data_t timebase{};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    const uint64_t ticks = mach_absolute_time();
    return (ticks * timebase.numer) / timebase.denom;
}
} // anonymous namespace

ControllerMetrics::ControllerMetrics()
{
    startTime_ = GetCurrentTimeNanos();
}

ControllerMetrics::~ControllerMetrics() = default;

void ControllerMetrics::RecordBusReset(uint32_t generation)
{
    busResetCount_.fetch_add(1, std::memory_order_relaxed);
    currentGeneration_.store(generation, std::memory_order_release);
    lastResetTimestamp_.store(GetCurrentTimeNanos(), std::memory_order_release);
}

uint64_t ControllerMetrics::GetBusResetCount() const
{
    return busResetCount_.load(std::memory_order_acquire);
}

uint32_t ControllerMetrics::GetCurrentGeneration() const
{
    return currentGeneration_.load(std::memory_order_acquire);
}

uint64_t ControllerMetrics::GetLastResetTimestamp() const
{
    return lastResetTimestamp_.load(std::memory_order_acquire);
}

void ControllerMetrics::RecordARRequestPacket(size_t /*bytes*/)
{
    arRequestPackets_.fetch_add(1, std::memory_order_relaxed);
}

void ControllerMetrics::RecordARResponsePacket(size_t /*bytes*/)
{
    arResponsePackets_.fetch_add(1, std::memory_order_relaxed);
}

void ControllerMetrics::RecordATRequestCompleted()
{
    atRequestsCompleted_.fetch_add(1, std::memory_order_relaxed);
}

void ControllerMetrics::RecordATResponseCompleted()
{
    atResponsesCompleted_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t ControllerMetrics::GetARRequestPacketCount() const
{
    return arRequestPackets_.load(std::memory_order_acquire);
}

uint64_t ControllerMetrics::GetARResponsePacketCount() const
{
    return arResponsePackets_.load(std::memory_order_acquire);
}

uint64_t ControllerMetrics::GetATRequestCompletedCount() const
{
    return atRequestsCompleted_.load(std::memory_order_acquire);
}

uint64_t ControllerMetrics::GetATResponseCompletedCount() const
{
    return atResponsesCompleted_.load(std::memory_order_acquire);
}

void ControllerMetrics::SetNodeCount(uint8_t count)
{
    nodeCount_.store(count, std::memory_order_release);
}

void ControllerMetrics::SetLocalNodeID(uint8_t nodeID)
{
    localNodeID_.store(nodeID, std::memory_order_release);
}

void ControllerMetrics::SetRootNodeID(uint8_t nodeID)
{
    rootNodeID_.store(nodeID, std::memory_order_release);
}

void ControllerMetrics::SetIRMNodeID(uint8_t nodeID)
{
    irmNodeID_.store(nodeID, std::memory_order_release);
}

uint8_t ControllerMetrics::GetNodeCount() const
{
    return nodeCount_.load(std::memory_order_acquire);
}

uint8_t ControllerMetrics::GetLocalNodeID() const
{
    return localNodeID_.load(std::memory_order_acquire);
}

uint8_t ControllerMetrics::GetRootNodeID() const
{
    return rootNodeID_.load(std::memory_order_acquire);
}

uint8_t ControllerMetrics::GetIRMNodeID() const
{
    return irmNodeID_.load(std::memory_order_acquire);
}

void ControllerMetrics::SetControllerState(const char* stateName)
{
    if (stateName) {
        strlcpy(stateName_, stateName, sizeof(stateName_));
    }
}

const char* ControllerMetrics::GetControllerState() const
{
    return stateName_;
}

uint64_t ControllerMetrics::GetUptimeNanoseconds() const
{
    return GetCurrentTimeNanos() - startTime_;
}

void ControllerMetrics::Reset()
{
    busResetCount_.store(0, std::memory_order_release);
    currentGeneration_.store(0, std::memory_order_release);
    lastResetTimestamp_.store(0, std::memory_order_release);
    arRequestPackets_.store(0, std::memory_order_release);
    arResponsePackets_.store(0, std::memory_order_release);
    atRequestsCompleted_.store(0, std::memory_order_release);
    atResponsesCompleted_.store(0, std::memory_order_release);
    nodeCount_.store(0, std::memory_order_release);
    localNodeID_.store(0xFF, std::memory_order_release);
    rootNodeID_.store(0xFF, std::memory_order_release);
    irmNodeID_.store(0xFF, std::memory_order_release);
    strlcpy(stateName_, "Reset", sizeof(stateName_));
}

} // namespace ASFW::Driver
