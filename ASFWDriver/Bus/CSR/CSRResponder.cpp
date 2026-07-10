// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// CSRResponder.cpp — see CSRResponder.hpp. Semantics ported from in-tree Linux
// firewire (ohci_read_csr / ohci_write_csr / handle_registers).

#include "CSRResponder.hpp"
#include "BroadcastChannelCSR.hpp"

namespace ASFW::Bus {

using Async::ResponseCode;
namespace FW = ASFW::FW;

uint32_t CSRResponder::BroadcastChannel() const noexcept {
    return (deps_.broadcastChannel != nullptr) ? deps_.broadcastChannel->Read()
                                               : FW::kBroadcastChannelInitial;
}

CSRResponder::Result CSRResponder::HandleStateWrite(bool isSet, uint32_t value) noexcept {
    // Cycle-master bit: only the root node may flip its local cycle master. A
    // non-root node discards the write but still answers Complete (inert success,
    // never an error) — Linux ohci_write_csr.
    if ((value & FW::kCSRStateBitCMSTR) != 0) {
        if (deps_.root != nullptr && deps_.root->IsLocalRoot() && deps_.cycleMaster != nullptr) {
            deps_.cycleMaster->SetCycleMaster(isSet);
        }
    }

    // Abdicate bit: STATE_SET sets it, STATE_CLEAR clears it.
    if ((value & FW::kCSRStateBitABDICATE) != 0) {
        abdicate_ = isSet;
    }

    return Result{.mine = true, .rcode = ResponseCode::Complete, .readValue = 0};
}

CSRResponder::Result CSRResponder::HandleStateRead() const noexcept {
    uint32_t value = 0;
    if (deps_.root != nullptr && deps_.root->IsLocalRoot() && deps_.cycleMaster != nullptr &&
        deps_.cycleMaster->IsCycleMasterEnabled()) {
        value |= FW::kCSRStateBitCMSTR;
    }
    if (abdicate_) {
        value |= FW::kCSRStateBitABDICATE;
    }
    return Result{.mine = true, .rcode = ResponseCode::Complete, .readValue = value};
}

CSRResponder::Result CSRResponder::WriteQuadlet(uint32_t csrOffsetLo, uint32_t value) noexcept {
    // The SPEED_MAP is explicitly marked as Obsoleted in the 1394-2008 standard.
    // If it is implemented, it should be read-only.
    if (InSpeedMapRegion(csrOffsetLo)) {
        return Result{.mine = true, .rcode = ResponseCode::TypeError, .readValue = 0};
    }

    switch (csrOffsetLo) {
    case FW::kCSR_StateSet:
        return HandleStateWrite(/*isSet=*/true, value);
    case FW::kCSR_StateClear:
        return HandleStateWrite(/*isSet=*/false, value);
    case FW::kCSR_ResetStart:
        if (deps_.resetTrigger != nullptr) {
            deps_.resetTrigger->TriggerBusReset(/*shortReset=*/false);
        }
        return Result{.mine = true, .rcode = ResponseCode::Complete, .readValue = 0};
    case FW::kCSR_BroadcastChannel:
        if (deps_.broadcastChannel != nullptr) {
            deps_.broadcastChannel->Write(value);
        }
        return Result{.mine = true, .rcode = ResponseCode::Complete, .readValue = 0};
    default:
        break;
    }

    if (IsIrmResourceCsr(csrOffsetLo)) {
        unexpectedResourceCsrSoftwareCount_++;
        // OHCI serves these autonomously; never software-answer.
        return Result{.mine = false};
    }
    if (InTopologyRegion(csrOffsetLo)) {
        // TOPOLOGY_MAP is read-only.
        return Result{.mine = true, .rcode = ResponseCode::TypeError, .readValue = 0};
    }
    return Result{.mine = false};
}

CSRResponder::Result CSRResponder::ReadQuadlet(uint32_t csrOffsetLo) noexcept {
    switch (csrOffsetLo) {
    case FW::kCSR_StateSet:
    case FW::kCSR_StateClear:
        return HandleStateRead();
    case FW::kCSR_BroadcastChannel:
        return Result{.mine = true, .rcode = ResponseCode::Complete, .readValue = BroadcastChannel()};
    case FW::kCSR_ResetStart:
        // RESET_START is write-only.
        return Result{.mine = true, .rcode = ResponseCode::TypeError, .readValue = 0};
    default:
        break;
    }

    if (IsIrmResourceCsr(csrOffsetLo)) {
        unexpectedResourceCsrSoftwareCount_++;
        return Result{.mine = false};
    }

    if (InTopologyRegion(csrOffsetLo)) {
        if ((csrOffsetLo & 0x3u) != 0) {
            return Result{.mine = true, .rcode = ResponseCode::AddressError, .readValue = 0};
        }
        if (deps_.topologyMap == nullptr) {
            // No map service yet (pre-FW-20): decline so the caller falls through
            // unchanged rather than fabricating an answer.
            return Result{.mine = false};
        }
        const uint32_t regionOffset = csrOffsetLo - FW::kCSR_TopologyMapBase;
        uint32_t out = 0;
        if (deps_.topologyMap->ReadQuadlet(regionOffset, out)) {
            return Result{.mine = true, .rcode = ResponseCode::Complete, .readValue = out};
        }
        return Result{.mine = true, .rcode = ResponseCode::AddressError, .readValue = 0};
    }

    if (InSpeedMapRegion(csrOffsetLo)) {
        if ((csrOffsetLo & 0x3u) != 0) {
            return Result{.mine = true, .rcode = ResponseCode::AddressError, .readValue = 0};
        }
        if (deps_.speedMap == nullptr) {
            return Result{.mine = false};
        }
        const uint32_t regionOffset = csrOffsetLo - FW::kCSR_SpeedMapBase;
        uint32_t out = 0;
        if (deps_.speedMap->ReadQuadlet(regionOffset, out)) {
            return Result{.mine = true, .rcode = ResponseCode::Complete, .readValue = out};
        }
        return Result{.mine = true, .rcode = ResponseCode::AddressError, .readValue = 0};
    }

    return Result{.mine = false};
}

CSRResponder::Result CSRResponder::BlockReadClaim(uint32_t csrOffsetLo, uint32_t length) noexcept {
    if (InSpeedMapRegion(csrOffsetLo)) {
        // SPEED_MAP is obsolete but readable.
        return Result{.mine = true, .rcode = ResponseCode::TypeError, .readValue = 0};
    }
    if (!InTopologyRegion(csrOffsetLo)) {
        return Result{.mine = false};
    }
    if (deps_.topologyMap == nullptr) {
        // Until FW-20 installs a provider, decline so 0x5 routing is unchanged.
        return Result{.mine = false};
    }
    const uint32_t regionOffset = csrOffsetLo - FW::kCSR_TopologyMapBase;
    uint64_t addr = 0;
    uint32_t len = 0;
    if (deps_.topologyMap->ResolveBlockRead(regionOffset, length, addr, len)) {
        return Result{
            .mine = true,
            .rcode = ResponseCode::Complete,
            .readValue = 0,
            .readBlockDeviceAddress = addr,
            .readBlockLength = len
        };
    }
    return Result{.mine = true, .rcode = ResponseCode::AddressError, .readValue = 0};
}

} // namespace ASFW::Bus
