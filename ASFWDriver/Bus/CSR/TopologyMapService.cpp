// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// TopologyMapService.cpp — see TopologyMapService.hpp

#include "TopologyMapService.hpp"
#include "TopologyMapBuilder.hpp"
#include "../../Controller/ControllerTypes.hpp"
#include "../../Logging/Logging.hpp"
#include <cstring>

#ifndef ASFW_HOST_TEST
#include <DriverKit/IOLib.h>
#endif

namespace ASFW::Bus {

TopologyMapService::TopologyMapService(ASFW::Driver::HardwareInterface* hw) noexcept
    : hardware_(hw),
      lock_(ASFW::Async::IOLockWrapper(IOLockAlloc())) {
    // Initialise hostMap_ with a default empty map structure (generation 0)
    ASFW::Driver::TopologySnapshot emptySnap{};
    emptySnap.nodeCount = 0;
    emptySnap.generation = 0;
    std::span<uint32_t, 256> outSpan(hostMap_);
    BuildTopologyMap(emptySnap, 0, outSpan);
}

TopologyMapService::~TopologyMapService() {
    Stop();
    if (lock_.IsValid()) {
        IOLockFree(lock_.Raw());
        lock_ = ASFW::Async::IOLockWrapper(nullptr);
    }
}

bool TopologyMapService::Start() noexcept {
    ASFW::Async::IOScopedLock guard(lock_);
    return StartLocked();
}

bool TopologyMapService::StartLocked() noexcept {
    if (started_) {
        return true;
    }
    if (hardware_ == nullptr) {
        ASFW_LOG(Controller, "❌ TopologyMapService: Start failed, hardware_ is null");
        InvalidateLocked();
        return false;
    }

    // Allocate 1 KiB DMA buffer
    auto opt = hardware_->AllocateDMA(1024, kIOMemoryDirectionInOut, 1024);
    if (!opt) {
        ASFW_LOG(Controller, "❌ TopologyMapService: Start failed, AllocateDMA failed");
        InvalidateLocked();
        return false;
    }
    dmaOpt_ = opt;

    // Create memory mapping for host CPU access
    IOMemoryMap* rawMap = nullptr;
    const kern_return_t kr = dmaOpt_->descriptor->CreateMapping(0, 0, 0, 0, 0, &rawMap);
    if (kr != kIOReturnSuccess || rawMap == nullptr) {
        ASFW_LOG(Controller, "❌ TopologyMapService: Start failed, CreateMapping failed kr=0x%x", kr);
        dmaOpt_.reset();
        InvalidateLocked();
        return false;
    }
    dmaMap_ = OSSharedPtr<IOMemoryMap>(rawMap, OSNoRetain);

    ZeroBuffer();
    started_ = true;

    // Write initial BE image to the mapped buffer
    auto* const base = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(dmaMap_->GetAddress()));
    for (size_t i = 0; i < 256; ++i) {
        base[i] = OSSwapHostToBigInt32(hostMap_[i]);
    }
    OSSynchronizeIO();

    ASFW_LOG(Controller, "✅ TopologyMapService: Started, DMA buffer allocated at 0x%llx", dmaOpt_->deviceAddress);
    return true;
}

void TopologyMapService::Stop() noexcept {
    if (!started_) {
        return;
    }
    Invalidate();
    dmaMap_.reset();
    dmaOpt_.reset();
    started_ = false;
    ASFW_LOG(Controller, "TopologyMapService: Stopped");
}

