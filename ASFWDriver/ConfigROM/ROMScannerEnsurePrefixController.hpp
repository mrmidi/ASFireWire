#pragma once

#include <cstdint>
#include <functional>

#include "ROMScanNodeStateMachine.hpp"
#include "ROMReader.hpp"

namespace ASFW::Discovery {

class ROMScannerEnsurePrefixController {
public:
    void EnsurePrefix(uint8_t nodeId,
                      uint32_t requiredTotalQuadlets,
                      Generation currentGen,
                      ROMReader& reader,
                      const std::function<ROMScanNodeStateMachine*(uint8_t)>& findNodeScan,
                      const std::function<void()>& incrementInflight,
                      const std::function<void(uint8_t,
                                               uint32_t,
                                               const std::function<void(bool)>&,
                                               const ROMReader::ReadResult&)>& publishEnsurePrefixEvent,
                      std::function<void(bool)> completion) const;
};

} // namespace ASFW::Discovery
