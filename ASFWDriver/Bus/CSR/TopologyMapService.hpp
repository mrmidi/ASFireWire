// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// TopologyMapService.hpp — serves the IEEE 1394 topology map region (FW-20).

#pragma once

#include "CSRResponder.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Async/Core/LockPolicy.hpp"
#include <cstdint>
#include <span>
#include <optional>

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOMemoryMap.h>
#endif

namespace ASFW::Bus {

/**
 * @brief Status of the published TOPOLOGY_MAP.
 */
enum class TopologyMapPublishStatus : uint8_t {
    Invalid = 0,
    Valid,
    ZeroLengthDueToTopologyError,
    StaleGeneration,
};

class TopologyMapService final : public ITopologyMapProvider {
public:
    explicit TopologyMapService(ASFW::Driver::HardwareInterface* hw) noexcept;
    ~TopologyMapService() override;

    // Disallow copy/move
    TopologyMapService(const TopologyMapService&) = delete;
    TopologyMapService& operator=(const TopologyMapService&) = delete;

    /**
     * @brief Allocates and prepares the 1 KiB DMA buffer for remote access.
     * Must be called during setup/initialisation.
     * @return true on success, false on failure.
     */
    [[nodiscard]] bool Start() noexcept;

    /**
     * @brief Clears map backing and releases the DMA buffer/mappings.
     */
    void Stop() noexcept;

    void Rebuild(const ASFW::Driver::TopologySnapshot& snapshot) noexcept;

    /**
     * @brief Publishes a zero-length map header for an invalid topology.
     */
    void PublishZeroLength(uint32_t generation) noexcept;

    /**
     * @brief Thread-safely invalidates the map, clearing its validation status and contents.
     */
    void Invalidate() noexcept;

    // ITopologyMapProvider overrides
    [[nodiscard]] bool ReadQuadlet(uint32_t regionByteOffset, uint32_t& outValue) const noexcept override;
    [[nodiscard]] bool ResolveBlockRead(uint32_t regionByteOffset, uint32_t requestedLength,
                                        uint64_t& outPayloadDeviceAddress, uint32_t& outPayloadLength) const noexcept override;

    // Diagnostic getters
    [[nodiscard]] bool IsValid() const noexcept {
        ASFW::Async::IOScopedLock guard(lock_);
        return hostMap_[0] != 0;
    }
    [[nodiscard]] uint32_t GetGeneration() const noexcept {
        ASFW::Async::IOScopedLock guard(lock_);
        return generation_;
    }
    [[nodiscard]] uint32_t GetSelfIdCount() const noexcept {
        ASFW::Async::IOScopedLock guard(lock_);
        return hostMap_[2] & 0xFFFFu;
    }
    [[nodiscard]] uint16_t GetCRC() const noexcept {
        ASFW::Async::IOScopedLock guard(lock_);
        return static_cast<uint16_t>(hostMap_[0] & 0xFFFFu);
    }
    [[nodiscard]] bool IsDMAReady() const noexcept {
        ASFW::Async::IOScopedLock guard(lock_);
        return started_ && dmaMap_;
    }
    [[nodiscard]] TopologyMapPublishStatus PublishStatus() const noexcept {
        ASFW::Async::IOScopedLock guard(lock_);
        return publishStatus_;
    }

private:
    ASFW::Driver::HardwareInterface* hardware_;
    mutable ASFW::Async::IOLockWrapper lock_;

    // Published map generation. Mirrors the Self-ID/bus generation used by the
    // rest of the CSR diagnostics instead of a private publish counter.
    uint32_t generation_{0};
    TopologyMapPublishStatus publishStatus_{TopologyMapPublishStatus::Invalid};

    // Host-order copy for fast CPU quadlet reads.
    // Index 0..255 maps to region offsets 0..0x3FC.
    uint32_t hostMap_[256]{};

    // DMA buffer backing
    std::optional<ASFW::Driver::HardwareInterface::DMABuffer> dmaOpt_;
    OSSharedPtr<IOMemoryMap> dmaMap_;
    bool started_{false};

    void ZeroBuffer() noexcept;
    void InvalidateLocked() noexcept;
    bool StartLocked() noexcept;
};

} // namespace ASFW::Bus
