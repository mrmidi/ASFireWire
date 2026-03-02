#include "ROMScanSession.hpp"

#include "../../Async/Interfaces/IFireWireBus.hpp"
#include "../../Bus/TopologyManager.hpp"
#include "../../IRM/IRMTypes.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>

#include <array>
#include <cstring>

namespace ASFW::Discovery {

void ROMScanSession::StartIRMRead(ROMScanNodeStateMachine& node) {
    const uint8_t nodeId = node.NodeId();

    node.SetNeedsIRMCheck(true);
    if (!TransitionNodeState(node, ROMScanNodeStateMachine::State::VerifyingIRM_Read,
                             "BIB complete enter IRM verify read")) {
        Pump();
        return;
    }

    ++inflight_;

    Async::FWAddress addr{IRM::IRMRegisters::kAddressHi,
                          IRM::IRMRegisters::kChannelsAvailable63_32};

    const Generation gen = gen_;
    auto weakSelf = weak_from_this();
    const auto handle = bus_.ReadQuad(
        FW::Generation{static_cast<uint32_t>(gen)}, FW::NodeId{nodeId}, addr, FW::FwSpeed::S100,
        [weakSelf, nodeId](Async::AsyncStatus status, std::span<const uint8_t> payload) mutable {
            bool ok = status == Async::AsyncStatus::kSuccess && payload.size() == 4;
            uint32_t valueHost = 0;
            if (ok) {
                uint32_t raw = 0;
                std::memcpy(&raw, payload.data(), sizeof(raw));
                valueHost = OSSwapBigToHostInt32(raw);
            }

            if (auto self = weakSelf.lock(); self) {
                self->DispatchAsync([self = std::move(self), nodeId, ok, valueHost]() mutable {
                    self->HandleIRMReadComplete(nodeId, ok, valueHost);
                });
            }
        });

    if (!handle) {
        HandleIRMReadComplete(nodeId, /*success=*/false, 0);
    }
}

void ROMScanSession::HandleIRMReadComplete(uint8_t nodeId, bool success, uint32_t valueHostOrder) {
    if (aborted_.load(std::memory_order_relaxed)) {
        return;
    }
    if (gen_ == 0) {
        return;
    }

    if (inflight_ > 0) {
        --inflight_;
    }

    auto* nodePtr = FindNode(nodeId);
    if (nodePtr == nullptr) {
        Pump();
        return;
    }

    auto& node = *nodePtr;

    if (!success) {
        ASFW_LOG_V1(ConfigROM, "ROMScanSession: Node %u IRM read verify failed - marking bad IRM",
                    nodeId);
        node.SetIRMIsBad(true);
        if (topologyManager_ != nullptr && topology_.irmNodeId.has_value() &&
            *topology_.irmNodeId == nodeId) {
            topologyManager_->MarkNodeAsBadIRM(nodeId);
        }

        node.SetNeedsIRMCheck(false);
        ContinueAfterIRMCheck(node);
        return;
    }

    node.SetIRMBitBucket(valueHostOrder);
    node.SetIRMCheckReadDone(true);
    StartIRMLock(node);
}

void ROMScanSession::StartIRMLock(ROMScanNodeStateMachine& node) {
    const uint8_t nodeId = node.NodeId();

    if (!TransitionNodeState(node, ROMScanNodeStateMachine::State::VerifyingIRM_Lock,
                             "IRM read complete enter lock verify")) {
        Pump();
        return;
    }

    ++inflight_;

    Async::FWAddress addr{IRM::IRMRegisters::kAddressHi,
                          IRM::IRMRegisters::kChannelsAvailable63_32};

    std::array<uint8_t, 8> casOperand{};
    const uint32_t beCompare = OSSwapHostToBigInt32(0xFFFFFFFFU);
    const uint32_t beSwap = OSSwapHostToBigInt32(0xFFFFFFFFU);
    std::memcpy(casOperand.data(), &beCompare, sizeof(beCompare));
    std::memcpy(casOperand.data() + sizeof(beCompare), &beSwap, sizeof(beSwap));

    const Generation gen = gen_;
    auto weakSelf = weak_from_this();
    const auto handle = bus_.Lock(
        FW::Generation{static_cast<uint32_t>(gen)}, FW::NodeId{nodeId}, addr,
        FW::LockOp::kCompareSwap, std::span<const uint8_t>{casOperand},
        /*responseLength=*/4, FW::FwSpeed::S100,
        [weakSelf, nodeId](Async::AsyncStatus status, std::span<const uint8_t> payload) mutable {
            const bool ok = status == Async::AsyncStatus::kSuccess && payload.size() == 4;
            if (auto self = weakSelf.lock(); self) {
                self->DispatchAsync([self = std::move(self), nodeId, ok]() mutable {
                    self->HandleIRMLockComplete(nodeId, ok);
                });
            }
        });

    if (!handle) {
        HandleIRMLockComplete(nodeId, /*success=*/false);
    }
}

void ROMScanSession::HandleIRMLockComplete(uint8_t nodeId, bool success) {
    if (aborted_.load(std::memory_order_relaxed)) {
        return;
    }
    if (gen_ == 0) {
        return;
    }

    if (inflight_ > 0) {
        --inflight_;
    }

    auto* nodePtr = FindNode(nodeId);
    if (nodePtr == nullptr) {
        Pump();
        return;
    }

    auto& node = *nodePtr;

    if (!success) {
        ASFW_LOG_V1(ConfigROM, "ROMScanSession: Node %u IRM lock verify failed - marking bad IRM",
                    nodeId);
        node.SetIRMIsBad(true);
        if (topologyManager_ != nullptr && topology_.irmNodeId.has_value() &&
            *topology_.irmNodeId == nodeId) {
            topologyManager_->MarkNodeAsBadIRM(nodeId);
        }
    } else {
        node.SetIRMCheckLockDone(true);
    }

    node.SetNeedsIRMCheck(false);
    ContinueAfterIRMCheck(node);
}

void ROMScanSession::ContinueAfterIRMCheck(ROMScanNodeStateMachine& node) {
    if (node.ROM().bib.crcLength <= node.ROM().bib.busInfoLength) {
        if (!TransitionNodeState(node, ROMScanNodeStateMachine::State::Complete,
                                 "IRM check complete minimal ROM complete")) {
            Pump();
            return;
        }
        completedROMs_.push_back(std::move(node.MutableROM()));
        Pump();
        return;
    }

    StartRootDirRead(node);
}

} // namespace ASFW::Discovery
