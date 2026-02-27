#pragma once

#include "ROMScanner.hpp"

namespace ASFW::Discovery {

class ROMScannerFSMFlow {
public:
    static void OnEnsurePrefixReadComplete(ROMScanner& scanner,
                                           uint8_t nodeId,
                                           uint32_t requiredTotalQuadlets,
                                           const std::shared_ptr<std::function<void(bool)>>& completion,
                                           const ROMReader::ReadResult& res);

    static void FinishEnsurePrefixStep(ROMScanner& scanner,
                                       const std::shared_ptr<std::function<void(bool)>>& completion,
                                       bool ok);
};

} // namespace ASFW::Discovery
