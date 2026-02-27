#include "ROMScannerBIBPhase.hpp"
#include "ConfigROMConstants.hpp"
#include "../Async/Interfaces/IFireWireBus.hpp"
#include "ConfigROMStore.hpp"
#include "../IRM/IRMTypes.hpp"
#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"

#include <cstring>

namespace ASFW::Discovery {

void ROMScanner::OnBIBComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    ROMScannerBIBPhase::HandleCompletion(*this, nodeId, result);
}

void ROMScannerBIBPhase::HandleCompletion(ROMScanner& scanner,
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
    node.SetBIBInProgress(false);

    if (!result.success) {
        ASFW_LOG(ConfigROM, "FSM: Node %u BIB read failed (ack_busy/error), retrying", nodeId);
        scanner.hadBusyNodes_ = true;
        scanner.RetryWithFallback(node);
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }

    // IEEE 1212 allows temporary zero in header while device is still booting.
    if (result.dataLength >= 4 && result.data[0] == 0) {
        ASFW_LOG(ConfigROM, "FSM: Node %u BIB quadlet[0]=0 (booting), retry", nodeId);
        scanner.hadBusyNodes_ = true;
        scanner.RetryWithFallback(node);
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }

    auto bibOpt = ConfigROMParser::ParseBIB(result.data);
    if (!bibOpt.has_value()) {
        ASFW_LOG(ConfigROM, "FSM: Node %u BIB parse failed", nodeId);
        scanner.TransitionNodeState(node, ROMScanner::NodeState::Failed, "BIB parse failed");
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }

    node.MutableROM().bib = bibOpt.value();

    const uint32_t bibQuadlets = result.dataLength / 4;
    node.MutableROM().rawQuadlets.clear();
    node.MutableROM().rawQuadlets.reserve(256);
    if (result.data && bibQuadlets > 0) {
        for (uint32_t i = 0; i < bibQuadlets; ++i) {
            node.MutableROM().rawQuadlets.push_back(result.data[i]);
        }
    }

    scanner.speedPolicy_.RecordSuccess(nodeId, node.CurrentSpeed());

    if (node.ROM().bib.crcLength <= node.ROM().bib.busInfoLength) {
        if (!scanner.TransitionNodeState(node, ROMScanner::NodeState::Complete, "BIB minimal ROM complete")) {
            scanner.CheckAndNotifyCompletion();
            scanner.ScheduleAdvanceFSM();
            return;
        }
        scanner.completedROMs_.push_back(std::move(node.MutableROM()));
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }

    node.SetNeedsIRMCheck(scanner.params_.doIRMCheck);
    if (node.NeedsIRMCheck()) {
        if (!scanner.TransitionNodeState(node,
                                         ROMScanner::NodeState::VerifyingIRM_Read,
                                         "BIB complete enter IRM read")) {
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
            scanner.PublishReadEvent(ROMScannerEventType::IRMReadComplete, nodeId, mockResult);
        };

        Async::FWAddress addr(IRM::IRMRegisters::kAddressHi,
                              IRM::IRMRegisters::kChannelsAvailable63_32,
                              static_cast<uint16_t>((scanner.currentTopology_.busNumber.value_or(0) << 10) | nodeId));
        scanner.bus_.ReadQuad(FW::Generation(scanner.currentGen_), FW::NodeId(nodeId), addr,
                              FW::FwSpeed::S100, callback);
        scanner.ScheduleAdvanceFSM();
        return;
    }

    if (!scanner.TransitionNodeState(node,
                                     ROMScanner::NodeState::ReadingRootDir,
                                     "BIB complete enter root dir read")) {
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
