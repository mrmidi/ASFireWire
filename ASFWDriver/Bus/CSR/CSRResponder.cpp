// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CSRResponder.cpp — see CSRResponder.hpp. Semantics ported from in-tree Linux
// firewire (ohci_read_csr / ohci_write_csr / handle_registers).

#include "CSRResponder.hpp"

namespace ASFW::Bus {

using Async::ResponseCode;
namespace FW = ASFW::FW;

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
    switch (csrOffsetLo) {
    case FW::kCSR_StateSet:
        return HandleStateWrite(/*isSet=*/true, value);
    case FW::kCSR_StateClear:
        return HandleStateWrite(/*isSet=*/false, value);
    case FW::kCSR_ResetStart:
        // RESET_START write behaves as STATE_CLEAR with the ABDICATE bit set,
        // i.e. it clears the abdicate flag (Linux core-transaction.c).
        abdicate_ = false;
        return Result{.mine = true, .rcode = ResponseCode::Complete, .readValue = 0};
    case FW::kCSR_BroadcastChannel:
        // Only the VALID bit is writable; the channel number and transmit-valid
        // bit are pinned to INITIAL (Linux handle_registers BROADCAST_CHANNEL).
        broadcastChannel_ = (value & FW::kBroadcastChannelValid) | FW::kBroadcastChannelInitial;
        return Result{.mine = true, .rcode = ResponseCode::Complete, .readValue = 0};
    default:
        break;
    }

    if (IsIrmResourceCsr(csrOffsetLo)) {
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
        return Result{.mine = true, .rcode = ResponseCode::Complete, .readValue = broadcastChannel_};
    case FW::kCSR_ResetStart:
        // RESET_START is write-only.
        return Result{.mine = true, .rcode = ResponseCode::TypeError, .readValue = 0};
    default:
        break;
    }

    if (IsIrmResourceCsr(csrOffsetLo)) {
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

    return Result{.mine = false};
}

CSRResponder::Result CSRResponder::BlockReadClaim(uint32_t csrOffsetLo, uint32_t length) noexcept {
    if (!InTopologyRegion(csrOffsetLo)) {
        return Result{.mine = false};
    }
    if (deps_.topologyMap == nullptr) {
        // Until FW-20 installs a provider, decline so 0x5 routing is unchanged.
        return Result{.mine = false};
    }
    // Gated/disabled for block reads until the DMA-backed path is implemented under FW-20.
    // Return AddressError rather than Complete with 0 payload.
    return Result{.mine = true, .rcode = ResponseCode::AddressError, .readValue = 0};
}

} // namespace ASFW::Bus
