#pragma once

#include "ROMScanner.hpp"

namespace ASFW::Discovery {

class ROMScannerIRMPhase {
public:
    static void HandleReadCompletion(ROMScanner& scanner,
                                     uint8_t nodeId,
                                     const ROMReader::ReadResult& result);

    static void HandleLockCompletion(ROMScanner& scanner,
                                     uint8_t nodeId,
                                     const ROMReader::ReadResult& result);

    static void HandleLockResult(ROMScanner& scanner,
                                 ROMScanNodeStateMachine& node,
                                 const ROMReader::ReadResult& result);
};

} // namespace ASFW::Discovery