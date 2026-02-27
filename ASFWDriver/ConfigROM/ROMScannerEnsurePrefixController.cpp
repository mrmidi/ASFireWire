#include "ROMScannerEnsurePrefixController.hpp"

#include "ConfigROMConstants.hpp"
#include "../Logging/LogConfig.hpp"
#include "../Logging/Logging.hpp"

namespace ASFW::Discovery {

void ROMScannerEnsurePrefixController::EnsurePrefix(
    uint8_t nodeId,
    uint32_t requiredTotalQuadlets,
    Generation currentGen,
    ROMReader& reader,
    const std::function<ROMScanNodeStateMachine*(uint8_t)>& findNodeScan,
    const std::function<void()>& incrementInflight,
    const std::function<void(uint8_t,
                             uint32_t,
                             const std::function<void(bool)>&,
                             const ROMReader::ReadResult&)>& publishEnsurePrefixEvent,
    std::function<void(bool)> completion) const {
    const auto* nodePtr = findNodeScan(nodeId);
    if (!nodePtr) {
        if (completion) {
            completion(false);
        }
        return;
    }

    const auto& node = *nodePtr;

    if (requiredTotalQuadlets > ASFW::ConfigROM::kMaxROMPrefixQuadlets) {
        ASFW_LOG(ConfigROM,
                 "EnsurePrefix: node=%u required=%u exceeds max ROM prefix (%u quadlets), skipping",
                 nodeId,
                 requiredTotalQuadlets,
                 ASFW::ConfigROM::kMaxROMPrefixQuadlets);
        if (completion) {
            completion(false);
        }
        return;
    }

    const auto have = static_cast<uint32_t>(node.ROM().rawQuadlets.size());
    if (have >= requiredTotalQuadlets) {
        if (completion) {
            completion(true);
        }
        return;
    }

    const uint32_t toRead = requiredTotalQuadlets - have;
    const uint32_t offsetBytes = have * 4u;

    ASFW_LOG_V3(ConfigROM,
                "EnsurePrefix: node=%u have=%u need=%u (read %u quadlets at offsetBytes=%u)",
                nodeId,
                have,
                requiredTotalQuadlets,
                toRead,
                offsetBytes);

    incrementInflight();

    auto callback = [publishEnsurePrefixEvent,
                     nodeId,
                     requiredTotalQuadlets,
                     completion = std::move(completion)](const ROMReader::ReadResult& res) mutable {
        publishEnsurePrefixEvent(nodeId, requiredTotalQuadlets, completion, res);
    };

    reader.ReadRootDirQuadlets(nodeId, currentGen, node.CurrentSpeed(), offsetBytes, toRead, callback);
}

} // namespace ASFW::Discovery