void TopologyMapService::Rebuild(const ASFW::Driver::TopologySnapshot& snapshot) noexcept {
    ASFW::Async::IOScopedLock guard(lock_);
    if (!started_ && !StartLocked()) {
        ASFW_LOG(Controller, "❌ TopologyMapService: Rebuild failed, service cannot be started");
        return;
    }

    generation_++;

    std::span<uint32_t, 256> outSpan(hostMap_);
    const uint32_t filledQuads = BuildTopologyMap(snapshot, generation_, outSpan);
    publishStatus_ = TopologyMapPublishStatus::Valid;

    // Sync to big-endian DMA mirror
    if (dmaMap_) {
        auto* const base = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(dmaMap_->GetAddress()));
        
        // Step 1: Write DMA header length = 0 first to invalidate
        base[0] = 0;
        OSSynchronizeIO();

        // Step 2: Write body / self-ID quadlets
        for (size_t i = 1; i < 256; ++i) {
            base[i] = OSSwapHostToBigInt32(hostMap_[i]);
        }
        OSSynchronizeIO();

        // Step 3: Write final header with length + CRC last
        base[0] = OSSwapHostToBigInt32(hostMap_[0]);
        OSSynchronizeIO();
    }

    const uint32_t selfIdCount = (filledQuads > 3) ? (filledQuads - 3) : 0;
    ASFW_LOG(Controller, "TopologyMapService: Map rebuilt for generation %u: nodes=%u, selfIds=%u",
             generation_, snapshot.nodeCount, selfIdCount);
}

void TopologyMapService::PublishZeroLength(uint32_t generation) noexcept {
    ASFW::Async::IOScopedLock guard(lock_);
    if (!started_ && !StartLocked()) {
        return;
    }

    // A zero-length map according to IEEE 1212 is just a header with length=0.
    // We use our local generation_ for continuity.
    generation_++;
    publishStatus_ = TopologyMapPublishStatus::ZeroLengthDueToTopologyError;

    std::memset(hostMap_, 0, sizeof(hostMap_));
    // q0: [length:16][generation:16]
    hostMap_[0] = (generation & 0xFFFFu); // length = 0

    if (dmaMap_) {
        auto* const base = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(dmaMap_->GetAddress()));
        base[0] = OSSwapHostToBigInt32(hostMap_[0]);
        OSSynchronizeIO();
    }

    ASFW_LOG(Controller, "TopologyMapService: Zero-length map published for generation %u", generation_);
}

void TopologyMapService::InvalidateLocked() noexcept {
    hostMap_[0] = 0;
    publishStatus_ = TopologyMapPublishStatus::Invalid;
    if (dmaMap_) {
        auto* const base = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(dmaMap_->GetAddress()));
        base[0] = 0;
        OSSynchronizeIO();
    }
}

void TopologyMapService::Invalidate() noexcept {
    ASFW::Async::IOScopedLock guard(lock_);
    InvalidateLocked();
}

bool TopologyMapService::ReadQuadlet(uint32_t regionByteOffset, uint32_t& outValue) const noexcept {
    ASFW::Async::IOScopedLock guard(lock_);
    if (regionByteOffset % 4 != 0 || regionByteOffset >= 1024) {
        return false;
    }
    outValue = hostMap_[regionByteOffset / 4];
    return true;
}

bool TopologyMapService::ResolveBlockRead(uint32_t regionByteOffset, uint32_t requestedLength,
                                         uint64_t& outPayloadDeviceAddress, uint32_t& outPayloadLength) const noexcept {
    ASFW::Async::IOScopedLock guard(lock_);
    if (!started_ || !dmaOpt_) {
        return false;
    }
    if (regionByteOffset % 4 != 0 || requestedLength % 4 != 0) {
        return false;
    }
    if (regionByteOffset + requestedLength > 1024) {
        return false;
    }
    outPayloadDeviceAddress = dmaOpt_->deviceAddress + regionByteOffset;
    outPayloadLength = requestedLength;
    return true;
}

void TopologyMapService::ZeroBuffer() noexcept {
    if (!dmaMap_) {
        return;
    }
    auto* const base = reinterpret_cast<std::byte*>(static_cast<uintptr_t>(dmaMap_->GetAddress()));
    std::memset(base, 0, 1024);
}

} // namespace ASFW::Bus
