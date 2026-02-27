#include "ROMScannerFSMFlow.hpp"

#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"

namespace ASFW::Discovery {

void ROMScannerFSMFlow::OnEnsurePrefixReadComplete(ROMScanner& scanner,
                                                   uint8_t nodeId,
                                                   uint32_t requiredTotalQuadlets,
                                                   const std::shared_ptr<std::function<void(bool)>>& completion,
                                                   const ROMReader::ReadResult& res) {
    scanner.DecrementInflight();

    auto* node = scanner.FindNodeScan(nodeId);
    if (!node) {
        FinishEnsurePrefixStep(scanner, completion, false);
        return;
    }

    if (!res.success || !res.data || res.dataLength == 0) {
        ASFW_LOG(ConfigROM, "EnsurePrefix read failed: node=%u", nodeId);
        FinishEnsurePrefixStep(scanner, completion, false);
        return;
    }

    const uint32_t gotQuadlets = res.dataLength / 4;
    auto& rawQuadlets = node->MutableROM().rawQuadlets;
    rawQuadlets.reserve(rawQuadlets.size() + gotQuadlets);
    for (uint32_t i = 0; i < gotQuadlets; ++i) {
        rawQuadlets.push_back(res.data[i]);
    }

    const bool ok = rawQuadlets.size() >= requiredTotalQuadlets;
    if (!ok) {
        ASFW_LOG_V2(ConfigROM,
                    "EnsurePrefix short read: node=%u have=%zu required=%u",
                    nodeId, rawQuadlets.size(), requiredTotalQuadlets);
    }

    FinishEnsurePrefixStep(scanner, completion, ok);
}

void ROMScannerFSMFlow::FinishEnsurePrefixStep(ROMScanner& scanner,
                                               const std::shared_ptr<std::function<void(bool)>>& completion,
                                               bool ok) {
    if (completion && *completion) {
        (*completion)(ok);
    }
    scanner.CheckAndNotifyCompletion();
    scanner.ScheduleAdvanceFSM();
}

} // namespace ASFW::Discovery
