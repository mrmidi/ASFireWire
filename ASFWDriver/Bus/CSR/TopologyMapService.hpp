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

    /**
     * @brief Rebuilds the topology map from a stable topology snapshot.
     */
    void Rebuild(const ASFW::Driver::TopologySnapshot& snapshot) noexcept;

    // ITopologyMapProvider overrides
    [[nodiscard]] bool ReadQuadlet(uint32_t regionByteOffset, uint32_t& outValue) const noexcept override;
    [[nodiscard]] bool ResolveBlockRead(uint32_t regionByteOffset, uint32_t requestedLength,
                                        uint64_t& outPayloadDeviceAddress, uint32_t& outPayloadLength) const noexcept override;

private:
    ASFW::Driver::HardwareInterface* hardware_;
    mutable ASFW::Async::IOLockWrapper lock_;

    // Monotonic generation counter, never reset. Bumps on each Rebuild.
    uint32_t generation_{0};

    // Host-order copy for fast CPU quadlet reads.
    // Index 0..255 maps to region offsets 0..0x3FC.
    uint32_t hostMap_[256]{};

    // DMA buffer backing
    std::optional<ASFW::Driver::HardwareInterface::DMABuffer> dmaOpt_;
    OSSharedPtr<IOMemoryMap> dmaMap_;
    bool started_{false};

    void ZeroBuffer() noexcept;
};

} // namespace ASFW::Bus
