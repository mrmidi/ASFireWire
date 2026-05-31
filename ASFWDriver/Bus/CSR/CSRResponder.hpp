// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CSRResponder.hpp — local software-owned CSR responder surface (FW-19).
//
// Answers inbound async requests targeting the local node's *software-owned* CSR
// registers: STATE_CLEAR/STATE_SET (incl. cycle-master + abdicate), RESET_START,
// BROADCAST_CHANNEL, and the TOPOLOGY_MAP region (delegated to a provider added
// in FW-20). The four IRM resource CSRs (BUS_MANAGER_ID / BANDWIDTH_AVAILABLE /
// CHANNELS_AVAILABLE_HI/LO) are served autonomously by the OHCI CSR engine and
// are deliberately NOT handled here (returns NotMine), matching the Linux model
// (handle_registers() reaches BUG() for those offsets).
//
// Semantics are ported from in-tree Linux firewire (ohci_read_csr/ohci_write_csr,
// handle_registers): a STATE_SET.cmstr write only flips local cycle-master when
// the local node is root; otherwise it is discarded but still answered Complete.
//
// The class is pure logic (no DriverKit / logging) so it builds and runs under
// ASFW_HOST_TEST. Hardware effects are injected via thin interfaces.

#pragma once

#include "../../Async/ResponseCode.hpp"
#include "../../Common/CSRSpace.hpp"

#include <cstdint>

namespace ASFW::Bus {

// Reports whether the local node is currently the bus root. Backed by the OHCI
// NodeID register (NodeIDBits::kRoot) in production; a fake in tests.
struct IRootStatus {
    virtual ~IRootStatus() = default;
    [[nodiscard]] virtual bool IsLocalRoot() const noexcept = 0;
};

// Drives the local OHCI LinkControl cycleMaster bit. SetCycleMaster(true) maps to
// LinkControlSet.cycleMaster; false maps to LinkControlClear.cycleMaster.
struct ICycleMasterControl {
    virtual ~ICycleMasterControl() = default;
    virtual void SetCycleMaster(bool enable) noexcept = 0;
    [[nodiscard]] virtual bool IsCycleMasterEnabled() const noexcept = 0;
};

// Triggers a physical bus reset on the local bus.
struct IBusResetTrigger {
    virtual ~IBusResetTrigger() = default;
    virtual void TriggerBusReset(bool shortReset) noexcept = 0;
};

class BroadcastChannelCSR;

// Supplies TOPOLOGY_MAP quadlets. Installed by FW-20; until then the responder
// holds a null provider and declines the region (NotMine → caller falls through,
// preserving today's behavior).
struct ITopologyMapProvider {
    virtual ~ITopologyMapProvider() = default;
    // regionByteOffset is relative to kCSR_TopologyMapBase (0..0x3FF, quad-aligned).
    // Returns false if the offset is out of the 1 KiB topology map region.
    [[nodiscard]] virtual bool ReadQuadlet(uint32_t regionByteOffset,
                                           uint32_t& outValue) const noexcept = 0;
    // Gated/implemented under FW-20: resolves block-read targets targeting topology map.
    [[nodiscard]] virtual bool ResolveBlockRead(uint32_t regionByteOffset, uint32_t requestedLength,
                                                uint64_t& outPayloadDeviceAddress, uint32_t& outPayloadLength) const noexcept = 0;
};

class CSRResponder {
public:
    struct Deps {
        IRootStatus* root{nullptr};
        ICycleMasterControl* cycleMaster{nullptr};
        IBusResetTrigger* resetTrigger{nullptr};
        ITopologyMapProvider* topologyMap{nullptr}; // null until FW-20
        BroadcastChannelCSR* broadcastChannel{nullptr};
    };

    // Outcome of a dispatch. When mine == false the caller must fall through to
    // the next handler in the tCode chain (SBP-2 / FCP / DICE).
    struct Result {
        bool mine{false};
        Async::ResponseCode rcode{Async::ResponseCode::AddressError};
        uint32_t readValue{0}; // valid for read dispatches when rcode == Complete
        uint64_t readBlockDeviceAddress{0};
        uint32_t readBlockLength{0};
    };

    explicit CSRResponder(Deps deps) noexcept : deps_(deps) {}

    // Inbound quadlet write (tCode 0x0). `value` is the host-order data quadlet.
    Result WriteQuadlet(uint32_t csrOffsetLo, uint32_t value) noexcept;

    // Inbound quadlet read (tCode 0x4).
    [[nodiscard]] Result ReadQuadlet(uint32_t csrOffsetLo) noexcept;

    // Inbound block read (tCode 0x5). Only the TOPOLOGY_MAP region is ours, and
    // only once a provider is installed (FW-20). Until then returns NotMine.
    [[nodiscard]] Result BlockReadClaim(uint32_t csrOffsetLo, uint32_t length) noexcept;

    // Abdicate flag (IEEE 1394a STATE_SET/CLEAR bit 10). FW-18's election path
    // consumes this once per bus reset to decide incumbent-vs-challenger timing.
    [[nodiscard]] bool Abdicate() const noexcept { return abdicate_; }
    bool ConsumeAbdicate() noexcept {
        const bool prev = abdicate_;
        abdicate_ = false;
        return prev;
    }

    // Current BROADCAST_CHANNEL register value (diagnostics/tests).
    [[nodiscard]] uint32_t BroadcastChannel() const noexcept;

    void SetTopologyMapProvider(ITopologyMapProvider* provider) noexcept {
        deps_.topologyMap = provider;
    }

    [[nodiscard]] uint32_t UnexpectedResourceCsrSoftwareCount() const noexcept {
        return unexpectedResourceCsrSoftwareCount_;
    }

private:
    [[nodiscard]] Result HandleStateWrite(bool isSet, uint32_t value) noexcept;
    [[nodiscard]] Result HandleStateRead() const noexcept;
    [[nodiscard]] static bool InTopologyRegion(uint32_t csrOffsetLo) noexcept {
        return csrOffsetLo >= ASFW::FW::kCSR_TopologyMapBase &&
               csrOffsetLo <= ASFW::FW::kCSR_TopologyMapEnd;
    }
    [[nodiscard]] static bool IsIrmResourceCsr(uint32_t csrOffsetLo) noexcept {
        return csrOffsetLo == ASFW::FW::kCSR_BusManagerID ||
               csrOffsetLo == ASFW::FW::kCSR_BandwidthAvailable ||
               csrOffsetLo == ASFW::FW::kCSR_ChannelsAvailableHi ||
               csrOffsetLo == ASFW::FW::kCSR_ChannelsAvailableLo;
    }
    [[nodiscard]] static bool InSpeedMapRegion(uint32_t csrOffsetLo) noexcept {
        return csrOffsetLo >= ASFW::FW::kCSR_SpeedMapBase &&
               csrOffsetLo <= ASFW::FW::kCSR_SpeedMapEnd;
    }

    Deps deps_;
    bool abdicate_{false};
    uint32_t unexpectedResourceCsrSoftwareCount_{0};
};

} // namespace ASFW::Bus
