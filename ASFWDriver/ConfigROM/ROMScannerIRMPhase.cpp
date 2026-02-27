#include "ROMScannerIRMPhase.hpp"
#include "ConfigROMConstants.hpp"
#include "../Async/Interfaces/IFireWireBus.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../IRM/IRMTypes.hpp"
#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"
#include <DriverKit/IOLib.h>

#include <array>
#include <cstring>

namespace ASFW::Discovery {

void ROMScanner::OnIRMReadComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    ROMScannerIRMPhase::HandleReadCompletion(*this, nodeId, result);
}

void ROMScanner::OnIRMLockComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    ROMScannerIRMPhase::HandleLockCompletion(*this, nodeId, result);
}

void ROMScanner::HandleIRMLockResult(ROMScanNodeStateMachine& node, const ROMReader::ReadResult& result) {
    ROMScannerIRMPhase::HandleLockResult(*this, node, result);
}

void ROMScannerIRMPhase::HandleReadCompletion(ROMScanner& scanner,
                                              uint8_t nodeId,
                                              const ROMReader::ReadResult& result) {
    scanner.DecrementInflight();

    auto* nodePtr = scanner.FindNodeScan(nodeId);
    if (!nodePtr) {
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }

    auto& node = *nodePtr;

    if (!result.success || !result.data) {
        ASFW_LOG_V1(ConfigROM, "Node %u IRM read test failed - marking as bad IRM", nodeId);
        node.SetIRMIsBad(true);

        if (scanner.topologyManager_ && scanner.currentTopology_.irmNodeId.has_value() &&
            *scanner.currentTopology_.irmNodeId == nodeId) {
            scanner.topologyManager_->MarkNodeAsBadIRM(nodeId);
        }

        if (!scanner.TransitionNodeState(node,
                                         ROMScanner::NodeState::ReadingRootDir,
                                         "IRM read failed continue with root dir")) {
            scanner.CheckAndNotifyCompletion();
            scanner.ScheduleAdvanceFSM();
            return;
        }
        node.SetRetriesLeft(scanner.params_.perStepRetries);
        scanner.IncrementInflight();

        const uint32_t offsetBytes = ASFW::ConfigROM::RootDirStartBytes(node.ROM().bib);
        auto callback = [&scanner, nodeId](const ROMReader::ReadResult& res) {
            scanner.PublishReadEvent(ROMScannerEventType::RootDirComplete, nodeId, res);
        };

        scanner.reader_->ReadRootDirQuadlets(nodeId, scanner.currentGen_, node.CurrentSpeed(),
                                             offsetBytes, 0, callback);
        scanner.ScheduleAdvanceFSM();
        return;
    }

    node.SetIRMBitBucket(OSSwapBigToHostInt32(*result.data));
    node.SetIRMCheckReadDone(true);
    if (!scanner.TransitionNodeState(node,
                                     ROMScanner::NodeState::VerifyingIRM_Lock,
                                     "IRM read success enter lock verify")) {
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }
    scanner.IncrementInflight();

    auto callback = [&scanner, nodeId](Async::AsyncStatus status, std::span<const uint8_t> payload) {
        ROMReader::ReadResult mockResult{};
        mockResult.success = (status == Async::AsyncStatus::kSuccess);
        mockResult.nodeId = nodeId;
        mockResult.generation = scanner.currentGen_;
        uint32_t dataQuadlet = 0;
        if (mockResult.success && payload.size() >= 4) {
            mockResult.dataLength = 4;
            std::memcpy(&dataQuadlet, payload.data(), sizeof(dataQuadlet));
            mockResult.data = &dataQuadlet;
        }
        scanner.PublishReadEvent(ROMScannerEventType::IRMLockComplete, nodeId, mockResult);
    };

    Async::FWAddress addr(IRM::IRMRegisters::kAddressHi,
                          IRM::IRMRegisters::kChannelsAvailable63_32,
                          static_cast<uint16_t>((scanner.currentTopology_.busNumber.value_or(0) << 10) | nodeId));

    std::array<uint8_t, 8> casOperand{};
    const uint32_t beCompare = OSSwapHostToBigInt32(0xFFFFFFFFu);
    const uint32_t beSwap = OSSwapHostToBigInt32(0xFFFFFFFFu);
    std::memcpy(casOperand.data(), &beCompare, sizeof(beCompare));
    std::memcpy(casOperand.data() + 4, &beSwap, sizeof(beSwap));

    if (auto handle = scanner.bus_.Lock(FW::Generation(scanner.currentGen_),
                                        FW::NodeId(nodeId),
                                        addr,
                                        FW::LockOp::kCompareSwap,
                                        std::span<const uint8_t>(casOperand),
                                        4,
                                        FW::FwSpeed::S100,
                                        callback);
        !handle) {
        scanner.DecrementInflight();
        ROMReader::ReadResult failure{};
        failure.success = false;
        ROMScannerIRMPhase::HandleLockResult(scanner, node, failure);
        return;
    }

    scanner.ScheduleAdvanceFSM();
}

void ROMScannerIRMPhase::HandleLockCompletion(ROMScanner& scanner,
                                              uint8_t nodeId,
                                              const ROMReader::ReadResult& result) {
    scanner.DecrementInflight();

    auto* nodePtr = scanner.FindNodeScan(nodeId);
    if (!nodePtr) {
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }

    ROMScannerIRMPhase::HandleLockResult(scanner, *nodePtr, result);
}

void ROMScannerIRMPhase::HandleLockResult(ROMScanner& scanner,
                                          ROMScanNodeStateMachine& node,
                                          const ROMReader::ReadResult& result) {
    const uint8_t nodeId = node.NodeId();

    if (!result.success || !result.data) {
        node.SetIRMIsBad(true);
        if (scanner.topologyManager_ && scanner.currentTopology_.irmNodeId.has_value() &&
            *scanner.currentTopology_.irmNodeId == nodeId) {
            scanner.topologyManager_->MarkNodeAsBadIRM(nodeId);
        }
    } else {
        node.SetIRMCheckLockDone(true);
    }

    if (!scanner.TransitionNodeState(node,
                                     ROMScanner::NodeState::ReadingRootDir,
                                     "IRM lock handling enter root dir read")) {
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }
    node.SetRetriesLeft(scanner.params_.perStepRetries);
    scanner.IncrementInflight();

    const uint32_t offsetBytes = ASFW::ConfigROM::RootDirStartBytes(node.ROM().bib);
    auto callback = [&scanner, nodeId](const ROMReader::ReadResult& res) {
        scanner.PublishReadEvent(ROMScannerEventType::RootDirComplete, nodeId, res);
    };

    scanner.reader_->ReadRootDirQuadlets(nodeId, scanner.currentGen_, node.CurrentSpeed(),
                                         offsetBytes, 0, callback);
    scanner.ScheduleAdvanceFSM();
}

} // namespace ASFW::Discovery
